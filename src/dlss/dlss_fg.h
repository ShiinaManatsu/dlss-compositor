#pragma once
#include <vulkan/vulkan.h>
#include "../core/types.h"
#include "ngx_wrapper.h"
#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"
#include <vector>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace dlss {

/// Per-frame inputs to DLSS FG eval
struct FgInputs {
    VkImageView  backbufferDisplayReady;  // FG §6 L539 — post-tonemap+EOTF display-ready
    VkImageView  depthNdcRenderRes;       // FG §6 L545 — same depth as SR (render res)
    VkImageView  motionVecDlssRenderRes;  // FG §6 L555 — same MV as SR (render res)
    std::vector<VkImageView> outputInterp; // FG output interpolated frames (multiplier-1 frames)

    // Optional image metadata for constructing NGX Vulkan resources when tests/integration
    // do not have the owning VkImage at hand yet. T37/T38 can fill exact handles.
    VkImage backbufferImage = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkImage motionVecImage = VK_NULL_HANDLE;
    std::vector<VkImage> outputInterpImages;
    VkFormat backbufferFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    VkFormat depthFormat = VK_FORMAT_R32_SFLOAT;
    VkFormat motionVecFormat = VK_FORMAT_R16G16_SFLOAT;
    uint32_t width = 0;
    uint32_t height = 0;

    // Camera matrices from dlssconfig per-frame data
    float viewToClip[4][4];
    float clipToPrevClip[4][4];
    float prevClipToClip[4][4];

    float camNear;
    float camFar;
    float fov;          // Field of view in degrees
    float aspectRatio;
    float camPosX, camPosY, camPosZ;
    float camUpX, camUpY, camUpZ;
    float camRightX, camRightY, camRightZ;
    float camFwdX, camFwdY, camFwdZ;

    bool is_hdr;        // FG §HDR — selects HDR path (PQ backbuffer)
    bool reset;         // FG §6 L587 — camera cut / scene change
    int  multiplier;    // 2/3/4 for FG 2x/3x/4x
};

/// Stage 5: DLSS Frame Generation evaluator (NGX-direct, no Streamline).
/// Multi-frame mode: for multiplier=2, calls evaluate multiplier-1=1 times per input.
/// For multiplier=4, calls evaluate 3 times per input (FG §1 L518).
class DlssFgPass {
public:
    DlssFgPass(NgxContext* ngx, VulkanContext* ctx, TexturePool* pool);
    ~DlssFgPass();

    /// Create the NGX FG feature.
    /// @param multiplier  2/3/4 for 2x/3x/4x frame generation
    void create(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, int multiplier);

    /// Evaluate FG, generating (multiplier-1) interpolated frames per call.
    void evaluate(VkCommandBuffer cmd, const FgInputs& in);

    void destroy();
    bool isCreated() const { return m_handle != nullptr; }

private:
    NgxContext*    m_ngx;
    VulkanContext* m_ctx;
    TexturePool*   m_pool;
    NVSDK_NGX_Handle*    m_handle    = nullptr;
    NVSDK_NGX_Parameter* m_ngxParams = nullptr;
    int m_multiplier = 2;
};

} // namespace dlss
