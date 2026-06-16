// =============================================================================
// dlss/rpc — Native MethodChannel transport bridge (stub)
//
// In-process bridge between the T30 Flutter plugin's MethodChannel layer and
// the shared RpcDispatcher. The plugin marshals method calls + arguments
// across the platform boundary, then invokes nativeBridge() with the JSON
// payload it has already deserialized.
//
// This translation unit currently provides only a thin shim. The wiring to
// the actual Flutter platform side lives in plugin/native code (T30) and
// will replace these stubs in a follow-up integration task.
// =============================================================================
#include "dispatcher.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>

namespace dlss {

namespace {

/// Process-wide singleton accessed by both transports.
std::mutex                       g_dispatcherMutex;
std::unique_ptr<RpcDispatcher>   g_dispatcher;

RpcDispatcher& ensureDispatcher() {
    std::lock_guard<std::mutex> lk(g_dispatcherMutex);
    if (!g_dispatcher) {
        g_dispatcher = std::make_unique<RpcDispatcher>();
    }
    return *g_dispatcher;
}

} // namespace

/// Plugin-facing entry point. The MethodChannel handler decodes the platform
/// message, then calls this with the method name + parsed JSON args. Returns
/// a JSON response that the plugin will re-encode for the platform reply.
nlohmann::json nativeBridge(const std::string& method, const nlohmann::json& params) {
    return ensureDispatcher().dispatch(method, params);
}

/// Allow the plugin to register an event sink (for getProgress streaming /
/// preview-frame ready notifications via EventChannel).
void nativeBridgeSetEventCallback(RpcDispatcher::EventCallback cb) {
    ensureDispatcher().setEventCallback(std::move(cb));
}

/// Direct accessor for tests / future C-API.
RpcDispatcher& nativeDispatcher() {
    return ensureDispatcher();
}

} // namespace dlss
