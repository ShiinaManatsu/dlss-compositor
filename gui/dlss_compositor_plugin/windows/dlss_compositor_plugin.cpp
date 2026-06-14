// DLSS Compositor Flutter Native Plugin — Windows
// Architecture:
// - MethodChannel "dlss_compositor/method": commands (setTask, checkReady, getStatus, etc.)
// - EventChannel "dlss_compositor/events": progress + events
// - FFI Pointer + shared memory ring buffer: 8MB preview frames (zero-copy)
//
// CRITICAL THREADING: Do NOT call event_sink->Success() from std::thread directly.
// Always post to Flutter UI thread via registrar task runner.

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>
#include <flutter/standard_message_codec.h>

#include <windows.h>

#include "cli/config_executor.h"
#include "cli/preview_ipc.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/vulkan_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace dlss_compositor_plugin {

constexpr char kMethodChannelName[] = "dlss_compositor/method";
constexpr char kEventChannelName[] = "dlss_compositor/events";
constexpr UINT kDrainTasksMessage = WM_APP + 0x3D1;

constexpr int kRingBufferSlots = 4;
constexpr size_t kPreviewFrameSize = 8 * 1024 * 1024;
constexpr uint32_t kPreviewMagic = 0x44505350u;

std::string PreviewSharedMemoryName() {
  char cwd[MAX_PATH] = {};
  GetCurrentDirectoryA(MAX_PATH, cwd);
  std::string suffix = cwd;
  for (char& ch : suffix) {
    const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) ch = '_';
  }
  return "Local\\DLSSCompositorPreview_" + suffix;
}

struct PreviewHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t generation;
  uint32_t width;
  uint32_t height;
  uint32_t byteSize;
  uint32_t reserved;
};

