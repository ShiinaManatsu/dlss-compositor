#pragma once

#include "../core/dlssconfig.h"
#include "../dlss/dlss_sr.h"
#include "../dlss/dlss_fg.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"
#include "../gpu/passes/depth_pass.h"
#include "../gpu/passes/mv_pass.h"
#include "../gpu/passes/tonemap_pass.h"
#include "../gpu/passes/inverse_tonemap_pass.h"
#include "../gpu/passes/lut_inverse_pass.h"
#include "../gpu/passes/lut_tonemap_pass.h"
#include "../io/frame_prefetcher.h"
#include "../io/channel_mapper.h"
#include "../io/async_writer.h"
#include "../io/prores_writer.h"
#include "../rpc/dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dlss {

struct SrFgConfig {
    DlssConfig dlssConfig;
    OutputFormat outputFormat = OutputFormat::ExrLinear;
    std::string inputDir;
    std::string outputDir;
    std::string filenameTemplate = "frame_####.exr";
    OverwriteMode overwriteMode = OverwriteMode::Overwrite;
    EncoderOptions encoderOptions{};
    DlssPreset presetOverride = DlssPreset::Auto;
    H265EncoderOptions h265Options{};
    int writerThreads = 4;
    int writerQueueMultiplier = 3;
    int fgMultiplier = 2;  // 2..6, capped by NGX capability query
    std::function<bool()> wantsPreviewFrame;
    std::function<void(const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)> previewRgba8Callback;
    std::function<void(const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)> previewCallback;
};

/// SR+FG combined pipeline.
/// Stage order: 1(prefetch) -> 2(SR) -> 4(tonemap) -> 5(FG) -> 6/7(optional inverse + write)
///
/// CRITICAL INVARIANT: depth/MV textures are acquired ONCE in Stage 1 and shared
/// by BOTH Stage 2 (SR) AND Stage 5 (FG). They must be the SAME PooledTextureHandle.
/// Per DLSS FG §6 L545: depth shared; §6 L555: motion vectors shared.
///
/// CRITICAL ORDER:
/// - SR (Stage 2) runs BEFORE tonemap (§3.1 — SR input must be scene-linear, before EOTF)
/// - Tonemap (Stage 4) runs BEFORE FG (§HDR — FG requires display-ready backbuffer)
/// - FG generated frames stay display-ready for PNG/ProRes; EXR output uses inverse to return to scene-linear.
class SrFgPipeline {
public:
    SrFgPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher);
    ~SrFgPipeline();

    void run(const SrFgConfig& config);
    void stop() { m_stop.store(true); }

private:
    bool shouldReset(int frameIdx, const DecodedFrame& frame) const;

    VulkanContext* m_ctx;
    NgxContext*    m_ngx;
    RpcDispatcher* m_dispatcher;
    std::atomic<bool> m_stop{false};
};

} // namespace dlss
