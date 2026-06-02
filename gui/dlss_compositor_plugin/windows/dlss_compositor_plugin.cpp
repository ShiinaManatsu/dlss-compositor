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
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dlss_compositor_plugin {

constexpr char kMethodChannelName[] = "dlss_compositor/method";
constexpr char kEventChannelName[] = "dlss_compositor/events";

constexpr int kRingBufferSlots = 4;
constexpr size_t kPreviewFrameSize = 8 * 1024 * 1024;

struct PreviewRingBuffer {
  uint8_t slots[kRingBufferSlots][kPreviewFrameSize] = {};
  std::atomic<int> writeSlot{0};
  std::atomic<int> readSlot{-1};
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
  auto plugin = std::make_unique<DlssCompositorPlugin>(registrar);
  registrar->AddPlugin(std::move(plugin));
}

DlssCompositorPlugin::DlssCompositorPlugin(
    flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar), ring_buffer_(std::make_unique<PreviewRingBuffer>()) {
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
            std::lock_guard<std::mutex> lock(event_sink_mutex_);
            event_sink_ = std::move(events);
            return nullptr;
          },
          [this](const flutter::EncodableValue* arguments)
              -> std::unique_ptr<
                  flutter::StreamHandlerError<flutter::EncodableValue>> {
            std::lock_guard<std::mutex> lock(event_sink_mutex_);
            event_sink_ = nullptr;
            return nullptr;
          });

  event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), kEventChannelName,
          &flutter::StandardMethodCodec::GetInstance());
  event_channel_->SetStreamHandler(std::move(event_handler));

  // Windows stable client wrapper does not expose ui_task_runner(); use the
  // registrar's top-level window proc as the platform-thread hop. Background
  // threads only enqueue work and post WM_APP back to the Flutter window.
  window_proc_delegate_id_ = registrar_->RegisterTopLevelWindowProcDelegate(
      [this](HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
          -> std::optional<LRESULT> {
        if (message == WM_APP + 0x3D1) {
          DrainUiThreadTasks();
          return 0;
        }
        return std::nullopt;
      });
}

DlssCompositorPlugin::~DlssCompositorPlugin() {
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
    PostMessage(view->GetNativeWindow(), WM_APP + 0x3D1, 0, 0);
  }
}

void DlssCompositorPlugin::DrainUiThreadTasks() {
  std::deque<std::function<void()>> tasks;
  {
    std::lock_guard<std::mutex> lock(ui_tasks_mutex_);
    tasks.swap(ui_tasks_);
  }
  for (auto& task : tasks) {
    task();
  }
}

void DlssCompositorPlugin::StopRenderThread() {
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

    if (method == "setTask") {
      const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
      const std::string config_path = ReadStringArgument(args, "config");
      std::string log = "DLSS Compositor setTask received";
      if (!config_path.empty()) {
        log += ": " + config_path;
      }
      log += "\n";
      OutputDebugStringA(log.c_str());
      result->Success(flutter::EncodableValue(true));

    } else if (method == "checkReady") {
      result->Success(flutter::EncodableValue(true));

    } else if (method == "getStatus") {
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
          flutter::EncodableValue(static_cast<int32_t>(4));
      status[flutter::EncodableValue("video_encoders")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("prores_ks"))});
      status[flutter::EncodableValue("export_formats")] = flutter::EncodableValue(
          flutter::EncodableList{flutter::EncodableValue(std::string("mov")),
                                 flutter::EncodableValue(std::string("mp4"))});
      result->Success(flutter::EncodableValue(status));

    } else if (method == "getProgress") {
      flutter::EncodableMap progress;
      progress[flutter::EncodableValue("current_frame")] =
          flutter::EncodableValue(static_cast<int32_t>(0));
      progress[flutter::EncodableValue("total_frames")] =
          flutter::EncodableValue(static_cast<int32_t>(0));
      progress[flutter::EncodableValue("percent")] = flutter::EncodableValue(0.0);
      progress[flutter::EncodableValue("state")] =
          flutter::EncodableValue(std::string("idle"));
      result->Success(flutter::EncodableValue(progress));

    } else if (method == "getPreviewFrame") {
      const int slot = ring_buffer_->readSlot.load();
      if (slot >= 0 && slot < kRingBufferSlots) {
        const intptr_t ptr =
            reinterpret_cast<intptr_t>(ring_buffer_->slots[slot]);
        flutter::EncodableMap frame;
        frame[flutter::EncodableValue("ptr")] =
            flutter::EncodableValue(static_cast<int64_t>(ptr));
        frame[flutter::EncodableValue("size")] =
            flutter::EncodableValue(static_cast<int64_t>(kPreviewFrameSize));
        frame[flutter::EncodableValue("width")] =
            flutter::EncodableValue(static_cast<int32_t>(1920));
        frame[flutter::EncodableValue("height")] =
            flutter::EncodableValue(static_cast<int32_t>(1080));
        result->Success(flutter::EncodableValue(frame));
      } else {
        result->Success(flutter::EncodableValue());
      }

    } else if (method == "startTask") {
      StopRenderThread();
      stop_render_.store(false);
      render_thread_ = std::thread([this] {
        for (int i = 0; i <= 10 && !stop_render_.load(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));

          const int slot = ring_buffer_->writeSlot.load();
          ring_buffer_->slots[slot][0] = static_cast<uint8_t>(i);
          ring_buffer_->readSlot.store(slot);
          ring_buffer_->writeSlot.store((slot + 1) % kRingBufferSlots);

          PostToFlutterThread([this, i] {
            std::lock_guard<std::mutex> lock(event_sink_mutex_);
            if (event_sink_) {
              flutter::EncodableMap evt;
              evt[flutter::EncodableValue("type")] =
                  flutter::EncodableValue(std::string("progress"));
              evt[flutter::EncodableValue("current_frame")] =
                  flutter::EncodableValue(static_cast<int32_t>(i));
              evt[flutter::EncodableValue("total_frames")] =
                  flutter::EncodableValue(static_cast<int32_t>(10));
              evt[flutter::EncodableValue("percent")] =
                  flutter::EncodableValue(static_cast<double>(i) / 10.0);
              event_sink_->Success(flutter::EncodableValue(evt));
            }
          });
        }
      });
      result->Success(flutter::EncodableValue(true));

    } else if (method == "setDebugMode") {
      flutter::EncodableMap response;
      response[flutter::EncodableValue("ok")] = flutter::EncodableValue(false);
      response[flutter::EncodableValue("reason")] =
          flutter::EncodableValue(std::string("not implemented"));
      result->Success(flutter::EncodableValue(response));

    } else {
      result->NotImplemented();
    }
  } catch (const std::exception& e) {
    result->Error("native_exception", e.what());
  } catch (...) {
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
