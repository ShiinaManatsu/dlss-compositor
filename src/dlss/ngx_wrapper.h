#pragma once

#include "cli/config.h"

#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

#include <string>

struct VkInstance_T;
using VkInstance = VkInstance_T*;
struct VkPhysicalDevice_T;
using VkPhysicalDevice = VkPhysicalDevice_T*;
struct VkDevice_T;
using VkDevice = VkDevice_T*;
struct VkCommandBuffer_T;
using VkCommandBuffer = VkCommandBuffer_T*;

class NgxContext {
public:
    NgxContext();
    ~NgxContext();

    bool init(VkInstance instance,
              VkPhysicalDevice physicalDevice,
              VkDevice device,
              VkCommandBuffer cmdBuf,
              std::string& errorMsg);

    bool isDlssRRAvailable() const;
    std::string unavailableReason() const;

    bool createDlssRR(int inputWidth,
                      int inputHeight,
                      int outputWidth,
                      int outputHeight,
                      DlssQualityMode quality,
                      VkCommandBuffer cmdBuf,
                      std::string& errorMsg);

    void releaseDlssRR();
    bool isDlssFGAvailable() const;
    bool createDlssFG(unsigned int width,
                      unsigned int height,
                      unsigned int backbufferFormat,
                      VkCommandBuffer cmdBuf,
                      std::string& errorMsg);
    void releaseDlssFG();
    void shutdown();

    NVSDK_NGX_Handle* featureHandle() const;
    NVSDK_NGX_Handle* fgFeatureHandle() const;
    NVSDK_NGX_Handle* getFeatureHandle() const;
    NVSDK_NGX_Parameter* parameters() const;
    int maxMultiFrameCount() const;

    bool isInitialized() const;

private:
    NVSDK_NGX_PerfQuality_Value mapQuality(DlssQualityMode quality) const;
    bool queryDlssRRAvailability();
    bool queryDlssFGAvailability();

    NVSDK_NGX_Handle* m_featureHandle = nullptr;
    NVSDK_NGX_Parameter* m_parameters = nullptr;
    VkDevice m_device = nullptr;
    bool m_initialized = false;
    bool m_dlssRRAvailable = false;
    NVSDK_NGX_Handle* m_fgFeatureHandle = nullptr;
    bool m_dlssFGAvailable = false;
    int m_maxMultiFrameCount = 0;
    std::string m_unavailableReason;
};
