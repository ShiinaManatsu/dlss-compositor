// =============================================================================
// dlss/rpc — RpcDispatcher (transport-agnostic)
//
// Receives nlohmann::json requests, returns nlohmann::json responses.
// Used by both MethodChannel (native plugin, transport_native.cpp) and
// JSON-RPC stdio (fallback CLI, transport_stdio.cpp).
//
// Lock-free progress contract: getProgress() must complete in < 10ms.
// =============================================================================
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

namespace dlss {

/// Lock-free progress struct for getProgress (must be < 10ms per read).
/// alignas(64) keeps the struct on its own cache line to prevent false sharing
/// between the render worker thread (writer) and any RPC reader thread.
/// MSVC C4324 (struct padded due to alignas) is intentional here — that is
/// exactly what we want to keep adjacent fields off the same cache line.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4324)
#endif
struct alignas(64) RenderProgress {
    std::atomic<int>   current_frame{0};
    std::atomic<int>   total_frames{0};
    std::atomic<float> percent{0.0f};
    std::atomic<int>   dlss_current_frame{0};
    std::atomic<int>   dlss_total_frames{0};
    std::atomic<float> dlss_percent{0.0f};
    // State: 0=Idle, 1=Running, 2=Done, 3=Error
    std::atomic<int>   state{0};
};
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

/// Transport-agnostic RPC dispatcher.
///
/// Lifetime: one instance per process; both MethodChannel + stdio transports
/// share it. All handlers are reentrant and lock-free with respect to
/// progress; setTask/startTask use a single std::atomic<bool> to gate
/// re-entry.
class RpcDispatcher {
public:
    RpcDispatcher();
    ~RpcDispatcher() = default;

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;

    /// Handle an incoming RPC request. Returns JSON response.
    /// Never throws — errors are encoded into the response.
    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params);

    /// Get current render progress (lock-free, < 10ms).
    const RenderProgress& progress() const { return m_progress; }
    RenderProgress& progress() { return m_progress; }

    /// Push event to registered listener (called from render worker thread).
    using EventCallback = std::function<void(const nlohmann::json&)>;
    void setEventCallback(EventCallback cb) { m_eventCb = std::move(cb); }

private:
    // Handler implementations (one per dlss/* method).
    nlohmann::json handleSetTask(const nlohmann::json& p);
    nlohmann::json handleCheckReady(const nlohmann::json& p);
    nlohmann::json handleCheckOutputCollisions(const nlohmann::json& p);
    nlohmann::json handleStartTask(const nlohmann::json& p);
    nlohmann::json handleGetStatus(const nlohmann::json& p);
    nlohmann::json handleGetTaskState(const nlohmann::json& p);
    nlohmann::json handleGetProgress(const nlohmann::json& p);
    nlohmann::json handleGetPreviewFrame(const nlohmann::json& p);
    nlohmann::json handleSetDebugMode(const nlohmann::json& p);

    RenderProgress m_progress;
    EventCallback  m_eventCb;

    // Stored task config (from setTask).
    nlohmann::json    m_taskConfig;
    std::string       m_currentTaskId;
    std::atomic<bool> m_taskRunning{false};
};

} // namespace dlss
