#pragma once
#include <vulkan/vulkan.h>
#include "../core/types.h"
#include "ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace dlss {

/// Per-frame inputs to DLSS SR eval
struct SrInputs {
    VkImageView sceneColorRenderRes;   // Scene-linear color at render resolution
    VkImageView sceneColorOutputRes;   // Output image (at target resolution)
    VkImageView depthNdc;              // NDC depth [0,1] at render resolution
    VkImageView motionVecDlss;         // 2ch DLSS MV at render resolution
    VkImage sceneColorRenderImage;
    VkImage sceneColorOutputImage;
    VkImage depthImage;
    VkImage motionVecImage;
    uint32_t renderW;
    uint32_t renderH;
    uint32_t outputW;
    uint32_t outputH;
    float jitterX;                     // DLSS SR §3.7 — pixel space [-0.5, +0.5]
    float jitterY;                     // DLSS top-left coordinates (+Y down)
    bool reset;                        // DLSS SR §3.13 — first frame / gap / camera cut
    float frameTimeDeltaMs;            // DLSS SR §3.14 — 1000.0f / fps (NEVER hardcode)
};

/// Stage 2: DLSS Super Resolution evaluator (NGX-direct, no Streamline).
/// Per DLSS SR §3.5 Create + §3.6 Eval.
class DlssSrPass {
public:
    DlssSrPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool);
    ~DlssSrPass();

    /// Create the NGX SR feature for given resolutions + quality mode.
    /// Must be called before the first evaluate().
    void create(uint32_t inW,
                uint32_t inH,
                uint32_t outW,
                uint32_t outH,
                QualityMode quality,
                DlssPreset presetOverride = DlssPreset::Auto);

    /// Evaluate SR for one frame.
    void evaluate(VkCommandBuffer cmd, const SrInputs& in);

    /// Release the NGX SR feature handle.
    void destroy();

    bool isCreated() const { return m_handle != nullptr; }

private:
    static int toNgxQuality(QualityMode q);
    static void setPresetForQuality(NVSDK_NGX_Parameter* params, QualityMode q, DlssPreset presetOverride);

    NgxContext*   m_ngx;
    VulkanContext* m_ctx;
    TexturePool*  m_pool;
    NVSDK_NGX_Handle*    m_handle    = nullptr;
    NVSDK_NGX_Parameter* m_ngxParams = nullptr;
};

} // namespace dlss
