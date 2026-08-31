// =============================================================================
// dlss/rpc — RpcDispatcher (transport-agnostic)
//
// Receives nlohmann::json requests, returns nlohmann::json responses.
// Used by both MethodChannel (native plugin, transport_native.cpp) and
// JSON-RPC stdio (fallback CLI, transport_stdio.cpp).
//
// Pipeline ownership:
//   Call bindGpu(ctx, ngx) once after Vulkan/NGX are initialised.
//   start_job → builds PipelineConfig, runs Pipeline::run on m_worker thread.
//   progress   → lock-free atomic reads.
//   stop        → Pipeline::stop() + worker join-request.
//   preview_frame → last JPEG bytes (base64-encoded in response).
//
// Lock-free progress contract: getProgress() must complete in < 10ms.
// =============================================================================
#pragma once

#include "../pipeline/pipeline.h"
#include "../pipeline/nr_tool_session.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace dlss {

class VulkanContext;
class NgxContext;

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
/// share it.  Call bindGpu() after Vulkan/NGX boot.  All handlers are
/// reentrant and lock-free with respect to progress; start_job uses a single
/// std::atomic<bool> to gate re-entry.
class RpcDispatcher {
public:
    RpcDispatcher();
    ~RpcDispatcher();

    RpcDispatcher(const RpcDispatcher&) = delete;
    RpcDispatcher& operator=(const RpcDispatcher&) = delete;

    /// Provide GPU resources.  Must be called before start_job is accepted.
    void bindGpu(VulkanContext* ctx, NgxContext* ngx);

    /// Handle an incoming RPC request. Returns JSON response.
    /// Never throws — errors are encoded into the response.
    nlohmann::json dispatch(const std::string& method, const nlohmann::json& params);

    /// Get current render progress (lock-free, < 10ms).
    const RenderProgress& progress() const { return m_progress; }
    RenderProgress& progress() { return m_progress; }

    /// Last engine error detail (shown in the GUI's export_error message).
    void setProgressError(const std::string& what) {
        std::lock_guard<std::mutex> lk(m_lastErrorMu);
        m_lastError = what;
    }
    std::string progressError() const {
        std::lock_guard<std::mutex> lk(m_lastErrorMu);
        return m_lastError;
    }

    /// Push event to registered listener (called from render worker thread).
    using EventCallback = std::function<void(const nlohmann::json&)>;
    void setEventCallback(EventCallback cb) { m_eventCb = std::move(cb); }

private:
    // ── Handler implementations ──────────────────────────────────────────────
    // Legacy two-step API (kept for MethodChannel compat)
    nlohmann::json handleSetTask(const nlohmann::json& p);
    nlohmann::json handleCheckReady(const nlohmann::json& p);
    nlohmann::json handleCheckOutputCollisions(const nlohmann::json& p);
    nlohmann::json handleStartTask(const nlohmann::json& p);
    // One-shot API used by run.py / Electron
    nlohmann::json handleStartJob(const nlohmann::json& p);
    nlohmann::json handleStop(const nlohmann::json& p);
    nlohmann::json handleProgress(const nlohmann::json& p);
    nlohmann::json handlePreviewFrame(const nlohmann::json& p);
    // NR tool (standalone DLSSNR compositor pass)
    nlohmann::json handleNrToolOpen(const nlohmann::json& p);
    nlohmann::json handleNrToolPreview(const nlohmann::json& p);
    nlohmann::json handleNrToolExport(const nlohmann::json& p);
    nlohmann::json handleNrToolClose(const nlohmann::json& p);
    // Status / debug
    nlohmann::json handleGetStatus(const nlohmann::json& p);
    nlohmann::json handleGetTaskState(const nlohmann::json& p);
    nlohmann::json handleGetProgress(const nlohmann::json& p);
    nlohmann::json handleGetPreviewFrame(const nlohmann::json& p);
    nlohmann::json handleSetDebugMode(const nlohmann::json& p);

    // ── Helpers ──────────────────────────────────────────────────────────────
    /// Build PipelineConfig from a start_job / startTask JSON params object.
    /// Throws std::runtime_error on bad params.
    PipelineConfig buildPipelineConfig(const nlohmann::json& p);

    /// Launch Pipeline::run on the worker thread.  Returns task_id.
    /// Throws if a job is already running or GPU not bound.
    std::string launchPipeline(PipelineConfig cfg);

    // ── State ─────────────────────────────────────────────────────────────────
    RenderProgress m_progress;
    EventCallback  m_eventCb;

    /// Engine error detail from the worker catch (progress RPC carries it).
    mutable std::mutex m_lastErrorMu;
    std::string        m_lastError;

    VulkanContext* m_ctx = nullptr;
    NgxContext*    m_ngx = nullptr;

    // Worker thread running Pipeline::run.
    std::unique_ptr<Pipeline>   m_pipeline;
    std::thread                 m_worker;
    std::atomic<bool>           m_jobRunning{false};

    // NR tool session (open/preview synchronous; export on its own worker).
    std::unique_ptr<NrToolSession> m_nrTool;
    std::thread                    m_nrExportWorker;
    std::atomic<bool>              m_nrExportRunning{false};
    std::atomic<bool>              m_nrExportStop{false};

    /// Parse the shared nr_* tuning keys (start_job and nrtool_* alike).
    static NrSettings parseNrSettings(const nlohmann::json& p);

    // Last preview JPEG delivered by the pipeline callback (may be empty).
    std::mutex              m_previewMu;
    std::vector<uint8_t>    m_previewJpeg;
    std::atomic<uint32_t>   m_previewW{0};
    std::atomic<uint32_t>   m_previewH{0};

    // Stored task config (from legacy setTask call).
    nlohmann::json    m_taskConfig;
    std::string       m_currentTaskId;
    std::atomic<bool> m_taskRunning{false};
};

/// Declaration only — definition in transport_stdio.cpp.
void runJsonRpcStdio(RpcDispatcher& dispatcher);

} // namespace dlss
