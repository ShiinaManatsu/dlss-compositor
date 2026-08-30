#pragma once
// NrToolSession — standalone DLSSNR compositor session (NR 工具 backend).
//
// Premiere-style effect pass: consumes the Stage-A export (color video or
// PNG16 sequence) + the dlssnrbuffer guidance sequence, previews any frame
// with live NR settings, and exports the denoised result once.
//
//   color (RGBA8, display-ready, output res)  ─┐
//   mv   (RG16F, render res, dlssnrbuffer R/G) ├→ DLSSNR → RGBA8 out
//   depth(R32F,  render res, dlssnrbuffer B)   ─┘    ├→ preview JPEG
//                                                    └→ unpack → rgb10_a2 → ProRes
//
// The pass keeps DLSSNR's temporal state between sequential frames; scrubbing
// further than ±1 frame triggers Reset. Fails loud on missing inputs (no
// fallback rendering).

#include "../dlss/dlss_nr.h"
#include "../gpu/passes/nr_format_pass.h"
#include "../gpu/passes/prores_convert_pass.h"
#include "../gpu/texture_pool.h"
#include "../gpu/vulkan_context.h"
#include "../io/nr_buffer_reader.h"
#include "../io/video_reader.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace dlss {

class NgxContext;

struct NrToolInfo {
    std::string colorSource;   ///< resolved video file / sequence directory
    std::string colorKind;     ///< "video" | "png_sequence"
    std::string bufferDir;     ///< dlssnrbuffer directory
    uint32_t    frames       = 0;
    uint32_t    width        = 0;  ///< color/output resolution (NR same-res)
    uint32_t    height       = 0;
    uint32_t    bufferWidth  = 0;  ///< guidance resolution (render res)
    uint32_t    bufferHeight = 0;
    double      fps          = 0.0;
};

class NrToolSession {
public:
    NrToolSession(VulkanContext* ctx, NgxContext* ngx,
                  std::filesystem::path colorSource,
                  std::filesystem::path bufferDir);
    ~NrToolSession();

    NrToolSession(const NrToolSession&) = delete;
    NrToolSession& operator=(const NrToolSession&) = delete;

    const NrToolInfo& info() const { return m_info; }

    struct PreviewResult {
        std::vector<std::uint8_t> jpeg;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        double evalMs = 0.0;
        bool   resetApplied = false;
    };

    /// Decode + denoise one frame, return its preview JPEG.
    PreviewResult preview(std::int64_t frameIndex, const NrSettings& settings);

    /// Retune without evaluating (settings persist for the next preview/export).
    void updateSettings(const NrSettings& settings) { m_settings = settings; }

    /// Full sequential export. Video formats mux through ProResWriter; PNG16
    /// writes per-frame files `outputDir/template`. `stop` aborts between
    /// frames. onProgress receives [0..1].
    void exportTo(const NrSettings& settings,
                  OutputFormat format,
                  const std::filesystem::path& outputPath,      ///< video file path
                  const std::filesystem::path& outputDir,       ///< png: directory
                  const std::string& templateName,              ///< png: "nr_####.png"
                  const std::function<void(float)>& onProgress,
                  std::atomic<bool>* stop);

private:
    /// Lazy GPU setup on first frame (NR feature creation = availability probe).
    void ensureCreated();

    /// Decode color + guidance for `frameIndex`, upload, evaluate DLSSNR.
    /// Returns with nrOut in GENERAL. Leaves videoReader positioned after the frame.
    void evaluateFrame(std::int64_t frameIndex);

    VulkanContext* m_ctx = nullptr;
    NgxContext*    m_ngx = nullptr;

    NrToolInfo m_info;
    NrSettings m_settings{};

    // Color source: video file or PNG16 sequence.
    VideoReader m_video;
    bool        m_isVideo = false;
    std::vector<std::filesystem::path> m_pngFiles;

    NrBufferReader m_buffer;

    // Persistent per-session textures (same every frame — no pooling churn).
    TexturePool        m_pool;
    PooledTextureHandle m_colorIn{};   ///< RGBA8, output res (NR input)
    PooledTextureHandle m_mvTex{};     ///< RG16F, render res (guidance)
    PooledTextureHandle m_depthTex{};  ///< R32F,  render res (guidance)
    PooledTextureHandle m_nrOut{};     ///< RGBA8, output res (NR output)
    PooledTextureHandle m_displayNr{}; ///< rgb10_a2, output res (prores path)

    std::unique_ptr<DlssNrPass>  m_nrPass;
    std::unique_ptr<NrFormatPass> m_nrUnpack;      ///< rgba8 → rgb10_a2
    std::unique_ptr<ProresConvertPass> m_proresConvert;

    VkCommandPool m_cmdPool = VK_NULL_HANDLE;
    std::int64_t  m_lastFrame = -1;  ///< DLSSNR temporal continuity tracking
};

} // namespace dlss
