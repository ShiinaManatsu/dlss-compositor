#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1

#include "gpu/vulkan_context.h"

#include <GLFW/glfw3.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_vk.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;
constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
    constexpr const char* kNoCompatibleGpuMessage =
    "Error: No compatible NVIDIA RTX GPU found. DLSS Super Resolution requires an NVIDIA RTX GPU.";
constexpr wchar_t kNgxWorkDir[] = L".";
constexpr char kNgxProjectId[] = "6c6f53ec-6f25-4f9f-8d71-2f0f3c5e7a11";
constexpr char kNgxEngineVersion[] = "0.1.0";

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
    void*) {
    const char* severity = "INFO";
    if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        severity = "ERROR";
    } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        severity = "WARN";
    } else if ((messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        severity = "VERBOSE";
    }

    std::fprintf(stderr,
                 "[Vulkan %s] %s\n",
                 severity,
                 callbackData != nullptr && callbackData->pMessage != nullptr ? callbackData->pMessage : "(no message)");
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT makeDebugMessengerCreateInfo() {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    return createInfo;
}

bool containsCaseInsensitive(const std::string& value, const char* needle) {
    std::string upperValue(value.size(), '\0');
    std::transform(value.begin(), value.end(), upperValue.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    std::string upperNeedle(needle != nullptr ? needle : "");
    std::transform(upperNeedle.begin(), upperNeedle.end(), upperNeedle.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });

    return upperValue.find(upperNeedle) != std::string::npos;
}

void addUniqueExtension(std::vector<const char*>& extensions, const char* extension) {
    if (extension == nullptr) {
        return;
    }

    if (std::find_if(extensions.begin(), extensions.end(), [extension](const char* current) {
            return current != nullptr && std::strcmp(current, extension) == 0;
        }) == extensions.end()) {
        extensions.push_back(extension);
    }
}

bool hasNamedLayer(const std::vector<VkLayerProperties>& layers, const char* name) {
    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& layer) {
        return std::strcmp(layer.layerName, name) == 0;
    });
}

bool hasNamedExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

bool populateNgxFeatureDiscoveryInfo(NVSDK_NGX_FeatureDiscoveryInfo& featureInfo) {
    featureInfo = {};
    featureInfo.SDKVersion = NVSDK_NGX_Version_API;
    featureInfo.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    featureInfo.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    featureInfo.Identifier.v.ProjectDesc.ProjectId = kNgxProjectId;
    featureInfo.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    featureInfo.Identifier.v.ProjectDesc.EngineVersion = kNgxEngineVersion;
    featureInfo.ApplicationDataPath = kNgxWorkDir;
    featureInfo.FeatureInfo = nullptr;
    return true;
}

bool addNgxRequiredInstanceExtensions(std::vector<const char*>& enabledExtensions,
                                      const std::vector<VkExtensionProperties>& availableInstanceExtensions,
                                      std::string& errorMsg) {
    NVSDK_NGX_FeatureDiscoveryInfo featureInfo{};
    populateNgxFeatureDiscoveryInfo(featureInfo);

    uint32_t extensionCount = 0;
    VkExtensionProperties* extensionProperties = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&featureInfo,
                                                                                              &extensionCount,
                                                                                              &extensionProperties);
    if (NVSDK_NGX_FAILED(result)) {
        errorMsg = "Failed to query required NGX Vulkan instance extensions.";
        return false;
    }

    for (uint32_t i = 0; i < extensionCount; ++i) {
        const char* extensionName = extensionProperties[i].extensionName;
        if (!hasNamedExtension(availableInstanceExtensions, extensionName)) {
            errorMsg = std::string("Required NGX Vulkan instance extension is not available: ") + extensionName;
            return false;
        }
        addUniqueExtension(enabledExtensions, extensionName);
    }

    return true;
}

