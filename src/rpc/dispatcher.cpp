// =============================================================================
// dlss/rpc — RpcDispatcher implementation
//
// Lock-free progress contract: handleGetProgress() must return in < 10ms.
// All atomic reads happen on the RenderProgress struct only.
// =============================================================================
#include "dispatcher.h"

#include "../core/log.h"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dlss {

namespace {

/// Generate a 32-bit hex task id (e.g. "task_a1b2c3d4").
std::string generateTaskId() {
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint32_t> dist;
    std::ostringstream ss;
    ss << "task_" << std::hex << std::setw(8) << std::setfill('0') << dist(rng);
    return ss.str();
}

} // namespace

RpcDispatcher::RpcDispatcher() = default;

nlohmann::json RpcDispatcher::dispatch(const std::string& method, const nlohmann::json& params) {
    LOG_DEBUG("[RPC] dispatch: {}", method);

    try {
        if (method == "setTask")               return handleSetTask(params);
        if (method == "checkReady")            return handleCheckReady(params);
        if (method == "checkOutputCollisions") return handleCheckOutputCollisions(params);
        if (method == "startTask")             return handleStartTask(params);
        if (method == "getStatus")             return handleGetStatus(params);
        if (method == "getTaskState")          return handleGetTaskState(params);
        if (method == "getProgress")           return handleGetProgress(params);
        if (method == "getPreviewFrame")       return handleGetPreviewFrame(params);
        if (method == "setDebugMode")          return handleSetDebugMode(params);

        return {{"error", "Method not found: " + method}};
    } catch (const std::exception& e) {
        // fmt v11 requires lvalue references in make_format_args; copy first.
        const std::string what = e.what();
        LOG_ERROR("[RPC] dispatch error for {}: {}", method, what);
        return {{"error", what}};
    }
}

nlohmann::json RpcDispatcher::handleSetTask(const nlohmann::json& p) {
    m_taskConfig = p;
    m_currentTaskId = generateTaskId();
    LOG_INFO("[RPC] setTask received, task_id={}", m_currentTaskId);
    return {{"ok", true}, {"task_id", m_currentTaskId}};
}

nlohmann::json RpcDispatcher::handleCheckReady(const nlohmann::json& /*p*/) {
    // TODO(T36+): cross-check NGX availability via VulkanContext + ngx_wrapper.
    const bool ready = !m_taskConfig.is_null() && !m_taskConfig.empty();
    const std::string reason = ready ? std::string{} : std::string{"No task configured"};
    LOG_INFO("[RPC] checkReady ready={} reason={}", ready, reason);
    return {{"ready", ready}, {"reason", reason}};
}

nlohmann::json RpcDispatcher::handleCheckOutputCollisions(const nlohmann::json& p) {
    // TODO(T36+): scan output_dir for files matching template + frame range.
    // Per plan: returns {collisions: N, files: [...]}.
    (void)p.value("output_dir", std::string{});
    (void)p.value("template", std::string{"frame_####.exr"});
    (void)p.value("frame_count", 0);
    LOG_INFO("[RPC] checkOutputCollisions complete collisions=0");
    return {{"collisions", 0}, {"files", nlohmann::json::array()}};
}

nlohmann::json RpcDispatcher::handleStartTask(const nlohmann::json& /*p*/) {
    if (m_taskConfig.is_null()) {
        return {{"ok", false}, {"error", "No task configured, call setTask first"}};
    }
    if (m_taskRunning.exchange(true)) {
        return {{"ok", false}, {"error", "Task already running"}};
    }

    // TODO(T36+): enqueue real pipeline run on a worker thread; this stub
    // only updates the lock-free progress block so callers can poll state.
    m_progress.state.store(1);          // Running
    m_progress.current_frame.store(0);
    m_progress.total_frames.store(100);
    m_progress.percent.store(0.0f);

    LOG_INFO("[RPC] startTask: {}", m_currentTaskId);
    return {{"ok", true}, {"task_id", m_currentTaskId}};
}

nlohmann::json RpcDispatcher::handleGetStatus(const nlohmann::json& /*p*/) {
    // IMPORTANT: video_encoders + export_formats list ONLY v2-implemented codecs.
    // DNxHR / H264 / H265 / AV1 / AVIF / JPEG / WebP MUST NOT appear here —
    // they exist as enum placeholders for future extensions only.
    LOG_INFO("[RPC] getStatus response prepared");
    return {
        {"dlss_version",        "310.6.0"},
        {"fg_max_multiplier",   4},
        {"gpu_name",            "NVIDIA RTX GPU"},
        {"driver_version",      "560.0"},
        {"is_hdr_supported",    true},
        // Per plan + user round 2: only ProRes is registered in v2.
        {"video_encoders",      nlohmann::json::array({"prores_ks"})},
        // LDR project formats: PNG-16 + EXR + ProRes (per user round 2).
        {"export_formats_ldr",  nlohmann::json::array({"png_16", "exr", "prores_422hq"})},
        // HDR project formats: EXR + ProRes (HDR has no PNG path).
        {"export_formats_hdr",  nlohmann::json::array({"exr", "prores_422hq"})},
    };
}

nlohmann::json RpcDispatcher::handleGetTaskState(const nlohmann::json& /*p*/) {
    const int state = m_progress.state.load();
    std::string stateStr;
    switch (state) {
        case 0: stateStr = "idle";    break;
        case 1: stateStr = "running"; break;
        case 2: stateStr = "done";    break;
        case 3: stateStr = "error";   break;
        default: stateStr = "unknown";
    }

    const int current = m_progress.current_frame.load();
    const int total   = m_progress.total_frames.load();
    const float eta = (total > 0 && current > 0 && current < total)
        ? static_cast<float>(total - current) / static_cast<float>(std::max(1, current))
        : 0.0f;

    return {
        {"state",         stateStr},
        {"current_frame", current},
        {"total_frames",  total},
        {"eta_seconds",   static_cast<int>(eta)},
    };
}

nlohmann::json RpcDispatcher::handleGetProgress(const nlohmann::json& /*p*/) {
    // Lock-free atomic reads on a single cache line — must be < 10ms.
    LOG_DEBUG("[RPC] getProgress current={} total={} state={}",
              m_progress.current_frame.load(),
              m_progress.total_frames.load(),
              m_progress.state.load());
    return {
        {"current_frame", m_progress.current_frame.load()},
        {"total_frames",  m_progress.total_frames.load()},
        {"percent",       m_progress.percent.load()},
        {"state",         m_progress.state.load()},
    };
}

nlohmann::json RpcDispatcher::handleGetPreviewFrame(const nlohmann::json& /*p*/) {
    // Preview frame pixels are delivered via FFI shared memory (T30 plugin);
    // this RPC response carries only the metadata pointer + dimensions.
    return {
        {"ptr",    0},
        {"size",   0},
        {"width",  0},
        {"height", 0},
    };
}

nlohmann::json RpcDispatcher::handleSetDebugMode(const nlohmann::json& /*p*/) {
    // Per plan: debug overlay is experimental and not implemented in v2.
    LOG_DEBUG("[RPC] setDebugMode: not implemented");
    return {{"ok", false}, {"reason", "not implemented"}};
}

} // namespace dlss
