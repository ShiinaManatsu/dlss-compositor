#pragma once

#include <string>

struct GLFWwindow;
struct ImGuiContext;
typedef struct VkInstance_T* VkInstance;
typedef struct VkPhysicalDevice_T* VkPhysicalDevice;
typedef struct VkDevice_T* VkDevice;
typedef struct VkQueue_T* VkQueue;
typedef struct VkSurfaceKHR_T* VkSurfaceKHR;
typedef struct VkSwapchainKHR_T* VkSwapchainKHR;
typedef struct VkRenderPass_T* VkRenderPass;
typedef struct VkCommandPool_T* VkCommandPool;
typedef struct VkDescriptorPool_T* VkDescriptorPool;
typedef struct VkPipelineCache_T* VkPipelineCache;
typedef struct VkSemaphore_T* VkSemaphore;
typedef struct VkFence_T* VkFence;

class App {
public:
    App();
    ~App();

    // testMode=true -> run 5 offscreen frames and exit (no window shown)
    bool run(bool testMode, std::string& errorMsg);

private:
    bool initWindow(bool testMode, std::string& errorMsg);
    bool initVulkan(std::string& errorMsg);
    bool initImGui();
    void cleanup();
    void frameRender();
    void framePresent();

    GLFWwindow* window = nullptr;
    ImGuiContext* imGuiContext = nullptr;

    VkInstance instance = nullptr;
    VkPhysicalDevice physicalDevice = nullptr;
    VkDevice device = nullptr;
    uint32_t queueFamily = (uint32_t)-1;
    VkQueue queue = nullptr;
    VkSurfaceKHR surface = nullptr;
    VkDescriptorPool descriptorPool = nullptr;
    VkRenderPass renderPass = nullptr;

    // We need a proper swapchain wrapper for simplicity, or just raw Vulkan.
    // Raw Vulkan is verbose but required since we can't use helper libraries.
    VkSwapchainKHR swapchain = nullptr;
    uint32_t minImageCount = 2;
    uint32_t imageCount = 0;
    
    // We'll dynamically allocate these arrays in app.cpp
    struct FrameData {
        VkCommandPool commandPool;
        struct VkCommandBuffer_T* commandBuffer;
        VkSemaphore imageAcquiredSemaphore;
        VkSemaphore renderCompleteSemaphore;
        VkFence fence;
    };
    struct FrameData* frames = nullptr;

    struct VkImage_T** swapchainImages = nullptr;
    struct VkImageView_T** swapchainImageViews = nullptr;
    struct VkFramebuffer_T** framebuffers = nullptr;

    uint32_t frameIndex = 0;
    uint32_t semaphoreIndex = 0;
    
    int width = 1280;
    int height = 720;
    bool swapchainRebuild = false;
    
    void createSwapchain(int w, int h);
    void cleanupSwapchain();
};