bool addNgxRequiredDeviceExtensions(VkInstance instance,
                                    VkPhysicalDevice physicalDevice,
                                    std::vector<const char*>& enabledDeviceExtensions,
                                    const std::vector<VkExtensionProperties>& availableDeviceExtensions,
                                    std::string& errorMsg) {
    NVSDK_NGX_FeatureDiscoveryInfo featureInfo{};
    populateNgxFeatureDiscoveryInfo(featureInfo);

    uint32_t extensionCount = 0;
    VkExtensionProperties* extensionProperties = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance,
                                                                                            physicalDevice,
                                                                                            &featureInfo,
                                                                                            &extensionCount,
                                                                                            &extensionProperties);
    if (NVSDK_NGX_FAILED(result)) {
        errorMsg = "Failed to query required NGX Vulkan device extensions.";
        return false;
    }

    for (uint32_t i = 0; i < extensionCount; ++i) {
        const char* extensionName = extensionProperties[i].extensionName;
        if (!hasNamedExtension(availableDeviceExtensions, extensionName)) {
            errorMsg = std::string("Required NGX Vulkan device extension is not available: ") + extensionName;
            return false;
        }
        addUniqueExtension(enabledDeviceExtensions, extensionName);
    }

    return true;
}

bool queryQueueFamilyIndices(VkPhysicalDevice physicalDevice,
                             uint32_t& computeQueueFamily,
                             uint32_t& graphicsQueueFamily) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    if (queueFamilyCount == 0) {
        return false;
    }

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t fallbackComputeQueueFamily = VK_QUEUE_FAMILY_IGNORED;

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const VkQueueFlags flags = queueFamilies[i].queueFlags;
        if (queueFamilies[i].queueCount == 0) {
            continue;
        }

        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0 && graphicsQueueFamily == VK_QUEUE_FAMILY_IGNORED) {
            graphicsQueueFamily = i;
        }

        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0) {
            if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                computeQueueFamily = i;
                break;
            }
            if (fallbackComputeQueueFamily == VK_QUEUE_FAMILY_IGNORED) {
                fallbackComputeQueueFamily = i;
            }
        }
    }

    if (computeQueueFamily == VK_QUEUE_FAMILY_IGNORED) {
        computeQueueFamily = fallbackComputeQueueFamily;
    }

    return computeQueueFamily != VK_QUEUE_FAMILY_IGNORED && graphicsQueueFamily != VK_QUEUE_FAMILY_IGNORED;
}

bool isPreferredNvidiaDiscreteGpu(const VkPhysicalDeviceProperties& properties) {
    return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU &&
           (properties.vendorID == kNvidiaVendorId || containsCaseInsensitive(properties.deviceName, "NVIDIA"));
}

} // namespace

VulkanContext::VulkanContext() = default;

VulkanContext::~VulkanContext() {
    destroy();
}

