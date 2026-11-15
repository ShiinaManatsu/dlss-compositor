// =============================================================================
// dlss/rpc — RpcDispatcher implementation
//
// Lock-free progress contract: handleGetProgress() / handleProgress() must
// return in < 10ms.  All atomic reads happen on the RenderProgress struct only.
// =============================================================================
#include "dispatcher.h"

#include "../core/dlssconfig.h"
#include "../core/log.h"
#include "../core/path_encoding.h"
#include "../dlss/ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../io/encoder_registry.h"
#include "../io/mov_concat.h"

#include <algorithm>
#include <cctype>
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

/// Parse an int or "2" / "2x" / "off" JSON value. 0 = missing / FG off.
int parseIntLike(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned>());
    if (!v.is_string()) return 0;
    std::string s = v.get<std::string>();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.empty() || s == "off" || s == "none" || s == "false") return 0;
    if (s.back() == 'x') s.pop_back();
    if (s.empty()) return 0;
    try { return std::stoi(s); } catch (...) { return 0; }
}

int parseFgMultiplier(const nlohmann::json& p) {
    if (p.contains("fg_multiplier") && !p["fg_multiplier"].is_null()) {
        const int n = parseIntLike(p["fg_multiplier"]);
        if (n != 0) return n;
    }
    if (p.contains("fg") && !p["fg"].is_null())
        return parseIntLike(p["fg"]);
    return 0;
}

/// Parse "sr"/"rr"/"rr+fg"/"sr+fg" → PipelineFeatures.
PipelineFeatures parseFeaturesFromMode(const std::string& mode, bool fg) {
    const std::string m = [&]{
        std::string s = mode;
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    PipelineFeatures f = 0;
    if (m == "rr")       f = kRR;
    else if (m == "sr")  f = kSR;
    else if (m == "rr+fg" || m == "rrfg") f = kRR | kFG;
    else if (m == "sr+fg" || m == "srfg") f = kSR | kFG;
    else                 f = kSR;   // default

    if (fg && !(f & kFG)) f |= kFG;
    return f;
}

/// Map JSON "output_format" string → OutputFormat enum.
OutputFormat parseOutputFormat(const std::string& s) {
    if (s == "exr" || s == "exr_linear") return OutputFormat::ExrLinear;
    if (s == "png_16" || s == "png")     return OutputFormat::Png16;
    if (s == "prores_422hq" || s == "prores") return OutputFormat::ProRes422HQ;
    if (s == "h265_hevc" || s == "h265" || s == "hevc") return OutputFormat::H265Mp4;
    return OutputFormat::ProRes422HQ;  // default
}

/// Map JSON "overwrite_mode" string → OverwriteMode enum.
OverwriteMode parseOverwriteMode(const std::string& s) {
    if (s == "skip")        return OverwriteMode::Skip;
    if (s == "autoVersion") return OverwriteMode::AutoVersion;
    return OverwriteMode::Overwrite;
}

} // namespace

// =============================================================================
// RpcDispatcher ctor / dtor
// =============================================================================

RpcDispatcher::RpcDispatcher() = default;

RpcDispatcher::~RpcDispatcher() {
    // Stop any running pipeline and wait for the worker.
    if (m_pipeline) m_pipeline->stop();
    if (m_worker.joinable()) m_worker.join();
    // NR tool export worker: request cancel then join.
    m_nrExportStop.store(true, std::memory_order_release);
    if (m_nrExportWorker.joinable()) m_nrExportWorker.join();
    m_nrTool.reset();
}

void RpcDispatcher::bindGpu(VulkanContext* ctx, NgxContext* ngx) {
    m_ctx = ctx;
    m_ngx = ngx;
}

// =============================================================================
// dispatch
// =============================================================================

nlohmann::json RpcDispatcher::dispatch(const std::string& method, const nlohmann::json& params) {
    LOG_DEBUG("[RPC] dispatch: {}", method);

    try {
        // One-shot API (used by run.py / Electron)
        if (method == "start_job")          return handleStartJob(params);
        if (method == "stop")               return handleStop(params);
        if (method == "progress")           return handleProgress(params);
        if (method == "preview_frame")      return handlePreviewFrame(params);

        // NR tool (standalone DLSSNR compositor pass)
        if (method == "nrtool_open")        return handleNrToolOpen(params);
        if (method == "nrtool_preview")     return handleNrToolPreview(params);
        if (method == "nrtool_export")      return handleNrToolExport(params);
        if (method == "nrtool_close")       return handleNrToolClose(params);
        if (method == "mov_concat")         return handleMovConcat(params);

        // Legacy two-step API (MethodChannel compat)
        if (method == "setTask")               return handleSetTask(params);
        if (method == "checkReady")            return handleCheckReady(params);
        if (method == "checkOutputCollisions") return handleCheckOutputCollisions(params);
        if (method == "startTask")             return handleStartTask(params);

        // Status / debug
        if (method == "getStatus")             return handleGetStatus(params);
        if (method == "getTaskState")          return handleGetTaskState(params);
        if (method == "getProgress")           return handleGetProgress(params);
        if (method == "getPreviewFrame")       return handleGetPreviewFrame(params);
        if (method == "setDebugMode")          return handleSetDebugMode(params);

        return {{"error", "Method not found: " + method}};
    } catch (const std::exception& e) {
        const std::string what = e.what();
        LOG_ERROR("[RPC] dispatch error for {}: {}", method, what);
        return {{"error", what}};
    }
}

