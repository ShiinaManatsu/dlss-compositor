#pragma once
// Pipeline — unified linear DLSS compositor pipeline.
//
// Single code path; feature bits control which passes run:
//
//   EXR (watcher/prefetch)
//     │
//     ▼
//   DepthConv + MvConv  (always — convert Blender space → NDC/DLSS space)
//     │
//     ▼
//   SR pass  ──── [ feature::kSR ]
//   RR pass  ──── [ feature::kRR ]    (exactly one of kSR / kRR must be set)
//     │
//     ▼
//   Tonemap              (always — SR/RR output is scene-linear in, display-ready out)
//     │
//     ▼
//   NR pass  ──── [ feature::kNR ]    (experimental — DLSSNR feature 18, same-res
//     │                                 display-domain denoise; rgb10a2 ↔ rgba8 pack)
//     ▼
//   FG pass  ──── [ feature::kFG ]    (optional; off → frames passed straight through)
//     │
//     ▼
//   InverseTonemap ───── when output format is ExrLinear
//     │
//     ▼
//   Output (AsyncFrameWriter / ProResWriter)
//
// Future features with bit positions reserved:
//   kHDR   = 0x10   Explicit HDR path flag (currently derived from dlssconfig.is_hdr)
//
// NOT a bitmask for input source selection (SR vs RR are mutually exclusive and
// validated at construction time).  Use PipelineFeatures::validate() before run().
//
// Frontend communication is entirely through PipelineCallbacks — no direct
// coupling to RpcDispatcher in this header.  The caller wires up the callbacks
// and can forward to RPC, IPC, or anything else.

#include "../core/dlssconfig.h"
#include "../core/types.h"
#include "../dlss/dlss_sr.h"
#include "../dlss/dlss_rr.h"
#include "../dlss/dlss_fg.h"
#include "../dlss/dlss_nr.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"
#include "../gpu/passes/depth_pass.h"
#include "../gpu/passes/mv_pass.h"
#include "../gpu/passes/tonemap_pass.h"
#include "../gpu/passes/inverse_tonemap_pass.h"
#include "../gpu/passes/lut_inverse_pass.h"
#include "../gpu/passes/lut_tonemap_pass.h"
#include "../gpu/passes/nr_format_pass.h"
#include "../io/frame_prefetcher.h"
#include "../io/rr_frame_prefetcher.h"
#include "../io/channel_mapper.h"
#include "../io/rr_channel_mapper.h"
#include "../io/async_writer.h"
#include "../io/prores_writer.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace dlss {

// ─────────────────────────────────────────────────────────────────────────────
// Feature flags
// ─────────────────────────────────────────────────────────────────────────────

/// Bitfield of active pipeline stages.  Combine with | ; test with &.
/// Bit assignments are stable — do not reorder or reuse positions.
enum PipelineFeature : std::uint32_t {
    kSR   = 0x01u,  ///< DLSS Super Resolution (mutually exclusive with kRR)
    kRR   = 0x02u,  ///< DLSS Ray Reconstruction (mutually exclusive with kSR)
    kFG   = 0x04u,  ///< DLSS Frame Generation (optional post-SR/RR)
    kNR   = 0x08u,  ///< DLSSNR feature 18 — experimental, display-domain denoise
                    ///< between tonemap and FG (see docs/research/magpie-nr-notes.md)
    // 0x10 reserved for kHDR — explicit HDR path override
    // 0x20–0x80 available for future use
};

using PipelineFeatures = std::uint32_t;

