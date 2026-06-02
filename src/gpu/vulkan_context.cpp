#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "vulkan_context.h"
#include "../core/log.h"

// volk must be initialized before any Vulkan calls
#include <volk.h>

// Platform-specific external memory extension (required by NGX on Windows)
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan_win32.h>

// DLSS NGX Vulkan extension query
#include <nvsdk_ngx_vk.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace dlss {

namespace {

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* /*pUserData*/)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOG_ERROR("[Vulkan] {}", pCallbackData->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOG_WARN("[Vulkan] {}", pCallbackData->pMessage);
    else
        LOG_DEBUG("[Vulkan] {}", pCallbackData->pMessage);
    return VK_FALSE;
}

} // anonymous namespace

VulkanContext::VulkanContext() {
    // Init volk (loads Vulkan function pointers)
    VkResult r = volkInitialize();
    if (r != VK_SUCCESS) {
        throw std::runtime_error("volkInitialize failed — is Vulkan runtime installed?");
    }

    // Query NGX required instance extensions
    // DLSS SR §3.4 — must query before vkCreateInstance
    uint32_t ngxInstExtCount = 0;
    const char** ngxInstExts = nullptr;
    NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
        nullptr, &ngxInstExtCount, &ngxInstExts);
    
    std::vector<const char*> extraInstExts;
    for (uint32_t i = 0; i < ngxInstExtCount; ++i)
        extraInstExts.push_back(ngxInstExts[i]);

    createInstance(extraInstExts);
    volkLoadInstance(m_instance);

#ifdef _DEBUG
    setupDebugMessenger();
#endif

    selectPhysicalDevice();

    // Query NGX required device extensions
    // DLSS SR §3.4 — must query before vkCreateDevice
    uint32_t ngxDevExtCount = 0;
    const char** ngxDevExts = nullptr;
    NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        m_physicalDevice, nullptr, &ngxDevExtCount, &ngxDevExts);
    
    std::vector<const char*> extraDevExts;
    for (uint32_t i = 0; i < ngxDevExtCount; ++i)
        extraDevExts.push_back(ngxDevExts[i]);
    
    // Always add external memory win32 (required by NGX on Windows)
    extraDevExts.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    extraDevExts.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);

    createLogicalDevice(extraDevExts);
    volkLoadDevice(m_device);
    createAllocator();

    LOG_INFO("[VulkanContext] Initialized on GPU: {} (vendor=0x{:04X})",
             m_deviceProperties.deviceName, m_deviceProperties.vendorID);
}

VulkanContext::~VulkanContext() {
    if (m_device) vkDeviceWaitIdle(m_device);
    if (m_allocator) vmaDestroyAllocator(m_allocator);
    if (m_device) vkDestroyDevice(m_device, nullptr);
#ifdef _DEBUG
    if (m_debugMessenger) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            m_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(m_instance, m_debugMessenger, nullptr);
    }
#endif
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

void VulkanContext::createInstance(const std::vector<const char*>& extraExtensions) {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "DLSS Compositor";
    appInfo.applicationVersion = VK_MAKE_VERSION(2, 0, 0);
    appInfo.pEngineName = "DlssCompositorEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(2, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = extraExtensions;
    // Always add debug utils for validation in debug builds
#ifdef _DEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    std::vector<const char*> layers;
#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    VkResult r = vkCreateInstance(&createInfo, nullptr, &m_instance);
    if (r != VK_SUCCESS)
        throw std::runtime_error("vkCreateInstance failed: " + std::to_string(r));
}

void VulkanContext::selectPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0)
        throw std::runtime_error("No NVIDIA RTX GPU found — no physical Vulkan devices");

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    // Select first NVIDIA device (vendor ID 0x10DE)
    for (auto pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.vendorID == 0x10DE) {  // NVIDIA
            m_physicalDevice = pd;
            m_deviceProperties = props;
            return;
        }
    }
    throw std::runtime_error("No NVIDIA RTX GPU found — DLSS requires NVIDIA hardware");
}

void VulkanContext::createLogicalDevice(const std::vector<const char*>& extraDeviceExtensions) {
    // Find compute and transfer queue families
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        if ((queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            m_computeQueueFamily == UINT32_MAX) {
            m_computeQueueFamily = i;
        }
        if ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT) &&
            m_transferQueueFamily == UINT32_MAX) {
            m_transferQueueFamily = i;
        }
    }

    if (m_computeQueueFamily == UINT32_MAX)
        throw std::runtime_error("No graphics+compute queue family found");

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;

    auto addQueue = [&](uint32_t familyIndex) {
        for (const auto& qi : queueCreateInfos)
            if (qi.queueFamilyIndex == familyIndex) return;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = familyIndex;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        queueCreateInfos.push_back(qi);
    };
    addQueue(m_computeQueueFamily);
    if (m_transferQueueFamily != UINT32_MAX)
        addQueue(m_transferQueueFamily);

    // Enable synchronization2 feature
    VkPhysicalDeviceSynchronization2FeaturesKHR sync2Features{};
    sync2Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;
    sync2Features.synchronization2 = VK_TRUE;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &sync2Features;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext = &features2;
    createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extraDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames = extraDeviceExtensions.data();

    VkResult r = vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
    if (r != VK_SUCCESS)
        throw std::runtime_error("vkCreateDevice failed: " + std::to_string(r));

    vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
    if (m_transferQueueFamily != UINT32_MAX)
        vkGetDeviceQueue(m_device, m_transferQueueFamily, 0, &m_transferQueue);
    else
        m_transferQueue = m_computeQueue;
}

void VulkanContext::createAllocator() {
    VmaAllocatorCreateInfo allocInfo{};
    allocInfo.physicalDevice = m_physicalDevice;
    allocInfo.device = m_device;
    allocInfo.instance = m_instance;
    allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
    
    VkResult r = vmaCreateAllocator(&allocInfo, &m_allocator);
    if (r != VK_SUCCESS)
        throw std::runtime_error("vmaCreateAllocator failed: " + std::to_string(r));
}

void VulkanContext::setupDebugMessenger() {
    auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        m_instance, "vkCreateDebugUtilsMessengerEXT");
    if (!fn) return;

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                               | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                           | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    fn(m_instance, &createInfo, nullptr, &m_debugMessenger);
}

VkCommandBuffer VulkanContext::beginOneTimeCommands(VkCommandPool pool) const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

void VulkanContext::endOneTimeCommands(VkCommandPool pool, VkCommandBuffer cmd, VkQueue queue) const {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(m_device, pool, 1, &cmd);
}

VkCommandPool VulkanContext::createTransferCommandPool() const {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_transferQueueFamily != UINT32_MAX ?
                                m_transferQueueFamily : m_computeQueueFamily;
    VkCommandPool pool;
    vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool);
    return pool;
}

VkCommandPool VulkanContext::createComputeCommandPool() const {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_computeQueueFamily;
    VkCommandPool pool;
    vkCreateCommandPool(m_device, &poolInfo, nullptr, &pool);
    return pool;
}

void VulkanContext::waitIdle() const {
    vkDeviceWaitIdle(m_device);
}

} // namespace dlss
