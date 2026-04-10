#pragma once

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <volk.h>

#include <cstdint>
#include <string>

struct VmaAllocator_T;
using VmaAllocator = VmaAllocator_T*;

class VulkanContext {
public:
    VulkanContext();
    ~VulkanContext();

    bool init(std::string& errorMsg);
    void destroy();

    bool isInitialized() const;

    VkInstance instance() const;
    VkPhysicalDevice physicalDevice() const;
    VkDevice device() const;
    VkQueue computeQueue() const;
    uint32_t computeQueueFamily() const;
    VkCommandPool commandPool() const;
    VmaAllocator allocator() const;

    std::string gpuName() const;

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_computeQueue = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    uint32_t m_computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t m_graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkCommandBuffer m_commandBuffer = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    std::string m_gpuName;
    bool m_glfwInitialized = false;
    bool m_validationEnabled = false;
    bool m_initialized = false;
};
