#pragma once

#include "../core/dlssconfig.h"
#include "../dlss/dlss_sr.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"
#include "../io/frame_prefetcher.h"
#include "../io/channel_mapper.h"
#include "../gpu/passes/depth_pass.h"
#include "../gpu/passes/mv_pass.h"
#include "../io/async_writer.h"
#include "../rpc/dispatcher.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dlss {

struct SrOnlyConfig {
    DlssConfig dlssConfig;
    OutputFormat outputFormat = OutputFormat::ExrLinear;
    std::string inputDir;
    std::string outputDir;
    std::string filenameTemplate = "frame_####.exr";
    OverwriteMode overwriteMode = OverwriteMode::Overwrite;
    std::function<void(const std::vector<std::uint8_t>&, std::uint32_t, std::uint32_t)> previewCallback;
};

/// SR-only pipeline: Stage 1 (prefetch+map+convert) → Stage 2 (SR) → Stage 7 (write)
/// Does NOT call Stage 4/5/6 (tonemap/FG/inverse tonemap — those are FG pipeline only)
class SrOnlyPipeline {
public:
    SrOnlyPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher);
    ~SrOnlyPipeline();

    /// Run the full SR-only pipeline. Blocks until all frames processed.
    void run(const SrOnlyConfig& config);

    /// Stop processing (called from another thread).
    void stop() { m_stop.store(true); }

private:
    bool shouldReset(int frameIdx, const DecodedFrame& frame,
                     const float* prevMatrix, const float* currMatrix) const;

    VulkanContext* m_ctx;
    NgxContext*    m_ngx;
    RpcDispatcher* m_dispatcher;
    std::atomic<bool> m_stop{false};
};

} // namespace dlss