bool VulkanContext::init(std::string& errorMsg) {
    if (m_initialized) {
        return true;
    }

    destroy();
    errorMsg.clear();

    const VkResult volkResult = volkInitialize();
    if (volkResult != VK_SUCCESS) {
        errorMsg = "Failed to initialize volk Vulkan loader. Is vulkan-1.dll accessible?";
        return false;
    }

    if (glfwInit() != GLFW_TRUE) {
        const char* glfwError = nullptr;
        glfwGetError(&glfwError);
        errorMsg = std::string("Failed to initialize GLFW") +
                   (glfwError != nullptr ? std::string(": ") + glfwError : std::string("."));
        return false;
    }
    m_glfwInitialized = true;

    if (glfwVulkanSupported() != GLFW_TRUE) {
        errorMsg = "GLFW reports that Vulkan is not supported on this system.";
        destroy();
        return false;
    }

    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerProperties(&layerCount, nullptr) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan instance layers.";
        destroy();
        return false;
    }

    std::vector<VkLayerProperties> availableLayers(layerCount);
    if (layerCount > 0 && vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data()) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan instance layers.";
        destroy();
        return false;
    }

    uint32_t instanceExtensionCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan instance extensions.";
        destroy();
        return false;
    }

    std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
    if (instanceExtensionCount > 0 &&
        vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, availableInstanceExtensions.data()) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan instance extensions.";
        destroy();
        return false;
    }

    uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr || glfwExtensionCount == 0) {
        errorMsg = "Failed to query required Vulkan instance extensions from GLFW.";
        destroy();
        return false;
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.reserve(glfwExtensionCount + 3);
    for (uint32_t i = 0; i < glfwExtensionCount; ++i) {
        addUniqueExtension(enabledExtensions, glfwExtensions[i]);
    }

    addUniqueExtension(enabledExtensions, VK_KHR_SURFACE_EXTENSION_NAME);
    addUniqueExtension(enabledExtensions, VK_KHR_WIN32_SURFACE_EXTENSION_NAME);

    uint32_t instanceApiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion != nullptr) {
        vkEnumerateInstanceVersion(&instanceApiVersion);
    }

    if (hasNamedExtension(availableInstanceExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME)) {
        addUniqueExtension(enabledExtensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    } else if (VK_API_VERSION_MAJOR(instanceApiVersion) < 1 ||
               (VK_API_VERSION_MAJOR(instanceApiVersion) == 1 && VK_API_VERSION_MINOR(instanceApiVersion) < 1)) {
        errorMsg = "Required Vulkan instance extension VK_KHR_get_physical_device_properties2 is not available.";
        destroy();
        return false;
    }

    if (!addNgxRequiredInstanceExtensions(enabledExtensions, availableInstanceExtensions, errorMsg)) {
        destroy();
        return false;
    }

#ifndef NDEBUG
    std::vector<const char*> enabledLayers;
    if (hasNamedLayer(availableLayers, kValidationLayerName)) {
        enabledLayers.push_back(kValidationLayerName);
        m_validationEnabled = true;
        if (hasNamedExtension(availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            addUniqueExtension(enabledExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr,
                         "Warning: Vulkan validation layer available but %s extension is missing; debug messenger disabled.\n",
                         VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    } else {
        std::fprintf(stderr, "Warning: Vulkan validation layer %s is not available.\n", kValidationLayerName);
    }
#endif

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dlss-compositor";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.pEngineName = "dlss-compositor";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instanceCreateInfo{};
    instanceCreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceCreateInfo.pApplicationInfo = &appInfo;
    instanceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
    instanceCreateInfo.ppEnabledExtensionNames = enabledExtensions.data();

#ifndef NDEBUG
    instanceCreateInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
    instanceCreateInfo.ppEnabledLayerNames = enabledLayers.data();
    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (m_validationEnabled && hasNamedExtension(availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        debugCreateInfo = makeDebugMessengerCreateInfo();
        instanceCreateInfo.pNext = &debugCreateInfo;
    }
#endif

    if (vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan instance.";
        destroy();
        return false;
    }

    volkLoadInstance(m_instance);

#ifndef NDEBUG
    if (m_validationEnabled && vkCreateDebugUtilsMessengerEXT != nullptr &&
        hasNamedExtension(availableInstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        const VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = makeDebugMessengerCreateInfo();
        if (vkCreateDebugUtilsMessengerEXT(m_instance, &debugCreateInfo, nullptr, &m_debugMessenger) != VK_SUCCESS) {
            std::fprintf(stderr, "Warning: Failed to create Vulkan debug messenger.\n");
        }
    }
#endif

    uint32_t physicalDeviceCount = 0;
    if (vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, nullptr) != VK_SUCCESS || physicalDeviceCount == 0) {
        errorMsg = kNoCompatibleGpuMessage;
        destroy();
        return false;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    if (vkEnumeratePhysicalDevices(m_instance, &physicalDeviceCount, physicalDevices.data()) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan physical devices.";
        destroy();
        return false;
    }

    VkPhysicalDevice selectedDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties selectedProperties{};
    uint32_t selectedComputeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t selectedGraphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;

    for (VkPhysicalDevice physicalDevice : physicalDevices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(physicalDevice, &features);
        (void)features;

        uint32_t computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        uint32_t graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
        if (!queryQueueFamilyIndices(physicalDevice, computeQueueFamily, graphicsQueueFamily)) {
            continue;
        }

        if (isPreferredNvidiaDiscreteGpu(properties)) {
            selectedDevice = physicalDevice;
            selectedProperties = properties;
            selectedComputeQueueFamily = computeQueueFamily;
            selectedGraphicsQueueFamily = graphicsQueueFamily;
            break;
        }
    }

    if (selectedDevice == VK_NULL_HANDLE) {
        errorMsg = kNoCompatibleGpuMessage;
        destroy();
        return false;
    }

    m_physicalDevice = selectedDevice;
    m_computeQueueFamily = selectedComputeQueueFamily;
    m_graphicsQueueFamily = selectedGraphicsQueueFamily;
    m_gpuName = selectedProperties.deviceName;

    uint32_t deviceExtensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &deviceExtensionCount, nullptr) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan device extensions.";
        destroy();
        return false;
    }

    std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
    if (deviceExtensionCount > 0 &&
        vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data()) != VK_SUCCESS) {
        errorMsg = "Failed to enumerate Vulkan device extensions.";
        destroy();
        return false;
    }

    std::vector<const char*> enabledDeviceExtensions;
    for (const char* extensionName : std::array<const char*, 2>{VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_MAINTENANCE1_EXTENSION_NAME}) {
        if (hasNamedExtension(availableDeviceExtensions, extensionName)) {
            addUniqueExtension(enabledDeviceExtensions, extensionName);
        } else {
            std::fprintf(stderr, "Warning: Vulkan device extension not available: %s\n", extensionName);
        }
    }

    if (!addNgxRequiredDeviceExtensions(m_instance,
                                        m_physicalDevice,
                                        enabledDeviceExtensions,
                                        availableDeviceExtensions,
                                        errorMsg)) {
        destroy();
        return false;
    }

    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(m_computeQueueFamily == m_graphicsQueueFamily ? 1 : 2);

    std::vector<uint32_t> uniqueQueueFamilies;
    uniqueQueueFamilies.push_back(m_computeQueueFamily);
    if (m_graphicsQueueFamily != m_computeQueueFamily) {
        uniqueQueueFamilies.push_back(m_graphicsQueueFamily);
    }

    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.pEnabledFeatures = &deviceFeatures;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();

    if (vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan logical device.";
        destroy();
        return false;
    }

    volkLoadDevice(m_device);

    vkGetDeviceQueue(m_device, m_computeQueueFamily, 0, &m_computeQueue);
    vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);

    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = m_computeQueueFamily;
    if (vkCreateCommandPool(m_device, &commandPoolCreateInfo, nullptr, &m_commandPool) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan command pool.";
        destroy();
        return false;
    }

    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = m_commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(m_device, &commandBufferAllocateInfo, &m_commandBuffer) != VK_SUCCESS) {
        errorMsg = "Failed to allocate Vulkan command buffer.";
        destroy();
        return false;
    }

    VmaVulkanFunctions vmaFunctions{};
    vmaFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vmaFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorCreateInfo{};
    allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorCreateInfo.instance = m_instance;
    allocatorCreateInfo.physicalDevice = m_physicalDevice;
    allocatorCreateInfo.device = m_device;
    allocatorCreateInfo.pVulkanFunctions = &vmaFunctions;

    if (vmaCreateAllocator(&allocatorCreateInfo, &m_allocator) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan Memory Allocator.";
        destroy();
        return false;
    }

    m_initialized = true;
    return true;
}