bool FileExists(const std::string& path) {
  const DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ParentPath(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  if (slash == std::string::npos || slash == 0) {
    return "";
  }
  return path.substr(0, slash);
}

std::string FindProjectRoot(std::string dir) {
  while (!dir.empty()) {
    if (FileExists(dir + "\\AGENTS.md") && FileExists(dir + "\\CMakeLists.txt")) {
      return dir;
    }
    const std::string parent = ParentPath(dir);
    if (parent.empty() || parent == dir) {
      break;
    }
    dir = parent;
  }

  char cwd[MAX_PATH] = {};
  GetCurrentDirectoryA(MAX_PATH, cwd);
  return cwd;
}

std::string ExecutableDir() {
  char exe_path[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
  return ParentPath(exe_path);
}

std::string EnvVar(const char* name) {
  char buffer[4096] = {};
  const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (length == 0 || length >= sizeof(buffer)) {
    return "";
  }
  return buffer;
}

std::string GuiLogPath() {
  const std::string logDir = ExecutableDir() + "\\logs";
  CreateDirectoryA(logDir.c_str(), nullptr);
  return logDir + "\\dlss-compositor-gui.log";
}

void RootLog(const std::string& stage, const std::string& message) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> lock(log_mutex);

  SYSTEMTIME now = {};
  GetLocalTime(&now);

  std::ostringstream line;
  line << '[' << now.wYear << '-';
  line.width(2); line.fill('0'); line << now.wMonth << '-';
  line.width(2); line << now.wDay << 'T';
  line.width(2); line << now.wHour << ':';
  line.width(2); line << now.wMinute << ':';
  line.width(2); line << now.wSecond << ".";
  line.width(3); line << now.wMilliseconds << "] [native:" << stage << "] " << message << "\n";
  const std::string text = line.str();

  const std::string path = GuiLogPath();
  HANDLE file = CreateFileA(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  CloseHandle(file);
}

struct PreviewRingBuffer {
  uint8_t slots[kRingBufferSlots][kPreviewFrameSize] = {};
  std::atomic<int> writeSlot{0};
  std::atomic<int> readSlot{-1};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
  std::atomic<uint32_t> byteSize{0};
};

class DlssCompositorPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

  explicit DlssCompositorPlugin(flutter::PluginRegistrarWindows* registrar);
  ~DlssCompositorPlugin() override;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  void PostToFlutterThread(std::function<void()> fn);
  void DrainUiThreadTasks();
  void StopRenderThread();
  bool EnsureRuntimeInitialized();
  void EmitEvent(flutter::EncodableMap event);
  void PublishPreviewRgba8Frame(const std::vector<std::uint8_t>& pixels,
                           std::uint32_t width,
                           std::uint32_t height);
  dlss::ConfigExecutionOptions BuildExecutionOptions() const;
  static std::string ReadStringArgument(
      const flutter::EncodableMap* args,
      const std::string& key,
      const std::string& fallback = "");
  static bool ReadBoolArgument(
      const flutter::EncodableMap* args,
      const std::string& key,
      bool fallback = false);
  static int ReadIntArgument(
      const flutter::EncodableMap* args,
      const std::string& key,
      int fallback = 0);

  flutter::PluginRegistrarWindows* registrar_;
  int window_proc_delegate_id_ = -1;

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>>
      method_channel_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> event_channel_;

  std::mutex event_sink_mutex_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;

  std::mutex ui_tasks_mutex_;
  std::deque<std::function<void()>> ui_tasks_;

  std::unique_ptr<PreviewRingBuffer> ring_buffer_;

  mutable std::mutex task_mutex_;
  flutter::EncodableMap task_params_;
  std::atomic<int> current_frame_{0};
  std::atomic<int> total_frames_{0};
  std::atomic<int> dlss_current_frame_{0};
  std::atomic<int> dlss_total_frames_{0};
  std::atomic<bool> task_running_{false};

  std::mutex runtime_mutex_;
  std::unique_ptr<dlss::VulkanContext> vulkan_;
  std::unique_ptr<dlss::NgxContext> ngx_;
  bool runtime_attempted_ = false;
  bool runtime_ready_ = false;
  std::string runtime_error_;
  std::string runtime_gpu_name_ = "NVIDIA GPU";
  std::string runtime_dlss_version_ = "unknown";
  bool runtime_sr_available_ = false;
  bool runtime_fg_available_ = false;
  int runtime_fg_max_multiplier_ = 0;

  std::thread render_thread_;
  std::atomic<bool> stop_render_{false};
};

void DlssCompositorPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  RootLog("startup", "RegisterWithRegistrar");
  RootLog("startup", "plugin exe_dir=" + ExecutableDir());
  RootLog("startup", "plugin gui_log_path=" + GuiLogPath());
  RootLog("startup", "env DLSS_COMPOSITOR_LOG=" + EnvVar("DLSS_COMPOSITOR_LOG"));
  RootLog("startup", "env DLSS_COMPOSITOR_LOG_DIR=" + EnvVar("DLSS_COMPOSITOR_LOG_DIR"));
  auto plugin = std::make_unique<DlssCompositorPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

DlssCompositorPlugin::DlssCompositorPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar), ring_buffer_(std::make_unique<PreviewRingBuffer>()) {
  RootLog("startup", "Constructing native plugin");
  method_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), kMethodChannelName,
          &flutter::StandardMethodCodec::GetInstance());

  method_channel_->SetMethodCallHandler(
      [this](const auto& call, auto result) {
        HandleMethodCall(call, std::move(result));
      });

  auto event_handler =
      std::make_unique<flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
          [this](const flutter::EncodableValue* arguments,
                 std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&&
                     events)
             -> std::unique_ptr<
                   flutter::StreamHandlerError<flutter::EncodableValue>> {
            RootLog("event", "progress stream listen");
            std::lock_guard<std::mutex> lock(event_sink_mutex_);
            event_sink_ = std::move(events);
            return nullptr;
          },
          [this](const flutter::EncodableValue* arguments)
             -> std::unique_ptr<
                   flutter::StreamHandlerError<flutter::EncodableValue>> {
            RootLog("event", "progress stream cancel");
            std::lock_guard<std::mutex> lock(event_sink_mutex_);
            event_sink_ = nullptr;
            return nullptr;
          });

  event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), kEventChannelName,
          &flutter::StandardMethodCodec::GetInstance());
  event_channel_->SetStreamHandler(std::move(event_handler));
  RootLog("startup", "Native plugin channels ready");

  // Windows stable client wrapper does not expose ui_task_runner(); use the
  // registrar's top-level window proc as the platform-thread hop. Background
  // threads only enqueue work and post WM_APP back to the Flutter window.
  window_proc_delegate_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
      [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
          -> std::optional<LRESULT> {
        if (message == kDrainTasksMessage) {
          DrainUiThreadTasks();
          return 0;
        }
        return std::nullopt;
      });
}

