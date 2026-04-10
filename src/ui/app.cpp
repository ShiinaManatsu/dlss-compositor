#include "app.h"

#include <iostream>
#include <vector>
#include <stdexcept>

#include <volk.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

static void glfw_error_callback(int error, const char* description) {
    std::cerr << "GLFW Error " << error << ": " << description << "\n";
}

App::App() {
    frames = new FrameData[minImageCount];
    for (uint32_t i = 0; i < minImageCount; i++) {
        frames[i] = {};
    }
}

App::~App() {
    delete[] frames;
}

bool App::run(bool testMode, std::string& errorMsg) {
    if (!initWindow(testMode, errorMsg)) return false;
    if (!initVulkan(errorMsg)) return false;
    if (!initImGui()) {
        errorMsg = "Failed to initialize ImGui";
        return false;
    }

    // Main loop
    if (testMode) {
        // Run exactly 5 frames for the test mode, without displaying a window
        for (int i = 0; i < 5; ++i) {
            glfwPollEvents();
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("Headless UI Test");
            ImGui::Text("DLSS Compositor v0.1");
            ImGui::End();

            ImGui::Render();
            frameRender();
            framePresent();
        }
    } else {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            if (swapchainRebuild) {
                int w, h;
                glfwGetFramebufferSize(window, &w, &h);
                if (w > 0 && h > 0) {
                    ImGui_ImplVulkan_SetMinImageCount(minImageCount);
                    createSwapchain(w, h);
                    frameIndex = 0;
                    swapchainRebuild = false;
                }
            }

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("DLSS Compositor Viewer");
            ImGui::Text("DLSS Compositor v0.1");
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 
                        1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();

            ImGui::Render();
            frameRender();
            framePresent();
        }
    }

    cleanup();
    return true;
}

bool App::initWindow(bool testMode, std::string& errorMsg) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        errorMsg = "Failed to initialize GLFW";
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (testMode) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    }

    window = glfwCreateWindow(width, height, "DLSS Compositor", nullptr, nullptr);
    if (!window) {
        errorMsg = "Failed to create GLFW window";
        return false;
    }

    if (!glfwVulkanSupported()) {
        errorMsg = "GLFW: Vulkan Not Supported";
        return false;
    }

    return true;
}

bool App::initVulkan(std::string& errorMsg) {
    if (volkInitialize() != VK_SUCCESS) {
        errorMsg = "Failed to initialize volk in UI";
        return false;
    }

    // Create Instance
    uint32_t extensions_count = 0;
    const char** extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    
    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "DLSS Compositor UI";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = extensions_count;
    createInfo.ppEnabledExtensionNames = extensions;

    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan instance for UI";
        return false;
    }
    volkLoadInstance(instance);

    // Create Surface
    if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan surface";
        return false;
    }

    // Pick Physical Device
    uint32_t gpu_count;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    
    for (VkPhysicalDevice gpu : gpus) {
        uint32_t qf_count;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qfps(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qf_count, qfps.data());

        for (uint32_t i = 0; i < qf_count; i++) {
            if (qfps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 present_support = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present_support);
                if (present_support) {
                    physicalDevice = gpu;
                    queueFamily = i;
                    break;
                }
            }
        }
        if (physicalDevice) break;
    }

    if (!physicalDevice) {
        errorMsg = "Failed to find a suitable physical device for UI";
        return false;
    }

    // Create Device
    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queue_priority;

    const char* device_extensions[] = { "VK_KHR_swapchain" };
    VkDeviceCreateInfo deviceInfo = {};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = device_extensions;

    if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
        errorMsg = "Failed to create Vulkan device for UI";
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    // Create Descriptor Pool
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
    };
    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = (uint32_t)std::size(pool_sizes);
    poolInfo.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool);

    // Render Pass
    VkAttachmentDescription attachment = {};
    attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_attachment = {};
    color_attachment.attachment = 0;
    color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &attachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;
    vkCreateRenderPass(device, &rpInfo, nullptr, &renderPass);

    createSwapchain(width, height);

    for (uint32_t i = 0; i < minImageCount; i++) {
        VkCommandPoolCreateInfo cmdPoolInfo = {};
        cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cmdPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cmdPoolInfo.queueFamilyIndex = queueFamily;
        vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &frames[i].commandPool);

        VkCommandBufferAllocateInfo cmdBufInfo = {};
        cmdBufInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cmdBufInfo.commandPool = frames[i].commandPool;
        cmdBufInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cmdBufInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cmdBufInfo, &frames[i].commandBuffer);

        VkSemaphoreCreateInfo semInfo = {};
        semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].imageAcquiredSemaphore);
        vkCreateSemaphore(device, &semInfo, nullptr, &frames[i].renderCompleteSemaphore);

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fenceInfo, nullptr, &frames[i].fence);
    }

    return true;
}

