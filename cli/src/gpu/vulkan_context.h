#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <stdexcept>
#include <vector>
#include <cstdint>
#include <cstddef>

namespace dlss {

/// Single-GPU compute-only Vulkan context for DLSS offline rendering.
/// No swapchain, no surface, no window. Pure headless compute.
class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    // Non-copyable, non-movable
    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Accessors
    VkInstance       instance()        const { return m_instance; }
    VkPhysicalDevice physicalDevice()  const { return m_physicalDevice; }
    VkDevice         device()          const { return m_device; }
    VmaAllocator     allocator()       const { return m_allocator; }

    uint32_t computeQueueFamily()  const { return m_computeQueueFamily; }
    uint32_t transferQueueFamily() const { return m_transferQueueFamily; }
    VkQueue  computeQueue()        const { return m_computeQueue; }
    VkQueue  transferQueue()       const { return m_transferQueue; }

    // Command helpers
    VkCommandBuffer beginOneTimeCommands(VkCommandPool pool) const;
    void            endOneTimeCommands(VkCommandPool pool, VkCommandBuffer cmd, VkQueue queue) const;
    std::vector<std::uint8_t> downloadImageRgba16f(
        VkImage image,
        uint32_t width,
        uint32_t height,
        VkImageLayout oldLayout = VK_IMAGE_LAYOUT_GENERAL) const;
    std::vector<std::uint8_t> downloadImageA2B10ToRgba16f(VkImage image,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          VkImageLayout oldLayout) const;
    std::vector<std::uint8_t> downloadImageRaw(VkImage image,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t bytesPerPixel,
                                               VkImageLayout oldLayout,
                                               bool restoreLayout = false) const;

    VkCommandPool   createTransferCommandPool() const;
    VkCommandPool   createComputeCommandPool() const;

    void waitIdle() const;

    void destroyCommandPool(VkCommandPool pool) const;
    void cmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* dependencyInfo) const;

    // Physical device info
    VkPhysicalDeviceProperties deviceProperties() const { return m_deviceProperties; }

private:
    void createInstance(const std::vector<const char*>& extraExtensions);
    void selectPhysicalDevice();
    void createLogicalDevice(const std::vector<const char*>& extraDeviceExtensions);
    void createAllocator();
    bool isNvidiaRtx(VkPhysicalDevice pd) const;
    void setupDebugMessenger();

    VkInstance               m_instance         = VK_NULL_HANDLE;
    VkPhysicalDevice         m_physicalDevice   = VK_NULL_HANDLE;
    VkDevice                 m_device           = VK_NULL_HANDLE;
    VmaAllocator             m_allocator        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger   = VK_NULL_HANDLE;

    uint32_t  m_computeQueueFamily  = UINT32_MAX;
    uint32_t  m_transferQueueFamily = UINT32_MAX;
    VkQueue   m_computeQueue        = VK_NULL_HANDLE;
    VkQueue   m_transferQueue       = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties m_deviceProperties{};
};

} // namespace dlss