// =============================================================================
// One-shot API
// =============================================================================

nlohmann::json RpcDispatcher::handleStartJob(const nlohmann::json& p) {
    if (!m_ctx || !m_ngx) {
        return {{"ok", false}, {"error", "GPU not initialised — call --rpc-stdio after Vulkan boot"}};
    }
    if (m_jobRunning.load(std::memory_order_acquire)) {
        return {{"ok", false}, {"error", "A job is already running; call stop first"}};
    }
    if (m_nrExportRunning.load(std::memory_order_acquire)) {
        return {{"ok", false}, {"error", "An NR tool export is running; stop it first"}};
    }

    PipelineConfig cfg;
    try {
        cfg = buildPipelineConfig(p);
    } catch (const std::exception& e) {
        const std::string what = e.what();
        LOG_ERROR("[RPC] start_job bad params: {}", what);
        return {{"ok", false}, {"error", what}};
    }

    std::string taskId;
    try {
        taskId = launchPipeline(std::move(cfg));
    } catch (const std::exception& e) {
        const std::string what = e.what();
        LOG_ERROR("[RPC] start_job launch failed: {}", what);
        return {{"ok", false}, {"error", what}};
    }

    return {{"ok", true}, {"task_id", taskId}};
}

nlohmann::json RpcDispatcher::handleStop(const nlohmann::json& /*p*/) {
    // NR tool export and pipeline jobs are mutually exclusive; cancel whichever runs.
    if (m_nrExportRunning.load(std::memory_order_acquire)) {
        m_nrExportStop.store(true, std::memory_order_release);
        LOG_INFO("[RPC] stop: requested NR tool export stop");
        return {{"ok", true}, {"nr_export_stop", true}};
    }
    if (m_pipeline) {
        m_pipeline->stop();
        LOG_INFO("[RPC] stop: requested pipeline stop");
    }
    return {{"ok", true}};
}

nlohmann::json RpcDispatcher::handleProgress(const nlohmann::json& /*p*/) {
    // Identical to getProgress — single lock-free read path used by run.py.
    nlohmann::json out = {
        {"current_frame",      m_progress.current_frame.load()},
        {"total_frames",       m_progress.total_frames.load()},
        {"percent",            m_progress.percent.load()},
        {"phase",              m_progress.phase.load()},  // 0=render 1=finalize
        {"dlss_current_frame", m_progress.dlss_current_frame.load()},
        {"dlss_total_frames",  m_progress.dlss_total_frames.load()},
        {"dlss_percent",       m_progress.dlss_percent.load()},
        {"temporal_resets",    m_progress.temporal_resets.load()},
        {"state",              m_progress.state.load()},
        {"job_running",        m_jobRunning.load(std::memory_order_acquire)},
    };
    const std::string lastError = progressError();
    if (!lastError.empty()) {
        out["error"] = lastError;
    }
    return out;
}

nlohmann::json RpcDispatcher::handlePreviewFrame(const nlohmann::json& /*p*/) {
    std::vector<uint8_t> jpeg;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lk(m_previewMu);
        jpeg = m_previewJpeg;
        w = m_previewW.load();
        h = m_previewH.load();
    }
    if (jpeg.empty()) {
        return {{"ok", false}, {"reason", "no preview available yet"}};
    }
    // Base64-encode the JPEG bytes for JSON transport.
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve((jpeg.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < jpeg.size(); i += 3) {
        const uint32_t a = jpeg[i];
        const uint32_t b = (i + 1 < jpeg.size()) ? jpeg[i + 1] : 0u;
        const uint32_t c = (i + 2 < jpeg.size()) ? jpeg[i + 2] : 0u;
        const uint32_t v = (a << 16) | (b << 8) | c;
        b64 += kB64[(v >> 18) & 0x3F];
        b64 += kB64[(v >> 12) & 0x3F];
        b64 += (i + 1 < jpeg.size()) ? kB64[(v >>  6) & 0x3F] : '=';
        b64 += (i + 2 < jpeg.size()) ? kB64[ v        & 0x3F] : '=';
    }
    return {{"ok", true}, {"jpeg_b64", b64}, {"width", w}, {"height", h}};
}

