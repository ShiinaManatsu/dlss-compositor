#pragma once
// NrToolSession — standalone DLSSNR/DLSSFG compositor session (NR/FG 工具 backend).
//
// Post pipeline over the Stage-A export: consumes the display-ready color
// (video or PNG16 sequence), optionally the dlssnrbuffer guidance sequence
// and dlssconfig.json camera data, then runs
//
//   color (RGBA8, display-ready, output res)  ─┐
//   mv   (RG16F, render res, dlssnrbuffer R/G) ├→ [DLSSNR] → [DLSSFG] → out
//   depth(R32F,  render res, dlssnrbuffer B)   ─┘        ├→ preview JPEGs
//                                                        └→ unpack → rgb10_a2 → ProRes / PNG16
//
// NR and FG are independently enabled. FG additionally requires the
// guidance buffer (MV) and dlssconfig.json (per-frame camera data, copied
// into the buffer dir by the export job) — fails loud when missing.
//
// The guidance buffers are optional for NR: without them the session runs
// guidance mode ForceZero only (blind temporal filter). Temporal state is
// kept between sequential frames; scrubbing further than ±1 frame triggers
// Reset for both features. Fails loud on missing inputs (no fallback).
//
// FG output ordering (FgOnlyPipeline parity): multiplier N emits per input
// frame the (N-1) interpolated frames between the previous and current real
// frame, then the real frame — the first input emits the real frame only.

#include "../core/dlssconfig.h"
#include "../dlss/dlss_fg.h"
#include "../dlss/dlss_nr.h"
#include "../gpu/passes/nr_format_pass.h"
#include "../gpu/passes/prores_convert_pass.h"
#include "../gpu/texture_pool.h"
#include "../gpu/vulkan_context.h"
#include "../io/nr_buffer_reader.h"
#include "../io/video_reader.h"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dlss {

class NgxContext;
class PreviewPass;

struct FgSettings {
    bool enabled    = false;
    int  multiplier = 2;  // 2..6, capped by NGX capability query
};

struct NrToolInfo {
    std::string colorSource;   ///< resolved video file / sequence directory
    std::string colorKind;     ///< "video" | "png_sequence"
    std::string bufferDir;     ///< dlssnrbuffer directory
    std::string cameraConfig;  ///< dlssconfig.json path ("" when none supplied)
    uint32_t    frames       = 0;
    uint32_t    width        = 0;  ///< color/output resolution (NR/FG same-res)
    uint32_t    height       = 0;
    uint32_t    bufferWidth  = 0;  ///< guidance resolution (render res)
    uint32_t    bufferHeight = 0;
    double      fps          = 0.0;
};

class NrToolSession {
public:
    NrToolSession(VulkanContext* ctx, NgxContext* ngx,
                  std::filesystem::path colorSource,
                  std::filesystem::path bufferDir,
                  std::filesystem::path cameraConfig);
    ~NrToolSession();

    NrToolSession(const NrToolSession&) = delete;
    NrToolSession& operator=(const NrToolSession&) = delete;

    const NrToolInfo& info() const { return m_info; }

    struct PreviewResult {
        std::vector<std::uint8_t> jpeg;
        /// FG interpolated frame between the previous and this real frame —
        /// empty when FG is off or the eval was a reset (history warmup).
        std::vector<std::uint8_t> interpJpeg;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double evalMs = 0.0;
        bool   resetApplied = false;
    };

    /// Decode + process one frame (NR, then FG when enabled), return preview
    /// JPEG(s). `interpJpeg` is valid while playing sequentially with FG on.
    /// Readbacks go through PreviewPass (GPU downscale to `maxLongEdge` on the
    /// long edge before the CPU ever sees the pixels — the display chain is
    /// capped near 1920 wide, so shipping full-res JPEGs is pure waste).
    PreviewResult preview(std::int64_t frameIndex, const NrSettings& settings,
                          const FgSettings& fg, std::uint32_t maxLongEdge);

    /// Retune without evaluating (settings persist for the next preview/export).
    void updateSettings(const NrSettings& settings) { m_settings = settings; }

    /// Output frames a full sequential export writes with these FG settings.
    uint32_t outputFrameCount(const FgSettings& fg) const {
        return fg.enabled
            ? m_info.frames + (m_info.frames - 1) * static_cast<uint32_t>(fg.multiplier - 1)
            : m_info.frames;
    }

    /// Full sequential export. Video formats mux through ProResWriter (FG
    /// exports mux at fps × multiplier); PNG16 writes per-frame files
    /// `outputDir/template`, renumbered sequentially. `stop` aborts between
    /// frames. onProgress receives [0..1] over output frames.
    void exportTo(const NrSettings& settings,
                  const FgSettings& fg,
                  OutputFormat format,
                  const std::filesystem::path& outputPath,      ///< video file path
                  const std::filesystem::path& outputDir,       ///< png: directory
                  const std::string& templateName,              ///< png: "nr_####.png"
                  const std::function<void(float)>& onProgress,
                  std::atomic<bool>* stop);

private:
    // ── Decode prefetch ──────────────────────────────────────────────────────
    // A worker thread owns the color/guidance decoders (VideoReader keeps
    // sequential state, so it must have a single owner) and speculatively
    // decodes frame N+1 while the main path uploads + evaluates frame N.
    struct PrefetchedFrame {
        std::int64_t              frame = -1;
        bool                      guidance = false;
        std::string               error;      ///< non-empty: decode threw on the worker
        std::vector<std::uint8_t> colorRgba8; ///< output-res
        std::vector<std::uint8_t> mvBytes;    ///< empty when !guidance
        std::vector<float>        depth;
    };

