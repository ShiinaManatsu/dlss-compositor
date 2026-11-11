#pragma once
// DlssNrPass — DLSSNR (NGX feature 18) neural same-resolution denoiser.
//
// Experimental. Contract reverse-engineered from the Magpie fork
// (SAOG0721/Magpie @ 8dfdf37, DLSSNRFilter.cpp — see docs/research/magpie-nr-notes.md):
//   - Feature ID 18, provided by nvngx_dlssnr.dll (310.9.16 / 310.8) located next to
//     the exe. The signed snippet verifies its caller (GetModuleFileNameW must
//     report "nvngx.dll") — dlss_nr.cpp installs an IAT shim for that.
//   - Same-resolution, SDR RGBA8_UNORM in/out (display domain).
//   - Guidance: "DLSSNR.MVec" (RG16F pixel-unit) + "DLSSNR.Depth" (R32 NDC,
//     DepthInverted=1). Unlike Magpie we feed REAL guidance, not zero-filled
//     stand-ins, so its synthetic depth-inference cadence does not apply.
//   - All parameters are string keys ("DLSSNR.*") on the capability parameter
//     block; resources are passed as NVSDK_NGX_Resource_VK pointers.
//   - Tuning params (style/intensity/tone/structure/automask) are re-set on
//     every evaluate — the standalone NR tool retunes between previews.
//
// Failure to create = the feature is unavailable; callers must surface that as
// a hard error (no fallback).

#include "../gpu/vulkan_context.h"
#include "../gpu/texture_pool.h"

#include <volk.h>

#include <nvsdk_ngx_vk.h>      // NVSDK_NGX_Result / Handle / Version, VK entry types
#include <nvsdk_ngx_params.h>  // NVSDK_NGX_Parameter
#include <nvsdk_ngx_defs.h>    // NVSDK_NGX_FeatureDiscoveryInfo

#include <cstdint>
#include <string>

namespace dlss {

class NgxContext;

/// Function-pointer table for the nvngx_dlssnr.dll snippet exports
/// (signatures mirror nvsdk_ngx_vk.h declarations; feature 18 is driven
/// through the snippet directly, not through the NGX core).
struct NrSnippetTable {
    void*    library = nullptr;
    bool     hasLightweightModel = false;
    NVSDK_NGX_Result (NVSDK_CONV *init)(unsigned long long, const wchar_t*,
                                        VkInstance, VkPhysicalDevice, VkDevice,
                                        PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr,
                                        NVSDK_NGX_Version, const NVSDK_NGX_Parameter*) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *create)(VkCommandBuffer, NVSDK_NGX_Feature,
                                          NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *evaluate)(VkCommandBuffer, const NVSDK_NGX_Handle*,
                                            NVSDK_NGX_Parameter*,
                                            PFN_NVSDK_NGX_ProgressCallback) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *release)(NVSDK_NGX_Handle*) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *shutdown)(VkDevice) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *instanceExtReqs)(const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                   std::uint32_t*,
                                                   VkExtensionProperties**) = nullptr;
    NVSDK_NGX_Result (NVSDK_CONV *deviceExtReqs)(const VkInstance, const VkPhysicalDevice,
                                                 const NVSDK_NGX_FeatureDiscoveryInfo*,
                                                 std::uint32_t*,
                                                 VkExtensionProperties**) = nullptr;
};

/// Creation-time model selection. Auto follows the runtime default (Magpie v0.6.11).
enum class NrModel : int { Auto = 0, Standard = 1, Lightweight = 2 };

/// NR "style" presets exposed by the snippet (Magpie DLSSNRSettings.style).
enum class NrStyle : int {
    Default   = 0,
    Natural   = 1,
    Cinematic = 2,
};

/// Guidance mask (Magpie guidanceMode): which guides reach the feature.
enum class NrGuidanceMode : int {
    All        = 0,  ///< MV + depth
    ForceZero  = 1,  ///< neither (blind temporal filter)
    MotionOnly = 2,  ///< MV only
    DepthOnly  = 3,  ///< depth only
};

/// Native NGX tuning keys, independent of Magpie's additional processing.
struct NrSettings {
    bool           enabled            = true;
    NrModel        model              = NrModel::Auto;
    NrStyle        style              = NrStyle::Default;
    float          intensity          = 1.0f;
    float          localToneStrength  = 1.0f;
    float          localStructureStrength = 1.0f;
    float          skinStructureStrength  = 0.0f;
    bool           useAutoMask        = false;
    bool           uiCorrection       = false;
    NrGuidanceMode guidanceMode       = NrGuidanceMode::All;
};

struct NrInputs {
    // Display-ready color (R8G8B8A8_UNORM, output resolution).
    VkImageView colorView;
    VkImage     colorImage;
    VkImageView outputView;
    VkImage     outputImage;

    // Guidance at render resolution (subrect communicates the smaller extent).
    // MV: R16G16_SFLOAT pixel-unit DLSS space (same buffer as SR/RR/FG consume).
    // Depth: R32_SFLOAT NDC (inverted — DLSSNR.DepthInverted = 1).
    VkImageView mvView;
    VkImage     mvImage;
    VkImageView depthView;
    VkImage     depthImage;

    uint32_t renderWidth;
    uint32_t renderHeight;

    bool reset;  ///< first frame / frame gap / camera cut
};

class DlssNrPass {
public:
    DlssNrPass(NgxContext* ngx, VulkanContext* ctx);
    ~DlssNrPass();

    DlssNrPass(const DlssNrPass&) = delete;
    DlssNrPass& operator=(const DlssNrPass&) = delete;

    /// Create the feature. Throws std::runtime_error when nvngx_dlssnr.dll /
    /// feature 18 is unavailable (this is the availability probe).
    void create(uint32_t width, uint32_t height, const NrSettings& settings);

    /// Retune without recreating (NR tool preview path).
    void updateSettings(const NrSettings& settings);

    void evaluate(VkCommandBuffer cmd, const NrInputs& in);

    void destroy();

private:
    void applyTuningParams();

    NgxContext*          m_ngx;
    VulkanContext*       m_ctx;
    NVSDK_NGX_Parameter* m_ngxParams = nullptr;
    NVSDK_NGX_Handle*    m_handle    = nullptr;
    NrSnippetTable       m_snippet;
    NrSettings           m_settings;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace dlss