// =============================================================================
// NR tool (standalone DLSSNR compositor pass)
// =============================================================================

namespace {

std::string b64Encode(const std::vector<uint8_t>& jpeg) {
    static constexpr char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve((jpeg.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < jpeg.size(); i += 3) {
        const uint32_t a = jpeg[i];
        const uint32_t b = (i + 1 < jpeg.size()) ? jpeg[i + 1] : 0u;
        const uint32_t c = (i + 2 < jpeg.size()) ? jpeg[i + 2] : 0u;
        const uint32_t v = (a << 16) | (b << 8) | c;
        b64 += kB64[(v >> 18) & 0x3F];
        b64 += kB64[(v >> 12) & 0x3F];
        b64 += (i + 1 < jpeg.size()) ? kB64[(v >>  6) & 0x3F] : '=';
        b64 += (i + 2 < jpeg.size()) ? kB64[ v        & 0x3F] : '=';
    }
    return b64;
}

} // namespace

/// Shared nr_* tuning keys (same names as start_job).
NrSettings RpcDispatcher::parseNrSettings(const nlohmann::json& p) {
    NrSettings s{};
    s.enabled = p.value("nr_enabled", true);
    s.model = static_cast<NrModel>(std::clamp(p.value("nr_model", 0), 0, 2));
    s.style = static_cast<NrStyle>(std::clamp(p.value("nr_style", 0), 0, 2));
    s.intensity = std::clamp(p.value("nr_intensity", 1.0f), 0.0f, 2.0f);
    s.localToneStrength = std::clamp(p.value("nr_local_tone", 1.0f), 0.0f, 2.0f);
    s.localStructureStrength = std::clamp(p.value("nr_local_structure", 1.0f), 0.0f, 2.0f);
    s.skinStructureStrength = std::clamp(p.value("nr_skin_structure", 0.0f), 0.0f, 2.0f);
    s.useAutoMask = p.value("nr_automask", false);
    s.uiCorrection = p.value("nr_ui_correction", false);
    s.guidanceMode = static_cast<NrGuidanceMode>(std::clamp(p.value("nr_guidance", 0), 0, 3));
    return s;
}

FgSettings RpcDispatcher::parseFgSettings(const nlohmann::json& p) {
    FgSettings f{};
    f.enabled = p.value("fg_enabled", false);
    f.multiplier = std::clamp(p.value("fg_multiplier", 2), 2, 6);
    return f;
}

nlohmann::json RpcDispatcher::handleNrToolOpen(const nlohmann::json& p) {
    if (!m_ctx || !m_ngx) {
        throw std::runtime_error("GPU not bound; call bindGpu before nrtool_open");
    }
    if (m_jobRunning.load(std::memory_order_acquire)) {
        throw std::runtime_error("nrtool_open: a pipeline job is running");
    }
    const std::string color = p.at("color_source").get<std::string>();
    // buffer_dir is optional: without it the session only runs guidance mode
    // ForceZero (blind temporal filter); the session enforces that combination.
    const std::string buffer = p.value("buffer_dir", "");
    // camera_config (dlssconfig.json) is optional: FG requires it, NR-only
    // sessions don't. The export job copies it into the buffer dir.
    const std::string cameraConfig = p.value("camera_config", "");

    if (m_nrExportWorker.joinable()) m_nrExportWorker.join();
    m_nrExportRunning.store(false, std::memory_order_release);
    m_nrTool.reset();  // close any previous session
    m_nrTool = std::make_unique<NrToolSession>(m_ctx, m_ngx,
                                               std::filesystem::path(color),
                                               std::filesystem::path(buffer),
                                               std::filesystem::path(cameraConfig));
    const auto& i = m_nrTool->info();
    return {{"ok", true},
            {"color_source", i.colorSource},
            {"color_kind", i.colorKind},
            {"buffer_dir", i.bufferDir},
            {"camera_config", i.cameraConfig},
            {"frames", i.frames},
            {"width", i.width},
            {"height", i.height},
            {"buffer_width", i.bufferWidth},
            {"buffer_height", i.bufferHeight},
            {"fps", i.fps}};
}

nlohmann::json RpcDispatcher::handleNrToolPreview(const nlohmann::json& p) {
    if (!m_nrTool) throw std::runtime_error("nrtool_preview: no session (call nrtool_open)");
    if (m_nrExportRunning.load(std::memory_order_acquire)) {
        throw std::runtime_error("nrtool_preview: an export is running");
    }
    const auto frame = p.value("frame", 0);
    if (frame < 0 || static_cast<uint32_t>(frame) >= m_nrTool->info().frames) {
        throw std::runtime_error("nrtool_preview: frame " + std::to_string(frame) +
                                 " out of range (0.." +
                                 std::to_string(m_nrTool->info().frames - 1) + ")");
    }
    // GPU downscale cap for the preview JPEGs (long edge). Matches the GUI
    // canvas cap (2560) so the readback/JPEG/IPC sizes track what's shown.
    const auto maxLongEdge = static_cast<uint32_t>(p.value("preview_max_long_edge", 2560));
    auto r = m_nrTool->preview(frame, parseNrSettings(p), parseFgSettings(p), maxLongEdge);
    nlohmann::json resp{{"ok", true},
                        {"frame", frame},
                        {"width", r.width},
                        {"height", r.height},
                        {"eval_ms", r.evalMs},
                        {"reset_applied", r.resetApplied}};
    if (!r.jpeg.empty()) resp["jpeg_b64"] = b64Encode(r.jpeg);
    if (!r.interpJpeg.empty()) resp["interp_jpeg_b64"] = b64Encode(r.interpJpeg);
    return resp;
}

nlohmann::json RpcDispatcher::handleNrToolExport(const nlohmann::json& p) {
    if (!m_nrTool) throw std::runtime_error("nrtool_export: no session (call nrtool_open)");
    bool expected = false;
    if (!m_nrExportRunning.compare_exchange_strong(expected, true)) {
        throw std::runtime_error("nrtool_export: an export is already running");
    }
    if (m_jobRunning.load(std::memory_order_acquire)) {
        m_nrExportRunning.store(false, std::memory_order_release);
        throw std::runtime_error("nrtool_export: a pipeline job is running");
    }

    const std::string fmtStr = p.value("output_format", std::string{"prores_422hq"});
    const OutputFormat fmt = parseOutputFormat(fmtStr);
    const std::string outPathStr = p.value("output_path", "");
    const std::string outDirStr  = p.value("output_dir", "");
    const std::string tmpl       = p.value("output_template", std::string{"nr_####.png"});
    const NrSettings settings = parseNrSettings(p);
    const FgSettings fg = parseFgSettings(p);
    m_nrExportStop.store(false, std::memory_order_release);

    // Validation on the RPC thread — fail before the worker starts.
    const bool isVideo = fmt == OutputFormat::ProRes422HQ || fmt == OutputFormat::H265Mp4;
    if (isVideo && outPathStr.empty()) {
        m_nrExportRunning.store(false, std::memory_order_release);
        throw std::runtime_error("nrtool_export: output_path is required for video formats");
    }
    if (!isVideo && (outDirStr.empty() || tmpl.empty())) {
        m_nrExportRunning.store(false, std::memory_order_release);
        throw std::runtime_error("nrtool_export: output_dir and output_template are required for png_16");
    }

    auto* session = m_nrTool.get();
    const int outTotal = static_cast<int>(session->outputFrameCount(fg));
    m_progress.total_frames.store(outTotal);
    m_progress.current_frame.store(0);
    m_progress.percent.store(0.0f);
    m_progress.state.store(1);  // Running

    if (m_nrExportWorker.joinable()) m_nrExportWorker.join();
    m_nrExportWorker = std::thread([this, session, settings, fg, fmt,
                                    outPath = std::filesystem::path(outPathStr),
                                    outDir = std::filesystem::path(outDirStr), tmpl]() {
        try {
            session->exportTo(settings, fg, fmt, outPath, outDir, tmpl,
                              [this](float prog) {
                                  m_progress.percent.store(prog);
                              },
                              &m_nrExportStop);
            m_progress.state.store(2);  // Done
            if (m_eventCb) m_eventCb({{"event", "nrtool_export_done"}});
        } catch (const std::exception& e) {
            m_progress.state.store(3);  // Error
            const std::string what = e.what();
            const bool cancelled = what.find("cancelled") != std::string::npos;
            LOG_ERROR("[NrTool] export error: {}", what);
            if (m_eventCb) {
                m_eventCb({{"event", cancelled ? "nrtool_export_stopped" : "nrtool_export_error"},
                           {"detail", what}});
            }
        }
        m_nrExportRunning.store(false, std::memory_order_release);
    });

    return {{"ok", true}, {"started", true}, {"frames", outTotal}};
}

nlohmann::json RpcDispatcher::handleNrToolClose(const nlohmann::json& /*p*/) {
    if (m_nrExportRunning.load(std::memory_order_acquire)) {
        throw std::runtime_error("nrtool_close: export is running — stop it first");
    }
    if (m_nrExportWorker.joinable()) m_nrExportWorker.join();
    m_nrTool.reset();
    return {{"ok", true}};
}

// =============================================================================
// Segmented rendering: stream-copy concat of the per-segment videos
// =============================================================================

nlohmann::json RpcDispatcher::handleMovConcat(const nlohmann::json& p) {
    if (!p.contains("inputs") || !p["inputs"].is_array() || p["inputs"].size() < 2) {
        throw std::runtime_error("mov_concat: inputs must be an array of at least 2 file paths");
    }
    std::vector<std::filesystem::path> inputs;
    for (const auto& item : p["inputs"]) {
        if (!item.is_string() || item.get<std::string>().empty())
            throw std::runtime_error("mov_concat: every input must be a non-empty path string");
        inputs.emplace_back(item.get<std::string>());
    }
    if (!p.contains("output") || !p["output"].is_string() || p["output"].get<std::string>().empty()) {
        throw std::runtime_error("mov_concat: output path is required");
    }
    const std::filesystem::path output = p["output"].get<std::string>();
    if (std::filesystem::is_regular_file(output)) {
        throw std::runtime_error("mov_concat: output already exists: " + output.string());
    }
    const std::int64_t frames = concatVideoFiles(inputs, output);
    return {{"ok", true}, {"frames", frames}, {"output", output.string()}};
}

// =============================================================================
// PipelineConfig builder
// =============================================================================

PipelineConfig RpcDispatcher::buildPipelineConfig(const nlohmann::json& p) {
    // ── Required fields ──────────────────────────────────────────────────────
    const std::string inputDir  = p.at("input_dir").get<std::string>();
    const std::string outputDir = p.at("output_dir").get<std::string>();

    // ── Load dlssconfig.json ─────────────────────────────────────────────────
    const std::string configPath = p.value("config", pathToUtf8(pathFromUtf8(inputDir) / "dlssconfig.json"));
    auto loadResult = DlssConfigLoader::Load(pathFromUtf8(configPath));
    if (!loadResult) {
        throw std::runtime_error("dlssconfig.json load failed: " + loadResult.error().message);
    }
    DlssConfig dlssCfg = std::move(*loadResult);

    // ── Feature flags ─────────────────────────────────────────────────────────
    const std::string mode = p.value("mode", std::string{"sr"});
    const int parsedFg = parseFgMultiplier(p);
    const bool hasFg = parsedFg >= 2;
    const bool hasNr = p.value("nr", false);
    PipelineFeatures features = parseFeaturesFromMode(mode, hasFg);
    if (hasNr) features |= kNR;
    const std::string err = validateFeatures(features);
    if (!err.empty()) throw std::runtime_error(err);

    // ── Output ────────────────────────────────────────────────────────────────
    const std::string fmtStr  = p.value("output_format", std::string{"prores_422hq"});
    const std::string tmpl    = p.value("output_template", std::string{"frame_####"});
    const std::string owStr   = p.value("overwrite_mode", std::string{"overwrite"});

    // ── FG multiplier ─────────────────────────────────────────────────────────
    // Default 2x when FG is on via mode ("rr+fg") but no multiplier was sent.
    int fgMult = parsedFg >= 2 ? parsedFg : 2;

    // ── Prefetch / writers / watch ────────────────────────────────────────────
    const int prefetch   = p.value("prefetch_depth", 3);
    const int wrThreads  = p.value("writer_threads", 4);
    const int wrQueueMul = p.value("writer_queue_multiplier", 3);
    const bool watchInput = p.value("watch", false)
                         || p.value("watch_input", false)
                         || p.value("wait_for_appearance", false);
    const bool deleteConsumed = p.value("delete_consumed", watchInput);

    // ── Build config ──────────────────────────────────────────────────────────
    PipelineConfig cfg;
    cfg.dlssConfig         = std::move(dlssCfg);
    cfg.features           = features;
    cfg.inputDir           = inputDir;
    cfg.outputDir          = outputDir;
    cfg.filenameTemplate   = tmpl;
    cfg.outputFormat       = parseOutputFormat(fmtStr);
    cfg.overwriteMode      = parseOverwriteMode(owStr);
    cfg.fgMultiplier       = std::max(2, std::min(6, fgMult));
    cfg.prefetchDepth      = std::max(1, prefetch);
    cfg.writerThreads      = std::max(1, wrThreads);
    cfg.writerQueueMultiplier = std::max(1, wrQueueMul);
    cfg.watchInput         = watchInput;
    cfg.atomicInputFiles   = watchInput && p.value("atomic_input_files", false);

    // ── Segmented rendering window (0 = unbounded / plain single-run) ─────────
    cfg.inputFrameBegin      = p.value("input_frame_begin", (std::int64_t)0);
    cfg.inputFrameEnd        = p.value("input_frame_end", (std::int64_t)0);
    cfg.outputFrameBegin     = p.value("output_frame_begin", (std::int64_t)0);
    cfg.outputFrameStartIndex = std::max(1, p.value("output_frame_start_index", 1));
    if (cfg.inputFrameBegin < 0 || cfg.inputFrameEnd < 0 || cfg.outputFrameBegin < 0 ||
        (cfg.inputFrameEnd > 0 && cfg.inputFrameEnd < cfg.inputFrameBegin)) {
        throw std::runtime_error("invalid frame window: input_frame_end must be >= input_frame_begin");
    }
    if (cfg.outputFrameBegin > 0 && cfg.inputFrameBegin > 0 &&
        cfg.outputFrameBegin < cfg.inputFrameBegin) {
        throw std::runtime_error("invalid frame window: output_frame_begin must be >= input_frame_begin");
    }

    // ── NR (experimental; feature bits carry the on/off) ──────────────────────
    const NrSettings nrS = parseNrSettings(p);
    cfg.nrStyle          = static_cast<int>(nrS.style);
    cfg.nrModel          = static_cast<int>(nrS.model);
    cfg.nrIntensity      = nrS.intensity;
    cfg.nrLocalTone      = nrS.localToneStrength;
    cfg.nrLocalStructure = nrS.localStructureStrength;
    cfg.nrAutoMask       = nrS.useAutoMask;
    cfg.nrGuidanceMode   = static_cast<int>(nrS.guidanceMode);
    cfg.nrBuffer         = p.value("nr_buffer", false);
    if (cfg.nrBuffer) {
        cfg.nrBufferDir = p.value("nr_buffer_dir", std::string{});
        if (cfg.nrBufferDir.empty()) {
            cfg.nrBufferDir = cfg.outputDir;
        }
        std::error_code mkdirEc;
        std::filesystem::create_directories(pathFromUtf8(cfg.nrBufferDir), mkdirEc);
        if (mkdirEc) {
            throw std::runtime_error("cannot create nr_buffer_dir " + cfg.nrBufferDir +
                                     ": " + mkdirEc.message());
        }
    }
    if (cfg.nrBuffer && (cfg.features & kNR)) {
        throw std::runtime_error(
            "nr_buffer cannot be combined with nr: dlssnrbuffer is the export for the "
            "standalone NR tool; run NR there instead");
    }
    if (cfg.nrBuffer && (cfg.features & kFG)) {
        throw std::runtime_error(
            "nr_buffer requires FG off: guidance frames are written at input rate and "
            "would not align with interpolated output");
    }

    // NGX DLSS RR has no UltraQuality slot: the OptimalSettings callback no-ops
    // for NVSDK_NGX_PerfQuality_Value_UltraQuality (success with an all-zero
    // range), which would fail deep inside DlssRrPass::create as
    // "outside NGX range 0x0..0x0". Reject it here with an actionable message.
    if ((cfg.features & kRR) && cfg.dlssConfig.quality == QualityMode::UltraQuality) {
        throw std::runtime_error(
            "quality_mode ultra_quality is not supported by DLSS RR — NGX RR offers "
            "dlaa / quality / balanced / performance (and ultra_performance at an "
            "exact 1/3 render scale)");
    }

    const std::string rrPresetStr = p.value("rr_preset", std::string{"F"});
    if (rrPresetStr == "D" || rrPresetStr == "d")      cfg.rrPreset = RrPreset::D;
    else if (rrPresetStr == "E" || rrPresetStr == "e") cfg.rrPreset = RrPreset::E;
    else                                              cfg.rrPreset = RrPreset::F;

    // ── Wire callbacks to RenderProgress atomics ──────────────────────────────
    // Frame-window aware totals: segmented rendering narrows the consumed set
    // (input_frame_begin/end) and holds back warmup frames (output_frame_begin),
    // so the progress bar counts real outputs only.
    const bool useFg      = (cfg.features & kFG) != 0;
    const int fgFactor    = useFg ? cfg.fgMultiplier : 1;
    const FrameWindowStats win = frameWindowStats(
        cfg.dlssConfig, cfg.inputFrameBegin, cfg.inputFrameEnd, cfg.outputFrameBegin,
        useFg, fgFactor);
    const int totalInput  = win.realInputs;
    const int totalOutput = win.expectedOutputs;

    m_progress.current_frame.store(0);
    m_progress.total_frames.store(totalOutput);
    m_progress.percent.store(0.0f);
    m_progress.dlss_current_frame.store(0);
    m_progress.dlss_total_frames.store(totalInput);
    m_progress.dlss_percent.store(0.0f);
    m_progress.temporal_resets.store(0);
    m_progress.state.store(0);
    setProgressError({});

    cfg.callbacks.onFrame = [this, totalOutput](const FrameEvent& ev) {
        if (!ev.isGenerated && ev.wasReset) m_progress.temporal_resets.fetch_add(1);
        m_progress.dlss_current_frame.store(ev.inputIndex + 1);
        m_progress.dlss_percent.store(
            totalOutput > 0 ? 100.0f * static_cast<float>(ev.inputIndex + 1) / static_cast<float>(m_progress.dlss_total_frames.load()) : 0.0f);
        m_progress.current_frame.store(ev.outputIndex + 1);
        const int tot = m_progress.total_frames.load();
        m_progress.percent.store(
            tot > 0 ? 100.0f * static_cast<float>(ev.outputIndex + 1) / static_cast<float>(tot) : 0.0f);
        LOG_DEBUG("[Pipeline] frame input={}/{} output={}/{} reset={}{}",
                  ev.inputIndex + 1, ev.inputTotal,
                  ev.outputIndex + 1, ev.outputTotal,
                  ev.wasReset,
                  ev.isGenerated ? " [FG]" : "");
    };

    cfg.callbacks.onProgress = [this](float prog) {
        // Pipeline reports 0–1; RPC percent is 0–100.
        m_progress.percent.store((prog >= 0.f && prog <= 1.f) ? prog * 100.f : prog);
    };

    if (cfg.watchInput && deleteConsumed) {
        cfg.callbacks.onInputConsumed = [](const std::filesystem::path& path) {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            if (ec)
                LOG_WARN("[Pipeline] consume-delete failed {}: {}", path.string(), ec.message());
            else
                LOG_INFO("[Pipeline] consumed {}", path.filename().string());
        };
    }

    cfg.callbacks.onRunEvent = [this](RunEvent ev, const std::string& detail) {
        switch (ev) {
            case RunEvent::Started:
                m_progress.state.store(1);
                m_progress.phase.store(0);
                LOG_INFO("[Pipeline] run started: {}", detail);
                break;
            case RunEvent::Stopping:
                m_progress.phase.store(1);  // finalize: writer drain + mov trailer
                LOG_INFO("[Pipeline] run stopping: {}", detail);
                break;
            case RunEvent::Cancelled:
                m_progress.state.store(0);
                m_progress.phase.store(0);
                m_jobRunning.store(false, std::memory_order_release);
                LOG_INFO("[Pipeline] run cancelled: {}", detail);
                break;
            case RunEvent::Completed:
                m_progress.state.store(2);
                m_progress.phase.store(0);
                m_jobRunning.store(false, std::memory_order_release);
                LOG_INFO("[Pipeline] run completed: {}", detail);
                break;
            case RunEvent::Error:
                m_progress.state.store(3);
                m_progress.phase.store(0);
                m_jobRunning.store(false, std::memory_order_release);
                LOG_ERROR("[Pipeline] run error: {}", detail);
                break;
        }
        if (m_eventCb) {
            m_eventCb({{"event", "run_event"},
                       {"kind",  static_cast<int>(ev)},
                       {"detail", detail}});
        }
    };

    cfg.callbacks.wantsPreview = []() { return true; };

    cfg.callbacks.encodePreview = [this](uint32_t w, uint32_t h,
                                         const std::vector<uint8_t>& rgba8) {
        auto jpeg = encodePreviewJpeg(w, h, rgba8, 80);
        if (!jpeg.empty()) {
            m_previewW.store(w);
            m_previewH.store(h);
        }
        return jpeg;
    };

    cfg.callbacks.onPreview = [this](std::vector<uint8_t> jpeg) {
        std::lock_guard<std::mutex> lk(m_previewMu);
        m_previewJpeg = std::move(jpeg);
    };

    cfg.callbacks.onFrameDiagnostics = [this](const std::string& json) {
        if (m_eventCb) {
            m_eventCb({{"event", "frame_diagnostics"}, {"data", json}});
        }
    };

    return cfg;
}

// =============================================================================
// Worker thread launcher
// =============================================================================

std::string RpcDispatcher::launchPipeline(PipelineConfig cfg) {
    if (!m_ctx || !m_ngx)
        throw std::runtime_error("GPU not bound; call bindGpu before launching pipeline");

    if (m_worker.joinable()) m_worker.join();  // clean up previous worker

    m_pipeline = std::make_unique<Pipeline>(m_ctx, m_ngx);
    m_jobRunning.store(true, std::memory_order_release);

    const std::string taskId = generateTaskId();
    m_currentTaskId = taskId;
    m_progress.state.store(0);  // will switch to 1 when Started fires

    // Capture cfg by value into the worker lambda.
    m_worker = std::thread([this, cfg = std::move(cfg)]() mutable {
        try {
            m_pipeline->run(cfg);
        } catch (const std::exception& e) {
            const std::string what = e.what();
            LOG_ERROR("[Pipeline worker] uncaught exception: {}", what);
            setProgressError(what);
            m_progress.state.store(3);
            m_jobRunning.store(false, std::memory_order_release);
            if (m_eventCb) {
                m_eventCb({{"event", "run_event"},
                           {"kind",  static_cast<int>(RunEvent::Error)},
                           {"detail", what}});
            }
        }
    });

    LOG_INFO("[RPC] start_job launched task_id={}", taskId);
    return taskId;
}

// =============================================================================
// Legacy two-step API (MethodChannel compat)
// =============================================================================

nlohmann::json RpcDispatcher::handleSetTask(const nlohmann::json& p) {
    m_taskConfig = p;
    m_currentTaskId = generateTaskId();
    LOG_INFO("[RPC] setTask received, task_id={}", m_currentTaskId);
    return {{"ok", true}, {"task_id", m_currentTaskId}};
}

nlohmann::json RpcDispatcher::handleCheckReady(const nlohmann::json& /*p*/) {
    const bool ready = m_ctx && m_ngx && !m_taskConfig.is_null() && !m_taskConfig.empty();
    const std::string reason = ready ? std::string{} : std::string{"No task configured or GPU not bound"};
    LOG_INFO("[RPC] checkReady ready={} reason={}", ready, reason);
    return {{"ready", ready}, {"reason", reason}};
}

nlohmann::json RpcDispatcher::handleCheckOutputCollisions(const nlohmann::json& p) {
    // TODO(T36+): scan output_dir for files matching template + frame range.
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
    if (!m_ctx || !m_ngx) {
        return {{"ok", false}, {"error", "GPU not bound; pipeline cannot start"}};
    }
    if (m_taskRunning.exchange(true)) {
        return {{"ok", false}, {"error", "Task already running"}};
    }

    PipelineConfig cfg;
    try {
        cfg = buildPipelineConfig(m_taskConfig);
    } catch (const std::exception& e) {
        m_taskRunning.store(false);
        const std::string what = e.what();
        return {{"ok", false}, {"error", what}};
    }

    try {
        launchPipeline(std::move(cfg));
    } catch (const std::exception& e) {
        m_taskRunning.store(false);
        const std::string what = e.what();
        return {{"ok", false}, {"error", what}};
    }

    LOG_INFO("[RPC] startTask: {}", m_currentTaskId);
    return {{"ok", true}, {"task_id", m_currentTaskId}};
}

// =============================================================================
// Status / debug
// =============================================================================

nlohmann::json RpcDispatcher::handleGetStatus(const nlohmann::json& /*p*/) {
    bool srAvailable = m_ngx ? m_ngx->isSrAvailable() : false;
    bool fgAvailable = m_ngx ? m_ngx->isFgAvailable() : false;
    bool rrAvailable = m_ngx ? m_ngx->isRrAvailable() : false;
    int  fgMax       = m_ngx ? m_ngx->getFgMaxMultiplier() : 0;

    NgxDllVersions ver{};
    if (m_ngx) ver = m_ngx->getDllVersions();

    const std::string gpuName = m_ctx ? "NVIDIA GPU" : "unknown";

    LOG_INFO("[RPC] getStatus sr={} fg={} rr={}", srAvailable, fgAvailable, rrAvailable);
    return {
        {"sr_available",        srAvailable},
        {"fg_available",        fgAvailable},
        {"rr_available",        rrAvailable},
        {"fg_max_multiplier",   fgMax},
        {"gpu_name",            gpuName},
        {"is_hdr_supported",    true},
        {"dlss_sr_version",     ver.sr},
        {"dlss_fg_version",     ver.fg},
        {"dlss_rr_version",     ver.rr},
        {"video_encoders",      nlohmann::json::array({"prores_422hq", "h265_hevc"})},
        {"export_formats_ldr",  nlohmann::json::array({"png_16", "exr", "prores_422hq", "h265_hevc"})},
        {"export_formats_hdr",  nlohmann::json::array({"exr", "prores_422hq", "h265_hevc"})},
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
    LOG_DEBUG("[RPC] getProgress current={} total={} state={}",
              m_progress.current_frame.load(),
              m_progress.total_frames.load(),
              m_progress.state.load());
    return {
        {"current_frame",      m_progress.current_frame.load()},
        {"total_frames",       m_progress.total_frames.load()},
        {"percent",            m_progress.percent.load()},
        {"phase",              m_progress.phase.load()},
        {"dlss_current_frame", m_progress.dlss_current_frame.load()},
        {"dlss_total_frames",  m_progress.dlss_total_frames.load()},
        {"dlss_percent",       m_progress.dlss_percent.load()},
        {"state",              m_progress.state.load()},
    };
}

nlohmann::json RpcDispatcher::handleGetPreviewFrame(const nlohmann::json& /*p*/) {
    return handlePreviewFrame({});
}

nlohmann::json RpcDispatcher::handleSetDebugMode(const nlohmann::json& /*p*/) {
    LOG_DEBUG("[RPC] setDebugMode: not implemented");
    return {{"ok", false}, {"reason", "not implemented"}};
}

} // namespace dlss
