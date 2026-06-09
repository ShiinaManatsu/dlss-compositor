#pragma once
#include "../core/dlssconfig.h"
#include "../dlss/dlss_fg.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"
#include "../gpu/passes/depth_pass.h"
#include "../gpu/passes/mv_pass.h"
#include "../gpu/passes/tonemap_pass.h"
#include "../gpu/passes/inverse_tonemap_pass.h"
#include "../io/frame_prefetcher.h"
#include "../io/channel_mapper.h"
#include "../io/async_writer.h"
#include "../rpc/dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dlss {

struct FgOnlyConfig {
    DlssConfig dlssConfig;
    OutputFormat outputFormat = OutputFormat::ExrLinear;
    std::string inputDir;
    std::string outputDir;
    std::string filenameTemplate = "frame_####.exr";
    OverwriteMode overwriteMode = OverwriteMode::Overwrite;
    int multiplier = 2;  // 2/3/4 for 2x/3x/4x
    std::function<bool()> wantsPreviewFrame;
    std::function<void(const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)> previewRgba8Callback;
    std::function<void(const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)> previewCallback;
};

/// FG-only pipeline: Stage 1 → Stage 4 (tonemap) → Stage 5 (FG) → Stage 6 (inverse) → Stage 7 (write)
/// Scale = 1.0 (no SR upscaling). Tonemap BEFORE FG, inverse tonemap AFTER FG for EXR output.
/// Output frame count = 2N-1 (2x) or 4N-3 (4x) renumbered sequentially.
class FgOnlyPipeline {
public:
    FgOnlyPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher);
    ~FgOnlyPipeline();

    void run(const FgOnlyConfig& config);
    void stop() { m_stop.store(true); }

private:
    VulkanContext* m_ctx;
    NgxContext*    m_ngx;
    RpcDispatcher* m_dispatcher;
    std::atomic<bool> m_stop{false};
};

} // namespace dlss