DlssCompositorPlugin::~DlssCompositorPlugin() {
  RootLog("shutdown", "Destroying native plugin");
  StopRenderThread();
  {
    std::lock_guard<std::mutex> lock(runtime_mutex_);
    ngx_.reset();
    vulkan_.reset();
    runtime_ready_ = false;
  }
  if (window_proc_delegate_id_ >= 0) {
    registrar_->UnregisterTopLevelWindowProcDelegate(window_proc_delegate_id_);
  }
}

void DlssCompositorPlugin::PostToFlutterThread(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(ui_tasks_mutex_);
    ui_tasks_.push_back(std::move(fn));
  }
  if (auto* view = registrar_->GetView()) {
    HWND target = GetAncestor(view->GetNativeWindow(), GA_ROOT);
    if (target == nullptr) {
      target = view->GetNativeWindow();
    }
    if (PostMessage(target, kDrainTasksMessage, 0, 0) == 0) {
      RootLog("event", "failed to post UI-thread drain message");
    }
  } else {
    RootLog("event", "cannot post UI-thread drain message without view");
  }
}

void DlssCompositorPlugin::DrainUiThreadTasks() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(ui_tasks_mutex_);
    tasks.swap(ui_tasks_);
  }
  if (!tasks.empty()) {
    RootLog("event", "draining UI-thread tasks");
  }
  for (auto& task : tasks) {
    task();
  }
}

void DlssCompositorPlugin::StopRenderThread() {
  RootLog("render", "StopRenderThread requested");
  stop_render_.store(true);
  if (render_thread_.joinable()) {
    render_thread_.join();
  }
  task_running_.store(false);
}

bool DlssCompositorPlugin::EnsureRuntimeInitialized() {
  std::lock_guard<std::mutex> lock(runtime_mutex_);
  if (runtime_ready_) {
    return true;
  }
  runtime_attempted_ = true;
  runtime_error_.clear();
  try {
    RootLog("runtime", "initializing Vulkan/NGX runtime");
    vulkan_ = std::make_unique<dlss::VulkanContext>();
    ngx_ = std::make_unique<dlss::NgxContext>(
        vulkan_->instance(), vulkan_->physicalDevice(), vulkan_->device());
    runtime_gpu_name_ = vulkan_->deviceProperties().deviceName;
    const auto versions = ngx_->getDllVersions();
    runtime_dlss_version_ = versions.sr;
    runtime_sr_available_ = ngx_->isSrAvailable();
    runtime_fg_available_ = ngx_->isFgAvailable();
    runtime_fg_max_multiplier_ = ngx_->getFgMaxMultiplier();
    runtime_ready_ = true;
    RootLog("runtime", "runtime ready");
    return true;
  } catch (const std::exception& e) {
    runtime_error_ = e.what();
    RootLog("runtime", std::string("runtime init failed: ") + runtime_error_);
  } catch (...) {
    runtime_error_ = "non-standard exception";
    RootLog("runtime", "runtime init failed: non-standard exception");
  }
  ngx_.reset();
  vulkan_.reset();
  runtime_ready_ = false;
  runtime_sr_available_ = false;
  runtime_fg_available_ = false;
  runtime_fg_max_multiplier_ = 0;
  return false;
}