void App::createSwapchain(int w, int h) {
    if (swapchain) {
        cleanupSwapchain();
    }
    
    width = w;
    height = h;

    VkSwapchainCreateInfoKHR createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;
    createInfo.minImageCount = minImageCount;
    createInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    createInfo.imageExtent = { (uint32_t)w, (uint32_t)h };
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    createInfo.clipped = VK_TRUE;

    vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);

    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    swapchainImages = new VkImage[imageCount];
    vkGetSwapchainImagesKHR(device, swapchain, &imageCount, swapchainImages);

    swapchainImageViews = new VkImageView[imageCount];
    framebuffers = new VkFramebuffer[imageCount];

    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
        viewInfo.components.r = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.g = VK_COMPONENT_SWIZZLE_G;
        viewInfo.components.b = VK_COMPONENT_SWIZZLE_B;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_A;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;
        vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[i]);

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &swapchainImageViews[i];
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffers[i]);
    }
}

void App::cleanupSwapchain() {
    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroyFramebuffer(device, framebuffers[i], nullptr);
        vkDestroyImageView(device, swapchainImageViews[i], nullptr);
    }
    delete[] framebuffers;
    delete[] swapchainImageViews;
    delete[] swapchainImages;
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = nullptr;
}

bool App::initImGui() {
    IMGUI_CHECKVERSION();
    imGuiContext = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_2;
    init_info.Instance = instance;
    init_info.PhysicalDevice = physicalDevice;
    init_info.Device = device;
    init_info.QueueFamily = queueFamily;
    init_info.Queue = queue;
    init_info.PipelineCache = nullptr;
    init_info.DescriptorPool = descriptorPool;
    init_info.RenderPass = renderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = minImageCount;
    init_info.ImageCount = imageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;

    init_info.CheckVkResultFn = [](VkResult err) {
        if (err != VK_SUCCESS) std::cerr << "ImGui Vulkan Error: " << err << "\n";
    };

    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, [](const char* function_name, void* vulkan_instance) {
        return vkGetInstanceProcAddr(*(reinterpret_cast<VkInstance*>(vulkan_instance)), function_name);
    }, &instance);

    ImGui_ImplVulkan_Init(&init_info);
    
    // Upload Fonts
    ImGui_ImplVulkan_CreateFontsTexture();
    
    return true;
}

void App::cleanup() {
    if (device) vkDeviceWaitIdle(device);

    if (imGuiContext) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext(imGuiContext);
        imGuiContext = nullptr;
    }

    if (device) {
        cleanupSwapchain();

        for (uint32_t i = 0; i < minImageCount; i++) {
            vkDestroyFence(device, frames[i].fence, nullptr);
            vkDestroySemaphore(device, frames[i].imageAcquiredSemaphore, nullptr);
            vkDestroySemaphore(device, frames[i].renderCompleteSemaphore, nullptr);
            vkDestroyCommandPool(device, frames[i].commandPool, nullptr);
        }

        vkDestroyRenderPass(device, renderPass, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDevice(device, nullptr);
        device = nullptr;
    }

    if (instance) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);
        instance = nullptr;
    }

    if (window) {
        glfwDestroyWindow(window);
        window = nullptr;
    }
    glfwTerminate();
}

void App::frameRender() {
    FrameData* fd = &frames[frameIndex];
    vkWaitForFences(device, 1, &fd->fence, VK_TRUE, UINT64_MAX);
    
    VkResult err = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, fd->imageAcquiredSemaphore, VK_NULL_HANDLE, &semaphoreIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchainRebuild = true;
        return;
    }
    vkResetFences(device, 1, &fd->fence);

    vkResetCommandPool(device, fd->commandPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(fd->commandBuffer, &beginInfo);

    VkRenderPassBeginInfo rpInfo = {};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = renderPass;
    rpInfo.framebuffer = framebuffers[semaphoreIndex];
    rpInfo.renderArea.extent.width = width;
    rpInfo.renderArea.extent.height = height;
    VkClearValue clearColor = {{{0.1f, 0.1f, 0.1f, 1.0f}}};
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearColor;
    vkCmdBeginRenderPass(fd->commandBuffer, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->commandBuffer);

    vkCmdEndRenderPass(fd->commandBuffer);
    vkEndCommandBuffer(fd->commandBuffer);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &fd->imageAcquiredSemaphore;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd->commandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &fd->renderCompleteSemaphore;

    vkQueueSubmit(queue, 1, &submitInfo, fd->fence);
}

void App::framePresent() {
    if (swapchainRebuild) return;
    
    FrameData* fd = &frames[frameIndex];
    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &fd->renderCompleteSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &semaphoreIndex;
    
    VkResult err = vkQueuePresentKHR(queue, &presentInfo);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        swapchainRebuild = true;
    }
    
    frameIndex = (frameIndex + 1) % minImageCount;
}