    /// Worker body: wait for a job, decode it, publish the result.
    void prefetchLoop();

    /// Worker-side decode (runs on the prefetch thread only).
    PrefetchedFrame decodeColorAndGuidance(std::int64_t frameIndex, bool guidance);

    /// Consume the sequential prefetch when it matches (frame + guidance);
    /// otherwise decode synchronously through the worker (seek / toggle).
    PrefetchedFrame fetchFrame(std::int64_t frameIndex, bool needGuidance);

    bool takePrefetched(std::int64_t frameIndex, bool guidance, PrefetchedFrame& out);
    PrefetchedFrame decodeNow(std::int64_t frameIndex, bool guidance);
    void postPrefetch(std::int64_t frameIndex, bool guidance);
    void startPrefetchWorker();
    void stopPrefetchWorker();

    /// Lazy GPU setup on first frame (NR feature creation = availability probe).
    void ensureCreated();

    /// Lazy FG feature creation; recreates when the multiplier changes.
    void ensureFgCreated(int multiplier);

    /// Decode color (+ guidance when needed) and evaluate DLSSNR when enabled.
    /// Takes the prefetched pixels; leaves the processed color in m_nrOut
    /// (enabled, GENERAL) or m_colorIn (disabled, SHADER_READ). Updates
    /// m_lastFrame.
    void evaluateFrame(std::int64_t frameIndex, bool nrEnabled, bool needGuidance,
                       PrefetchedFrame&& pixels);

    /// Evaluate DLSSFG on the processed color. Returns the (multiplier-1)
    /// interpolated output handles in GENERAL — the caller writes/downloads
    /// and releases them. m_displayNr holds the real frame (SHADER_READ).
    std::vector<PooledTextureHandle> evaluateFg(std::int64_t frameIndex, bool reset,
                                                const FgSettings& fg);

    /// DLSS_NRTOOL_DUMP_DIR forensics: write the raw 4-byte-per-pixel device
    /// bytes of `image` to dir/f<frame>_<tag>.raw (layout restored afterwards).
    void debugDumpRaw(const std::filesystem::path& dir, std::int64_t frameIndex,
                      const char* tag, VkImage image, uint32_t w, uint32_t h);

    /// Lazy preview-pass construction (needs preview_display.comp.spv — fail
    /// loud on first preview if the pack is broken, never at open time).
    void ensurePreviewPass();

    VulkanContext* m_ctx = nullptr;
    NgxContext*    m_ngx = nullptr;

    NrToolInfo m_info;
    NrSettings m_settings{};

    // Color source: video file or PNG16 sequence.
    VideoReader m_video;
    bool        m_isVideo = false;
    std::vector<std::filesystem::path> m_pngFiles;

    NrBufferReader m_buffer;
    bool           m_hasBuffer = false;  ///< guidance dir supplied at open

    std::filesystem::path m_cameraConfigPath;
    std::unique_ptr<DlssConfig> m_camConfig;  ///< per-frame camera data for FG
    std::vector<std::string> m_camKeys;       ///< frame keys sorted numerically

    // Persistent per-session textures (same every frame — no pooling churn).
    TexturePool        m_pool;
    PooledTextureHandle m_colorIn{};   ///< RGBA8, output res (source color)
    PooledTextureHandle m_mvTex{};     ///< RG16F, render res (guidance)
    PooledTextureHandle m_depthTex{};  ///< R32F,  render res (guidance)
    PooledTextureHandle m_nrOut{};     ///< RGBA8, output res (NR output)
    PooledTextureHandle m_displayNr{}; ///< rgb10_a2, output res (prores pack / FG backbuffer)

    std::unique_ptr<DlssNrPass>  m_nrPass;
    std::unique_ptr<DlssFgPass>  m_fgPass;
    int m_fgMultiplier = 0;             ///< multiplier m_fgPass was created with
    std::unique_ptr<NrFormatPass> m_nrUnpack;      ///< rgba8 → rgb10_a2
    std::unique_ptr<ProresConvertPass> m_proresConvert;
    std::unique_ptr<PreviewPass> m_previewPass;    ///< GPU downscale + RGBA8 readback

    // Prefetch worker (owns m_video / m_buffer / m_pngFiles reads).
    std::thread             m_prefetchThread;
    std::mutex              m_prefetchMu;
    std::condition_variable m_prefetchCv;
    bool                    m_prefetchQuit = false;
    struct PrefetchJob {
        std::int64_t frame = -1;
        bool         guidance = false;
    };
    PrefetchJob    m_prefetchJob;    ///< pending work (worker picks it up)
    PrefetchedFrame m_prefetchResult;///< last completed decode
    std::int64_t   m_lastFetched = -1;  ///< sequential-prefetch tracking

    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    std::int64_t  m_lastFrame = -1;  ///< NR/FG temporal continuity tracking
};

} // namespace dlss