/// Validate a feature combination. Returns empty string on success, error message on failure.
inline std::string validateFeatures(PipelineFeatures f) {
    const bool hasSR = (f & kSR) != 0;
    const bool hasRR = (f & kRR) != 0;
    if (!hasSR && !hasRR)  return "pipeline features: exactly one of kSR / kRR must be set";
    if (hasSR && hasRR)    return "pipeline features: kSR and kRR are mutually exclusive";
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// Front-end communication callbacks
// ─────────────────────────────────────────────────────────────────────────────

/// Per-frame progress state delivered to the front end.
struct FrameEvent {
    int  inputIndex;       ///< 0-based index of the input (Blender-rendered) frame
    int  inputTotal;       ///< Total input frames in this run
    int  outputIndex;      ///< 0-based output frame index (includes FG-generated frames)
    int  outputTotal;      ///< Total expected output frames
    bool isGenerated;      ///< True for FG-interpolated frames (not a direct SR/RR output)
    bool wasReset;         ///< True when InReset was set (first frame / gap / camera cut)
    float frameEvalMs;     ///< Wall-clock time for the GPU eval of this frame (debug)
};

/// Lifecycle events sent once per run.
enum class RunEvent {
    Started,    ///< run() entered, resources allocated, first frame about to be processed
    Stopping,   ///< stop() was called; mid-run cancellation in progress
    Cancelled,  ///< run() exited due to stop()
    Completed,  ///< run() finished normally
    Error,      ///< run() threw an exception (message in the error callback)
};

/// All callbacks are optional (nullptr = skip).  Called from the pipeline thread.
/// Implementations must be thread-safe if they touch shared state.
struct PipelineCallbacks {
    // ── Progress ────────────────────────────────────────────────────────────
    /// Called once per output frame (real and FG-generated).
    std::function<void(const FrameEvent&)> onFrame;

    /// Called when aggregate progress changes (0.0–1.0 output fraction).
    /// More frequent than onFrame on the front end; coalescing is caller's job.
    std::function<void(float progress)> onProgress;

    // ── Preview ──────────────────────────────────────────────────────────────
    /// Called with an RGBA8 JPEG-encoded thumbnail after each rendered (non-generated)
    /// output frame when the front end has opted in.  Empty jpeg = skip sending.
    std::function<std::vector<uint8_t>(uint32_t w, uint32_t h,
                                       const std::vector<uint8_t>& rgba8)> encodePreview;
    std::function<void(std::vector<uint8_t> jpeg)> onPreview;

    /// Return true when the front end wants the next preview frame.
    /// Returning false skips the GPU download for that frame (cheaper).
    std::function<bool()> wantsPreview;

    // ── Lifecycle ────────────────────────────────────────────────────────────
    std::function<void(RunEvent, const std::string& detail)> onRunEvent;

    // ── Input EXR lifecycle (for watcher/delete integration) ─────────────────
    /// Called *after* the EXR at `path` has been fully decoded into CPU memory.
    /// The pipeline guarantees it will not read the file again after this call.
    /// Use to unlink the file and keep the RAM disk at low watermark.
    std::function<void(const std::filesystem::path& path)> onInputConsumed;

    // ── Debug/diagnostics ─────────────────────────────────────────────────────
    /// Called with structured per-frame timing data (populated even in release).
    std::function<void(const std::string& jsonDiagnostics)> onFrameDiagnostics;
};

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline configuration
// ─────────────────────────────────────────────────────────────────────────────

struct PipelineConfig {
    // ── Required ──────────────────────────────────────────────────────────────
    DlssConfig      dlssConfig;          ///< Camera / jitter / LUT data from inject.py
    PipelineFeatures features = kSR | kFG; ///< Feature bits (see PipelineFeature)
    std::string     inputDir;            ///< Directory containing input EXR files
    std::string     outputDir;           ///< Destination for output files
    std::string     filenameTemplate = "frame_####"; ///< Output filename pattern

    // ── SR / RR ───────────────────────────────────────────────────────────────
    DlssPreset      srPreset    = DlssPreset::Auto; ///< SR preset (ignored when kRR)
    RrPreset        rrPreset    = RrPreset::F;      ///< RR preset F = RR2 (310.7.128+ / driver ≥580)
    RrRoughnessMode rrRoughness = RrRoughnessMode::Unpacked;

    // ── FG ────────────────────────────────────────────────────────────────────
    int fgMultiplier = 2; ///< Frame generation multiplier (2–6; clamped to NGX cap)

    // ── NR (experimental — requires nvngx_dlssnr.dll next to the exe) ─────────
    int  nrStyle        = 0;    ///< NrStyle: 0 Default / 1 Natural / 2 Cinematic
    float nrIntensity   = 1.0f; ///< 0–1
    int  nrGuidanceMode = 0;    ///< NrGuidanceMode: 0 all / 1 zero / 2 motion / 3 depth

    // ── Output ────────────────────────────────────────────────────────────────
    OutputFormat    outputFormat = OutputFormat::ExrLinear;
    OverwriteMode   overwriteMode = OverwriteMode::Overwrite;
    EncoderOptions  encoderOptions{};
    H265EncoderOptions h265Options{};

    // ── Performance ───────────────────────────────────────────────────────────
    int prefetchDepth        = 3;   ///< EXR read-ahead depth
    int writerThreads        = 4;
    int writerQueueMultiplier = 3;

    /// Live folder watch (RAM-disk / Blender File Output). When true, run() waits
    /// for each expected EXR to appear in inputDir, decodes it, then fires
    /// onInputConsumed so the caller can unlink and keep the disk at low watermark.
    /// When false (default), prefetchers read the existing files listed in dlssconfig.
    bool watchInput = false;

    // ── Callbacks (frontend communication) ────────────────────────────────────
    PipelineCallbacks callbacks;

    // ── Debug (compiled out in Release unless DLSS_DEBUG_PIPELINE=1) ──────────
    /// Dump guide buffers for the first N input frames.
    /// Populated from DLSS_COMPOSITOR_DUMP_GUIDES env var in debug builds only.
    int debugDumpGuideFrames = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline
// ─────────────────────────────────────────────────────────────────────────────

/// Unified linear DLSS compositor pipeline.
///
/// Construction is cheap.  All GPU resources are allocated inside run().
/// Thread-hostile: do not call run() from multiple threads simultaneously.
/// stop() is thread-safe and may be called from any thread.
class Pipeline {
public:
    Pipeline(VulkanContext* ctx, NgxContext* ngx);
    ~Pipeline() = default;

    Pipeline(const Pipeline&)            = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /// Execute the pipeline with the given config.
    /// Blocks until all output frames are written or stop() is called.
    /// Throws std::runtime_error on misconfiguration or GPU failure.
    void run(const PipelineConfig& config);

    /// Signal run() to stop cleanly at the next frame boundary.
    /// Safe to call from any thread.
    void stop() { m_stop.store(true, std::memory_order_release); }

    /// True while run() is executing.
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    // Pass implementations
    void runSrPass(VkCommandBuffer cmd, const PipelineConfig& config,
                   DlssSrPass& sr, DlssFgPass* fg,
                   const MappedBuffers& mapped, const DlssConfig& dlssCfg,
                   int frameIdx, bool reset, float frameTimeMs,
                   TexturePool& pool, struct PassTextures& tex);

    void runRrPass(VkCommandBuffer cmd, const PipelineConfig& config,
                   DlssRrPass& rr, DlssFgPass* fg,
                   const RrMappedBuffers& mapped, const DlssConfig& dlssCfg,
                   int frameIdx, bool reset, float frameTimeMs,
                   TexturePool& pool, struct PassTextures& tex);

    // Helpers
    static bool shouldDumpGuides();

    VulkanContext* m_ctx;
    NgxContext*    m_ngx;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
};

} // namespace dlss