void DlssCompositorPlugin::EmitEvent(flutter::EncodableMap event) {
  PostToFlutterThread([this, event = std::move(event)]() mutable {
    std::lock_guard<std::mutex> lock(event_sink_mutex_);
    if (event_sink_) {
      event_sink_->Success(flutter::EncodableValue(event));
    }
  });
}

void DlssCompositorPlugin::PublishPreviewRgba8Frame(
    const std::vector<std::uint8_t>& pixels,
    std::uint32_t width,
    std::uint32_t height) {
  if (pixels.empty() || width == 0 || height == 0) {
    return;
  }
  const size_t byteSize = (pixels.size() < kPreviewFrameSize) ? pixels.size() : kPreviewFrameSize;
  const int slot = ring_buffer_->writeSlot.fetch_add(1) % kRingBufferSlots;
  std::memcpy(ring_buffer_->slots[slot], pixels.data(), byteSize);
  ring_buffer_->width.store(width);
  ring_buffer_->height.store(height);
  ring_buffer_->byteSize.store(static_cast<uint32_t>(byteSize));
  ring_buffer_->readSlot.store(slot);
}

std::string DlssCompositorPlugin::ReadStringArgument(
    const flutter::EncodableMap* args,
    const std::string& key,
    const std::string& fallback) {
  if (!args) {
    return fallback;
  }

  const auto it = args->find(flutter::EncodableValue(key));
  if (it == args->end()) {
    return fallback;
  }

  if (const auto* value = std::get_if<std::string>(&it->second)) {
    return *value;
  }
  return fallback;
}

bool DlssCompositorPlugin::ReadBoolArgument(
    const flutter::EncodableMap* args,
    const std::string& key,
    bool fallback) {
  if (!args) {
    return fallback;
  }
  const auto it = args->find(flutter::EncodableValue(key));
  if (it == args->end()) {
    return fallback;
  }
  if (const auto* value = std::get_if<bool>(&it->second)) {
    return *value;
  }
  return fallback;
}

int DlssCompositorPlugin::ReadIntArgument(
    const flutter::EncodableMap* args,
    const std::string& key,
    int fallback) {
  if (!args) {
    return fallback;
  }
  const auto it = args->find(flutter::EncodableValue(key));
  if (it == args->end()) {
    return fallback;
  }
  if (const auto* value = std::get_if<int32_t>(&it->second)) {
    return *value;
  }
  if (const auto* value = std::get_if<int64_t>(&it->second)) {
    return static_cast<int>(*value);
  }
  return fallback;
}

dlss::ConfigExecutionOptions DlssCompositorPlugin::BuildExecutionOptions() const {
  flutter::EncodableMap params;
  {
    std::lock_guard<std::mutex> lock(task_mutex_);
    params = task_params_;
  }
  const auto* args = &params;
  dlss::ConfigExecutionOptions options;
  options.configPath = ReadStringArgument(args, "config");
  options.outputDir = ReadStringArgument(args, "output_dir");
  options.outputTemplate = ReadStringArgument(
      args, "output_template", ReadStringArgument(args, "template"));
  options.outputFormat = ReadStringArgument(args, "output_format", "exr");
  options.overwriteMode = ReadStringArgument(args, "overwrite_mode", "overwrite");
  options.pngQualityPercent = ReadIntArgument(args, "png_quality", 97);
  options.exrCompression = ReadStringArgument(args, "exr_compression", "zip");
  options.exrCompressionLevelPercent = ReadIntArgument(args, "exr_compression_level", 97);
  options.fgEnabled = ReadBoolArgument(args, "fg_enabled", false);
  options.fgMultiplier = ReadIntArgument(args, "fg_multiplier", 2);
  options.emitEvents = false;
  return options;
}

void DlssCompositorPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  try {
    const std::string& method = call.method_name();
    RootLog("method", "begin " + method);

    if (method == "setTask") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      const std::string config_path = ReadStringArgument(args, "config");
      if (config_path.empty()) {
        result->Success(flutter::EncodableValue(false));
        return;
      }
      {
        std::lock_guard<std::mutex> lock(task_mutex_);
        task_params_ = args ? *args : flutter::EncodableMap{};
      }
      std::string log = "DLSS Compositor setTask received";
      if (!config_path.empty()) {
        log += " with config";
      }
      log += "\n";
      RootLog("method", log);
      result->Success(flutter::EncodableValue(true));

    } else if (method == "checkReady") {
      const bool ready = EnsureRuntimeInitialized();
      RootLog("method", std::string("checkReady -> ") + (ready ? "true" : "false"));
      result->Success(flutter::EncodableValue(ready));

    } else if (method == "getStatus") {
      RootLog("method", "getStatus building response");
      const bool ready = EnsureRuntimeInitialized();
      std::lock_guard<std::mutex> runtimeLock(runtime_mutex_);
      RootLog("method", std::string("getStatus using resident runtime ready=") + (ready ? "true" : "false"));
      flutter::EncodableMap status;
      status[flutter::EncodableValue("dlss_version")] =
          flutter::EncodableValue(runtime_dlss_version_);
      status[flutter::EncodableValue("gpu_name")] =
          flutter::EncodableValue(runtime_gpu_name_);
      status[flutter::EncodableValue("sr_available")] =
          flutter::EncodableValue(runtime_sr_available_);
      status[flutter::EncodableValue("fg_available")] =
          flutter::EncodableValue(runtime_fg_available_);
      status[flutter::EncodableValue("fg_max_multiplier")] =
          flutter::EncodableValue(static_cast<int32_t>(runtime_fg_max_multiplier_));
      status[flutter::EncodableValue("runtime_ready")] =
          flutter::EncodableValue(runtime_ready_);
      status[flutter::EncodableValue("runtime_error")] =
          flutter::EncodableValue(runtime_error_);
      status[flutter::EncodableValue("video_encoders")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("prores_422hq"))});
      status[flutter::EncodableValue("export_formats_ldr")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("png_16")),
                                 flutter::EncodableValue(std::string("exr")),
                                 flutter::EncodableValue(std::string("prores_422hq"))});
      status[flutter::EncodableValue("export_formats_hdr")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("exr")),
                                 flutter::EncodableValue(std::string("prores_422hq"))});
      result->Success(flutter::EncodableValue(status));

    } else if (method == "getProgress") {
      RootLog("method", "getProgress response");
      flutter::EncodableMap progress;
      progress[flutter::EncodableValue("current_frame")] =
          flutter::EncodableValue(static_cast<int32_t>(current_frame_.load()));
      progress[flutter::EncodableValue("total_frames")] =
          flutter::EncodableValue(static_cast<int32_t>(total_frames_.load()));
      const int total = total_frames_.load();
      progress[flutter::EncodableValue("percent")] = flutter::EncodableValue(
          total > 0 ? static_cast<double>(current_frame_.load()) / static_cast<double>(total) : 0.0);
      progress[flutter::EncodableValue("dlss_current_frame")] =
          flutter::EncodableValue(static_cast<int32_t>(dlss_current_frame_.load()));
      progress[flutter::EncodableValue("dlss_total_frames")] =
          flutter::EncodableValue(static_cast<int32_t>(dlss_total_frames_.load()));
      const int dlssTotal = dlss_total_frames_.load();
      progress[flutter::EncodableValue("dlss_percent")] = flutter::EncodableValue(
          dlssTotal > 0 ? static_cast<double>(dlss_current_frame_.load()) / static_cast<double>(dlssTotal) : 0.0);
      progress[flutter::EncodableValue("state")] =
          flutter::EncodableValue(std::string(task_running_.load() ? "running" : "idle"));
      result->Success(flutter::EncodableValue(progress));

    } else if (method == "checkOutputCollisions") {
      RootLog("method", "checkOutputCollisions stub response");
      flutter::EncodableMap response;
      response[flutter::EncodableValue("collisions")] =
          flutter::EncodableValue(static_cast<int32_t>(0));
      response[flutter::EncodableValue("files")] =
          flutter::EncodableValue(flutter::EncodableList{});
      result->Success(flutter::EncodableValue(response));

    } else if (method == "getPreviewFrame") {
      const std::string shmName = PreviewSharedMemoryName();
      HANDLE mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, shmName.c_str());
      if (mapping) {
        const size_t totalSize = sizeof(PreviewHeader) + kPreviewFrameSize;
        void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, totalSize);
        if (view) {
          const auto* header = static_cast<const PreviewHeader*>(view);
          const size_t byteSize = header->byteSize;
          if (header->magic == kPreviewMagic && header->width > 0 && header->height > 0 &&
              byteSize > 0 && byteSize <= kPreviewFrameSize) {
            const int slot = ring_buffer_->writeSlot.fetch_add(1) % kRingBufferSlots;
            std::memcpy(ring_buffer_->slots[slot],
                        static_cast<const uint8_t*>(view) + sizeof(PreviewHeader),
                        byteSize);
            ring_buffer_->width.store(header->width);
            ring_buffer_->height.store(header->height);
            ring_buffer_->byteSize.store(header->byteSize);
            ring_buffer_->readSlot.store(slot);
          }
          UnmapViewOfFile(view);
        }
        CloseHandle(mapping);
      }

      const int slot = ring_buffer_->readSlot.load();
      if (slot >= 0 && slot < kRingBufferSlots) {
        const intptr_t ptr = reinterpret_cast<intptr_t>(ring_buffer_->slots[slot]);
        flutter::EncodableMap frame;
        frame[flutter::EncodableValue("ptr")] =
            flutter::EncodableValue(static_cast<int64_t>(ptr));
        frame[flutter::EncodableValue("size")] =
            flutter::EncodableValue(static_cast<int64_t>(ring_buffer_->byteSize.load()));
        frame[flutter::EncodableValue("width")] =
            flutter::EncodableValue(static_cast<int32_t>(ring_buffer_->width.load()));
        frame[flutter::EncodableValue("height")] =
            flutter::EncodableValue(static_cast<int32_t>(ring_buffer_->height.load()));
        result->Success(flutter::EncodableValue(frame));
        RootLog("preview", "getPreviewFrame returned slot");
      } else {
        result->Success(flutter::EncodableValue());
        RootLog("preview", "getPreviewFrame returned null");
      }

    } else if (method == "startTask") {
      if (task_running_.load()) {
        RootLog("render", "startTask rejected: already running");
        result->Success(flutter::EncodableValue(false));
        return;
      }
      dlss::ConfigExecutionOptions options = BuildExecutionOptions();
      if (options.configPath.empty()) {
        RootLog("render", "startTask rejected: missing config");
        result->Success(flutter::EncodableValue(false));
        return;
      }
      StopRenderThread();
      stop_render_.store(false);
      task_running_.store(true);
      current_frame_.store(0);
      total_frames_.store(0);
      dlss_current_frame_.store(0);
      dlss_total_frames_.store(0);
      render_thread_ = std::thread([this, options = std::move(options)]() mutable {
        RootLog("render", "native engine worker started");
        try {
          options.progressCallback = [this](int current, int total, int dlssCurrent, int dlssTotal) {
            current_frame_.store(current);
            total_frames_.store(total);
            dlss_current_frame_.store(dlssCurrent);
            dlss_total_frames_.store(dlssTotal);
          };
          options.previewRgba8Callback = [this](const std::vector<std::uint8_t>& pixels,
                                                std::uint32_t width,
                                                std::uint32_t height) {
            PublishPreviewRgba8Frame(pixels, width, height);
          };
          options.cancelRequested = [this]() {
            return stop_render_.load();
          };
          if (!EnsureRuntimeInitialized()) {
            throw std::runtime_error(runtime_error_.empty() ? "DLSS runtime unavailable" : runtime_error_);
          }
          const int exitCode = dlss::executeConfig(options, *vulkan_, *ngx_);
          task_running_.store(false);
          auto addProgressFields = [this](flutter::EncodableMap& event) {
            event[flutter::EncodableValue("current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(current_frame_.load()));
            event[flutter::EncodableValue("total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(total_frames_.load()));
            event[flutter::EncodableValue("dlss_current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_current_frame_.load()));
            event[flutter::EncodableValue("dlss_total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_total_frames_.load()));
          };
          flutter::EncodableMap event;
          if (stop_render_.load()) {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("cancelled"));
          } else if (exitCode == 0) {
            const int total = total_frames_.load();
            if (total > 0) {
              current_frame_.store(total);
            }
            const int dlssTotal = dlss_total_frames_.load();
            if (dlssTotal > 0) {
              dlss_current_frame_.store(dlssTotal);
            }
            flutter::EncodableMap writeComplete;
            writeComplete[flutter::EncodableValue("type")] =
                flutter::EncodableValue(std::string("write_complete"));
            addProgressFields(writeComplete);
            EmitEvent(std::move(writeComplete));
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("done"));
          } else {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("error"));
            event[flutter::EncodableValue("message")] = flutter::EncodableValue(
                std::string("Native engine exited with code ") + std::to_string(exitCode));
          }
          addProgressFields(event);
          EmitEvent(std::move(event));
        } catch (const std::exception& e) {
          task_running_.store(false);
          RootLog("render", std::string("native engine exception: ") + e.what());
          auto addProgressFields = [this](flutter::EncodableMap& event) {
            event[flutter::EncodableValue("current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(current_frame_.load()));
            event[flutter::EncodableValue("total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(total_frames_.load()));
            event[flutter::EncodableValue("dlss_current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_current_frame_.load()));
            event[flutter::EncodableValue("dlss_total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_total_frames_.load()));
          };
          flutter::EncodableMap event;
          if (stop_render_.load()) {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("cancelled"));
          } else {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("error"));
            event[flutter::EncodableValue("message")] = flutter::EncodableValue(std::string(e.what()));
          }
          addProgressFields(event);
          EmitEvent(std::move(event));
        } catch (...) {
          task_running_.store(false);
          RootLog("render", "native engine exception: non-standard exception");
          auto addProgressFields = [this](flutter::EncodableMap& event) {
            event[flutter::EncodableValue("current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(current_frame_.load()));
            event[flutter::EncodableValue("total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(total_frames_.load()));
            event[flutter::EncodableValue("dlss_current_frame")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_current_frame_.load()));
            event[flutter::EncodableValue("dlss_total_frames")] =
                flutter::EncodableValue(static_cast<int32_t>(dlss_total_frames_.load()));
          };
          flutter::EncodableMap event;
          if (stop_render_.load()) {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("cancelled"));
          } else {
            event[flutter::EncodableValue("type")] = flutter::EncodableValue(std::string("error"));
            event[flutter::EncodableValue("message")] = flutter::EncodableValue(std::string("Native engine aborted with a non-standard exception"));
          }
          addProgressFields(event);
          EmitEvent(std::move(event));
        }
        RootLog("render", "native engine worker finished");
      });
      result->Success(flutter::EncodableValue(true));

    } else if (method == "cancelTask") {
      RootLog("render", "cancelTask requested");
      stop_render_.store(true);
      result->Success(flutter::EncodableValue(true));

    } else if (method == "setDebugMode") {
      RootLog("method", "setDebugMode not implemented");
      flutter::EncodableMap response;
      response[flutter::EncodableValue("ok")] = flutter::EncodableValue(false);
      response[flutter::EncodableValue("reason")] =
          flutter::EncodableValue(std::string("not implemented"));
      result->Success(flutter::EncodableValue(response));

    } else {
      RootLog("method", "not implemented " + method);
      result->NotImplemented();
    }
  } catch (const std::exception& e) {
    RootLog("error", e.what());
    result->Error("native_exception", e.what());
  } catch (...) {
    RootLog("error", "Unknown native plugin exception");
    result->Error("native_exception", "Unknown native plugin exception");
  }
}

}  // namespace dlss_compositor_plugin

extern "C" void DlssCompositorPluginRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  dlss_compositor_plugin::DlssCompositorPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