void VulkanContext::destroy() {
    m_initialized = false;

    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
    }

    if (m_allocator != nullptr) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = nullptr;
    }

    if (m_commandPool != VK_NULL_HANDLE && m_device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        m_commandPool = VK_NULL_HANDLE;
    }
    m_commandBuffer = VK_NULL_HANDLE;
    m_computeQueue = VK_NULL_HANDLE;
    m_graphicsQueue = VK_NULL_HANDLE;

    if (m_device != VK_NULL_HANDLE) {
        vkDestroyDevice(m_device, nullptr);
        m_device = VK_NULL_HANDLE;
    }

    if (m_debugMessenger != VK_NULL_HANDLE && m_instance != VK_NULL_HANDLE && vkDestroyDebugUtilsMessengerEXT != nullptr) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
        m_debugMessenger = VK_NULL_HANDLE;
    }

    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
        m_instance = VK_NULL_HANDLE;
    }

    if (m_glfwInitialized) {
        glfwTerminate();
        m_glfwInitialized = false;
    }

    m_physicalDevice = VK_NULL_HANDLE;
    m_computeQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    m_graphicsQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    m_validationEnabled = false;
    m_gpuName.clear();
}

bool VulkanContext::isInitialized() const {
    return m_initialized;
}

VkInstance VulkanContext::instance() const {
    return m_instance;
}

VkPhysicalDevice VulkanContext::physicalDevice() const {
    return m_physicalDevice;
}

VkDevice VulkanContext::device() const {
    return m_device;
}

VkQueue VulkanContext::computeQueue() const {
    return m_computeQueue;
}

uint32_t VulkanContext::computeQueueFamily() const {
    return m_computeQueueFamily;
}

VkCommandPool VulkanContext::commandPool() const {
    return m_commandPool;
}

VmaAllocator VulkanContext::allocator() const {
    return m_allocator;
}

std::string VulkanContext::gpuName() const {
    return m_gpuName;
}
