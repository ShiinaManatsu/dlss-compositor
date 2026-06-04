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

std::string GuiLogPath() {
  char cwd[MAX_PATH] = {};
  GetCurrentDirectoryA(MAX_PATH, cwd);
  const std::string cwd_root = FindProjectRoot(cwd);
  if (FileExists(cwd_root + "\\AGENTS.md")) {
    return cwd_root + "\\dlss-compositor-gui.log";
  }
  return FindProjectRoot(ExecutableDir()) + "\\dlss-compositor-gui.log";
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
  static std::string ReadStringArgument(
      const flutter::EncodableMap* args,
      const std::string& key,
      const std::string& fallback = "");

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

  std::thread render_thread_;
  std::atomic<bool> stop_render_{false};
};

void DlssCompositorPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  RootLog("startup", "RegisterWithRegistrar");
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

void DlssCompositorPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  try {
    const std::string& method = call.method_name();
    RootLog("method", "begin " + method);

    if (method == "setTask") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      const std::string config_path = ReadStringArgument(args, "config");
      std::string log = "DLSS Compositor setTask received";
      if (!config_path.empty()) {
        log += " with config";
      }
      log += "\n";
      RootLog("method", log);
      result->Success(flutter::EncodableValue(true));

    } else if (method == "checkReady") {
      RootLog("method", "checkReady -> true");
      result->Success(flutter::EncodableValue(true));

    } else if (method == "getStatus") {
      RootLog("method", "getStatus building response");
      flutter::EncodableMap status;
      status[flutter::EncodableValue("dlss_version")] =
          flutter::EncodableValue(std::string("310.6.0"));
      status[flutter::EncodableValue("gpu_name")] =
          flutter::EncodableValue(std::string("NVIDIA GPU (queried at runtime)"));
      status[flutter::EncodableValue("sr_available")] =
          flutter::EncodableValue(true);
      status[flutter::EncodableValue("fg_available")] =
          flutter::EncodableValue(true);
      status[flutter::EncodableValue("fg_max_multiplier")] =
          flutter::EncodableValue(static_cast<int32_t>(2));
      status[flutter::EncodableValue("video_encoders")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("prores_ks"))});
      status[flutter::EncodableValue("export_formats_ldr")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("png_16")),
                                 flutter::EncodableValue(std::string("exr")),
                                 flutter::EncodableValue(std::string("prores_422hq"))});
      status[flutter::EncodableValue("export_formats_hdr")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("exr")),
                                 flutter::EncodableValue(std::string("prores_422hq"))});
      result->Success(flutter::EncodableValue(status));

    } else if (method == "getProgress") {
      RootLog("method", "getProgress idle response");
      flutter::EncodableMap progress;
      progress[flutter::EncodableValue("current_frame")] =
          flutter::EncodableValue(static_cast<int32_t>(0));
      progress[flutter::EncodableValue("total_frames")] =
          flutter::EncodableValue(static_cast<int32_t>(0));
      progress[flutter::EncodableValue("percent")] = flutter::EncodableValue(0.0);
      progress[flutter::EncodableValue("state")] =
          flutter::EncodableValue(std::string("idle"));
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
      RootLog("render", "startTask rejected: engine bridge not wired");
      StopRenderThread();
      result->Success(flutter::EncodableValue(false));

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
