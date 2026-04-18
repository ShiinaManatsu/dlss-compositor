#include "pipeline/sequence_processor.h"

#include "cli/config.h"
#include "core/camera_data_loader.h"
#include "core/channel_mapper.h"
#include "core/exr_reader.h"
#include "core/exr_writer.h"
#include "core/mv_converter.h"
#include "dlss/dlss_fg_processor.h"
#include "dlss/dlss_sr_processor.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/lut_processor.h"
#include "gpu/pq_transfer_processor.h"
#include "gpu/texture_pool.h"
#include "gpu/texture_pipeline.h"
#include "pipeline/async_exr_writer.h"
#include "pipeline/frame_prefetcher.h"
#include "gpu/vulkan_context.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/// Return the directory containing the running executable.
std::filesystem::path getExeDir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
    // Fallback: current working directory
    return fs::current_path();
}

struct DlssFeatureGuard {
    explicit DlssFeatureGuard(NgxContext& ngxIn) : ngx(ngxIn) {}

    ~DlssFeatureGuard() {
        ngx.releaseDlssSR();
    }

    NgxContext& ngx;
};

struct DlssFgFeatureGuard {
    explicit DlssFgFeatureGuard(NgxContext& ngxIn) : ngx(ngxIn) {}

    ~DlssFgFeatureGuard() {
        ngx.releaseDlssFG();
    }

    NgxContext& ngx;
};

bool hasExrExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return extension == ".exr";
}

bool extractTrailingFrameNumber(const std::filesystem::path& path, int& frameNumber) {
    static const std::regex trailingDigitsRegex(R"(([0-9]+)$)");

    const std::string stem = path.stem().string();
    std::smatch match;
    if (!std::regex_search(stem, match, trailingDigitsRegex)) {
        return false;
    }

    try {
        frameNumber = std::stoi(match.str(1));
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<float> expandRgbToRgba(const std::vector<float>& rgb, float alpha) {
    std::vector<float> rgba;
    rgba.resize((rgb.size() / 3u) * 4u, alpha);
    for (size_t i = 0; i + 2 < rgb.size(); i += 3u) {
        const size_t dstIndex = (i / 3u) * 4u;
        rgba[dstIndex + 0u] = rgb[i + 0u];
        rgba[dstIndex + 1u] = rgb[i + 1u];
        rgba[dstIndex + 2u] = rgb[i + 2u];
        rgba[dstIndex + 3u] = alpha;
    }
    return rgba;
}

void forceOpaqueAlpha(std::vector<float>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4u) {
        rgba[i] = 1.0f;
    }
}

void splitRgba(const std::vector<float>& rgba,
               std::vector<float>& r,
               std::vector<float>& g,
               std::vector<float>& b,
               std::vector<float>& a) {
    const size_t pixelCount = rgba.size() / 4u;
    r.resize(pixelCount);
    g.resize(pixelCount);
    b.resize(pixelCount);
    a.resize(pixelCount);

    for (size_t i = 0; i < pixelCount; ++i) {
        const size_t srcIndex = i * 4u;
        r[i] = rgba[srcIndex + 0u];
        g[i] = rgba[srcIndex + 1u];
        b[i] = rgba[srcIndex + 2u];
        a[i] = rgba[srcIndex + 3u];
    }
}

void destroyHandle(TexturePipeline& pipeline, TextureHandle& handle) {
    if (handle.image != VK_NULL_HANDLE || handle.view != VK_NULL_HANDLE || handle.allocation != nullptr) {
        pipeline.destroy(handle);
    }
}

void transitionImage(VkCommandBuffer cmdBuf,
                     VkImage image,
                     VkImageLayout oldLayout,
                     VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    } else {
        throw std::runtime_error("Unsupported image layout transition in SequenceProcessor.");
    }

    vkCmdPipelineBarrier(cmdBuf,
                         srcStage,
                         dstStage,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &barrier);
}

bool allocateCommandBuffer(VulkanContext& ctx, VkCommandBuffer& cmdBuf, std::string& errorMsg) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = ctx.commandPool();
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    const VkResult result = vkAllocateCommandBuffers(ctx.device(), &allocInfo, &cmdBuf);
    if (result != VK_SUCCESS) {
        errorMsg = "Failed to allocate command buffer.";
        return false;
    }
    return true;
}

bool beginCommandBuffer(VkCommandBuffer cmdBuf, std::string& errorMsg) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmdBuf, &beginInfo) != VK_SUCCESS) {
        errorMsg = "Failed to begin command buffer.";
        return false;
    }
    return true;
}

bool submitAndWait(VulkanContext& ctx, VkCommandBuffer cmdBuf, std::string& errorMsg) {
    if (vkEndCommandBuffer(cmdBuf) != VK_SUCCESS) {
        errorMsg = "Failed to end command buffer.";
        vkFreeCommandBuffers(ctx.device(), ctx.commandPool(), 1, &cmdBuf);
        return false;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(ctx.device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        errorMsg = "Failed to create fence.";
        vkFreeCommandBuffers(ctx.device(), ctx.commandPool(), 1, &cmdBuf);
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuf;

    const VkResult submitResult = vkQueueSubmit(ctx.computeQueue(), 1, &submitInfo, fence);
    const VkResult waitResult = submitResult == VK_SUCCESS
                                    ? vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX)
                                    : VK_ERROR_UNKNOWN;
    const VkResult idleResult = waitResult == VK_SUCCESS ? vkQueueWaitIdle(ctx.computeQueue()) : VK_ERROR_UNKNOWN;

    vkDestroyFence(ctx.device(), fence, nullptr);
    vkFreeCommandBuffers(ctx.device(), ctx.commandPool(), 1, &cmdBuf);

    if (submitResult != VK_SUCCESS) {
        errorMsg = "Failed to submit command buffer.";
        return false;
    }
    if (waitResult != VK_SUCCESS) {
        errorMsg = "Failed waiting for command buffer completion.";
        return false;
    }
    if (idleResult != VK_SUCCESS) {
        errorMsg = "Failed waiting for compute queue idle.";
        return false;
    }

    return true;
}

std::string quote(const std::filesystem::path& path) {
    return std::string("\"") + path.string() + "\"";
}

bool writeRgbaExr(const std::filesystem::path& outputDir,
                  int outputFrameCounter,
                  int width,
                  int height,
                  const std::vector<float>& rgba,
                  const AppConfig& config,
                  std::string& errorMsg) {
    std::vector<float> opaqueRgba = rgba;
    forceOpaqueAlpha(opaqueRgba);
    std::vector<float> r;
    std::vector<float> g;
    std::vector<float> b;
    std::vector<float> a;
    splitRgba(opaqueRgba, r, g, b, a);

    ExrWriter writer;
    std::ostringstream oss;
    oss << "frame_" << std::setfill('0') << std::setw(4) << outputFrameCounter << ".exr";
    const std::filesystem::path outputPath = outputDir / oss.str();
    if (!writer.create(outputPath.string(), width, height, errorMsg)) {
        return false;
    }

    writer.setCompression(config.exrCompression, config.exrDwaQuality);
    if (!writer.addChannel("R", r.data()) ||
        !writer.addChannel("G", g.data()) ||
        !writer.addChannel("B", b.data()) ||
        !writer.addChannel("A", a.data()) ||
        !writer.write(errorMsg)) {
        if (errorMsg.empty()) {
            errorMsg = "Failed to write output EXR.";
        }
        return false;
    }

    return true;
}

/// Build an AsyncExrWriter::WriteJob ready for submission.
/// Moves the rgba data out of the source vector (caller must not reuse it).
AsyncExrWriter::WriteJob buildWriteJob(const std::filesystem::path& outputDir,
                                       int outputFrameCounter,
                                       int width,
                                       int height,
                                       std::vector<float>& rgba,
                                       const AppConfig& config) {
    std::ostringstream oss;
    oss << "frame_" << std::setfill('0') << std::setw(4) << outputFrameCounter << ".exr";
    AsyncExrWriter::WriteJob job;
    job.path = (outputDir / oss.str()).string();
    job.width = width;
    job.height = height;
    job.rgba = std::move(rgba);
    job.compression = config.exrCompression;
    job.dwaQuality = config.exrDwaQuality;
    return job;
}

/// Overload that copies from a const vector (for buffers that must remain valid).
AsyncExrWriter::WriteJob buildWriteJobCopy(const std::filesystem::path& outputDir,
                                           int outputFrameCounter,
                                           int width,
                                           int height,
                                           const std::vector<float>& rgba,
                                           const AppConfig& config) {
    std::ostringstream oss;
    oss << "frame_" << std::setfill('0') << std::setw(4) << outputFrameCounter << ".exr";
    AsyncExrWriter::WriteJob job;
    job.path = (outputDir / oss.str()).string();
    job.width = width;
    job.height = height;
    job.rgba = rgba; // copy
    job.compression = config.exrCompression;
    job.dwaQuality = config.exrDwaQuality;
    return job;
}

/// Resolve the path for a default LUT file relative to the executable directory.
/// Searches: <exe_dir>/luts/<name>, ./luts/<name>, ../luts/<name>.
std::string resolveDefaultLutPath(const std::string& filename) {
    namespace fs = std::filesystem;

    // Try relative to executable directory first (most reliable)
    fs::path exePath = getExeDir() / "luts" / filename;
    if (fs::exists(exePath)) {
        return exePath.string();
    }

    // Try relative to CWD
    fs::path cwdPath = fs::path("luts") / filename;
    if (fs::exists(cwdPath)) {
        return cwdPath.string();
    }

    // Try parent of CWD (common in build trees)
    fs::path parentPath = fs::path("../luts") / filename;
    if (fs::exists(parentPath)) {
        return parentPath.string();
    }

    // Return the exe-relative path as fallback (will fail with a clear error)
    return exePath.string();
}

/// Resolve path for a shader SPV file.
/// Searches: <exe_dir>/shaders/<name>, ./shaders/<name>, ../shaders/<name>.
std::string resolveShaderPath(const std::string& filename) {
    namespace fs = std::filesystem;

    // Try relative to executable directory first
    fs::path exePath = getExeDir() / "shaders" / filename;
    if (fs::exists(exePath)) {
        return exePath.string();
    }

    fs::path cwdPath = fs::path("shaders") / filename;
    if (fs::exists(cwdPath)) {
        return cwdPath.string();
    }

    fs::path parentPath = fs::path("../shaders") / filename;
    if (fs::exists(parentPath)) {
        return parentPath.string();
    }

    return exePath.string();
}

/// Initialize forward and inverse LUT processors based on AppConfig.
/// Returns true if tonemapping is active (at least forward LUT loaded).
/// Used only for Custom LUT mode.
bool initLutProcessors(VulkanContext& ctx,
                       const AppConfig& config,
                       LutProcessor& forwardLut,
                       LutProcessor& inverseLut,
                       bool& forwardReady,
                       bool& inverseReady,
                       std::string& errorMsg) {
    forwardReady = false;
    inverseReady = false;

    if (config.tonemapMode != TonemapMode::Custom) {
        return true;
    }

    // Only apply tonemapping when FG is active
    if (config.interpolateFactor <= 0) {
        return true;
    }

    const std::string& forwardPath = config.forwardLutFile;
    const std::string& inversePath = config.inverseLutFile;
    constexpr int kLutSize = 128;

    // Load forward LUT
    if (!forwardPath.empty()) {
        if (!forwardLut.loadLut(forwardPath, kLutSize, errorMsg)) {
            return false;
        }
    }

    // Load inverse LUT (only if inverse is enabled)
    if (config.inverseTonemapEnabled && !inversePath.empty()) {
        if (!inverseLut.loadLut(inversePath, kLutSize, errorMsg)) {
            return false;
        }
    }

    // Create compute pipeline (shared SPIR-V for both)
    const std::string spvPath = resolveShaderPath("apply_lut.comp.spv");

    if (!forwardPath.empty()) {
        if (!forwardLut.createPipeline(spvPath, errorMsg)) {
            return false;
        }
        forwardReady = true;
        std::fprintf(stdout, "Forward tonemapping enabled (%s)\n", forwardPath.c_str());
        std::fflush(stdout);
    }

    if (config.inverseTonemapEnabled && !inversePath.empty()) {
        if (!inverseLut.createPipeline(spvPath, errorMsg)) {
            return false;
        }
        inverseReady = true;
        std::fprintf(stdout, "Inverse tonemapping enabled (%s)\n", inversePath.c_str());
        std::fflush(stdout);
    }

    if (!config.inverseTonemapEnabled) {
        std::fprintf(stdout, "Inverse tonemapping disabled; FG output will be display-referred\n");
        std::fflush(stdout);
    }

    return true;
}

/// Initialize PQ transfer processor for FG transport encoding.
/// Returns true on success. pqReady is set to true if PQ transport is active.
bool initPqTransfer(VulkanContext& ctx,
                    const AppConfig& config,
                    PqTransferProcessor& pqTransfer,
                    bool& pqReady,
                    std::string& errorMsg) {
    pqReady = false;

    if (config.tonemapMode != TonemapMode::PQ) {
        return true;
    }

    // Only apply PQ transport when FG is active
    if (config.interpolateFactor <= 0) {
        return true;
    }

    const std::string spvPath = resolveShaderPath("pq_transfer.comp.spv");
    if (!pqTransfer.createPipeline(spvPath, errorMsg)) {
        return false;
    }

    pqReady = true;
        std::fprintf(stdout, "PQ transport encoding enabled (ST 2084, 10000 nits)\n");
        std::fflush(stdout);

    if (!config.inverseTonemapEnabled) {
        std::fprintf(stdout, "Inverse PQ decode disabled; FG output will be PQ-encoded\n");
        std::fflush(stdout);
    }

    return true;
}

bool pathsAreEquivalent(const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
    std::error_code ec;
    const std::filesystem::path lhsCanonical = std::filesystem::weakly_canonical(lhs, ec);
    if (ec) {
        ec.clear();
        const std::filesystem::path lhsAbsolute = std::filesystem::absolute(lhs, ec);
        if (ec) {
            return false;
        }
        ec.clear();
        const std::filesystem::path rhsAbsolute = std::filesystem::absolute(rhs, ec);
        if (ec) {
            return false;
        }
        return lhsAbsolute == rhsAbsolute;
    }

    ec.clear();
    const std::filesystem::path rhsCanonical = std::filesystem::weakly_canonical(rhs, ec);
    if (ec) {
        ec.clear();
        const std::filesystem::path rhsAbsolute = std::filesystem::absolute(rhs, ec);
        if (ec) {
            return false;
        }
        return lhsCanonical == rhsAbsolute;
    }

    return lhsCanonical == rhsCanonical;
}

}

/// Convert Blender world-space depth (meters) to DLSS NDC depth [0, 1].
/// Uses linear normalization: z_ndc = (z_world - near) / (far - near)
/// If cameraData is nullptr or invalid, returns a copy of the original depth unchanged.
std::vector<float> convertWorldDepthToNdc(const std::vector<float>& worldDepth, const CameraFrameData* cameraData) {
    std::vector<float> result = worldDepth;
    if (!cameraData) return result;
    float n = cameraData->near_clip;
    float f = cameraData->far_clip;
    if (f <= n || f - n < 1e-6f) return result;
    float range = f - n;
    for (size_t i = 0; i < result.size(); ++i) {
        float z = worldDepth[i];
        if (z <= 0.0f) {
            result[i] = 1.0f;
        } else {
            float z_ndc = (z - n) / range;
            result[i] = std::clamp(z_ndc, 0.0f, 1.0f);
        }
    }
    return result;
}

SequenceProcessor::SequenceProcessor(VulkanContext& ctx, NgxContext& ngx, TexturePipeline& texturePipeline)
    : m_ctx(ctx), m_ngx(ngx), m_texturePipeline(texturePipeline) {}

bool SequenceProcessor::scanAndSort(const std::filesystem::path& inputDir,
                                    std::vector<SequenceFrameInfo>& frames,
                                    std::string& errorMsg) {
    frames.clear();
    errorMsg.clear();

    std::error_code ec;
    if (!std::filesystem::exists(inputDir, ec) || !std::filesystem::is_directory(inputDir, ec)) {
        errorMsg = "Input directory does not exist: " + inputDir.string();
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(inputDir, ec)) {
        if (ec) {
            errorMsg = "Failed to scan input directory: " + inputDir.string();
            return false;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (!hasExrExtension(entry.path())) {
            continue;
        }

        SequenceFrameInfo frameInfo;
        frameInfo.path = entry.path();
        frameInfo.frameNumber = std::numeric_limits<int>::max();
        extractTrailingFrameNumber(entry.path(), frameInfo.frameNumber);
        frames.push_back(std::move(frameInfo));
    }

    std::sort(frames.begin(), frames.end(), [](const SequenceFrameInfo& lhs, const SequenceFrameInfo& rhs) {
        if (lhs.frameNumber != rhs.frameNumber) {
            return lhs.frameNumber < rhs.frameNumber;
        }
        return lhs.path.filename().string() < rhs.path.filename().string();
    });

    if (frames.empty()) {
        errorMsg = "No EXR files found in input directory: " + inputDir.string();
        return false;
    }

    return true;
}

std::vector<bool> SequenceProcessor::computeResetFlags(const std::vector<SequenceFrameInfo>& frames) {
    std::vector<bool> resets(frames.size(), false);
    if (frames.empty()) {
        return resets;
    }

    resets[0] = true;
    for (size_t i = 1; i < frames.size(); ++i) {
        const int prevFrame = frames[i - 1].frameNumber;
        const int currentFrame = frames[i].frameNumber;
        resets[i] = prevFrame != std::numeric_limits<int>::max() && currentFrame != std::numeric_limits<int>::max() &&
                    (currentFrame - prevFrame) > 1;
    }
    return resets;
}

bool SequenceProcessor::processDirectory(const std::string& inputDir,
                                         const std::string& outputDir,
                                         const AppConfig& config,
                                         std::string& errorMsg) {
    errorMsg.clear();

    if (config.interpolateFactor > 0 && config.scaleFactor >= 1.0f) {
        return processDirectorySRFG(inputDir, outputDir, config, errorMsg);
    }
    if (config.interpolateFactor > 0) {
        return processDirectoryFG(inputDir, outputDir, config, errorMsg);
    }

    const bool requestedUnsupportedPasses = hasPass(config.outputPasses, OutputPass::Depth) ||
                                            hasPass(config.outputPasses, OutputPass::Normals);
    if (requestedUnsupportedPasses) {
        std::fprintf(stderr,
                     "Warning: only 'beauty' pass output is currently supported. Other passes will be ignored.\n");
    }

    if (config.scaleExplicit && config.scaleFactor < 1.0f) {
        errorMsg = "Scale factor must be at least 1.0.";
        return false;
    }

    std::vector<SequenceFrameInfo> frames;
    if (!scanAndSort(inputDir, frames, errorMsg)) {
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        errorMsg = "Failed to create output directory: " + outputDir;
        return false;
    }

    const std::vector<bool> resetFlags = computeResetFlags(frames);
    ChannelMapper channelMapper;
    DlssSRProcessor processor(m_ctx, m_ngx);
    DlssFeatureGuard featureGuard(m_ngx);

    // Optionally load camera data for jitter values
    CameraDataLoader cameraLoader;
    bool hasCameraData = false;
    if (!config.cameraDataFile.empty()) {
        std::fprintf(stdout, "Loading camera data: %s\n", config.cameraDataFile.c_str());
        std::fflush(stdout);
        std::string cameraError;
        if (cameraLoader.load(config.cameraDataFile, cameraError)) {
            hasCameraData = true;
            std::fprintf(stdout, "Camera data loaded (%d frames)\n", cameraLoader.frameCount());
            std::fflush(stdout);
        } else {
            std::fprintf(stderr, "Warning: Camera data load failed: %s\n", cameraError.c_str());
            std::fprintf(stderr, "Continuing without jitter values (defaulting to 0.0)\n");
        }
    }

    // Peek at the first frame to determine input resolution for feature creation and prefetcher.
    ExrReader peekReader;
    std::string peekError;
    if (!peekReader.open(frames.front().path.string(), peekError)) {
        errorMsg = "Failed to open first frame: " + peekError;
        return false;
    }

    const int expectedInputWidth = peekReader.width();
    const int expectedInputHeight = peekReader.height();
    auto roundEven = [](double value) -> int {
        const int rounded = static_cast<int>(std::round(value));
        return (rounded % 2 != 0) ? rounded + 1 : rounded;
    };
    const int expectedOutputWidth =
        roundEven(static_cast<double>(expectedInputWidth) * static_cast<double>(config.scaleFactor));
    const int expectedOutputHeight =
        roundEven(static_cast<double>(expectedInputHeight) * static_cast<double>(config.scaleFactor));

    VkCommandBuffer createCmdBuf = VK_NULL_HANDLE;
    if (!allocateCommandBuffer(m_ctx, createCmdBuf, errorMsg) ||
        !beginCommandBuffer(createCmdBuf, errorMsg) ||
        !m_ngx.createDlssSR(expectedInputWidth,
                            expectedInputHeight,
                            expectedOutputWidth,
                            expectedOutputHeight,
                            config.quality,
                            config.preset,
                            createCmdBuf,
                            errorMsg) ||
        !submitAndWait(m_ctx, createCmdBuf, errorMsg)) {
        if (createCmdBuf != VK_NULL_HANDLE && !errorMsg.empty()) {
            vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &createCmdBuf);
        }
        return false;
    }

    const int64_t maxMemory = static_cast<int64_t>(config.memoryBudgetGB) * 1024LL * 1024LL * 1024LL;
    std::unique_ptr<TexturePool> texturePool = std::make_unique<TexturePool>(m_ctx, m_texturePipeline, maxMemory);
    const std::vector<float> outputInit(static_cast<size_t>(expectedOutputWidth) *
                                            static_cast<size_t>(expectedOutputHeight) *
                                            4u,
                                        0.0f);

    MvConverter mvConverter;
    FramePrefetcher prefetcher(channelMapper, mvConverter, 3, expectedInputWidth, expectedInputHeight);
    prefetcher.start(frames);

    bool anyFrameSucceeded = false;
    bool pooledHandlesAcquired = false;
    TextureHandle color;
    TextureHandle depth;
    TextureHandle motion;
    TextureHandle diffuse;
    TextureHandle normals;
    TextureHandle roughness;
    TextureHandle output;

    for (size_t index = 0; index < frames.size(); ++index) {
        const SequenceFrameInfo& frameInfo = frames[index];
        const std::string filename = frameInfo.path.filename().string();
        std::fprintf(stdout, "Processing frame %d/%d: %s\n",
                     static_cast<int>(index + 1),
                     static_cast<int>(frames.size()),
                     filename.c_str());
        std::fflush(stdout);

        if (index > 0 && resetFlags[index]) {
            std::fprintf(stderr,
                         "Warning: frame gap detected between %d and %d; resetting temporal history\n",
                         frames[index - 1].frameNumber,
                         frameInfo.frameNumber);
        }

        std::string frameError;

        try {
            auto prefetched = prefetcher.getNext();
            if (!prefetched || !prefetched->valid) {
                std::fprintf(stderr, "Frame %s failed: prefetch error\n", filename.c_str());
                continue;
            }

            auto& mappedBuffers = prefetched->mappedBuffers;
            auto& mvResult = prefetched->mvResult;

            const std::vector<float> diffuseRgba = expandRgbToRgba(mappedBuffers.diffuseAlbedo, 1.0f);
            const std::vector<float> normalsRgba = expandRgbToRgba(mappedBuffers.normals, 1.0f);

            try {
                if (!pooledHandlesAcquired) {
                    color = texturePool->acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
                    depth = texturePool->acquire(expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT);
                    motion = texturePool->acquire(expectedInputWidth, expectedInputHeight, 2, VK_FORMAT_R16G16_SFLOAT);
                    diffuse = texturePool->acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
                    normals = texturePool->acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
                    roughness = texturePool->acquire(expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT);
                    output = texturePool->acquire(expectedOutputWidth, expectedOutputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
                    pooledHandlesAcquired = true;
                }

                texturePool->updateData(color, mappedBuffers.color.data());
                
                // Convert Blender world-space depth to DLSS NDC depth [0, 1]
                const CameraFrameData* camFrame = nullptr;
                if (hasCameraData && cameraLoader.hasFrame(frameInfo.frameNumber)) {
                    camFrame = &cameraLoader.getFrame(frameInfo.frameNumber);
                }
                const std::vector<float> convertedDepth = convertWorldDepthToNdc(mappedBuffers.depth, camFrame);
                texturePool->updateData(depth, convertedDepth.data());
                
                texturePool->updateData(motion, mvResult.mvXY.data());
                texturePool->updateData(diffuse, diffuseRgba.data());
                texturePool->updateData(normals, normalsRgba.data());
                texturePool->updateData(roughness, mappedBuffers.roughness.data());
                texturePool->updateData(output, outputInit.data());

                DlssSRFrameInput frame{};
                frame.color = color.image;
                frame.colorView = color.view;
                frame.depth = depth.image;
                frame.depthView = depth.view;
                frame.motionVectors = motion.image;
                frame.motionView = motion.view;
                frame.diffuseAlbedo = diffuse.image;
                frame.diffuseView = diffuse.view;
                frame.normals = normals.image;
                frame.normalsView = normals.view;
                frame.roughness = roughness.image;
                frame.roughnessView = roughness.view;
                frame.output = output.image;
                frame.outputView = output.view;
                frame.inputWidth = static_cast<uint32_t>(expectedInputWidth);
                frame.inputHeight = static_cast<uint32_t>(expectedInputHeight);
                frame.outputWidth = static_cast<uint32_t>(expectedOutputWidth);
                frame.outputHeight = static_cast<uint32_t>(expectedOutputHeight);
                // DLSS expects MV in output-pixel units; Blender MV is in render-pixel units.
                // Scale by output/input ratio so DLSS sees correct displacement magnitude.
                frame.mvScaleX = mvResult.scaleX * static_cast<float>(expectedOutputWidth) / static_cast<float>(expectedInputWidth);
                frame.mvScaleY = mvResult.scaleY * static_cast<float>(expectedOutputHeight) / static_cast<float>(expectedInputHeight);
                frame.reset = resetFlags[index] || !anyFrameSucceeded;

                // Assign jitter values from camera data if available
                if (hasCameraData && cameraLoader.hasFrame(frameInfo.frameNumber)) {
                    const auto& camData = cameraLoader.getFrame(frameInfo.frameNumber);
                    frame.jitterX = camData.jitter_x;
                    frame.jitterY = camData.jitter_y;
                    
                    // Log non-zero jitter values
                    if (std::abs(frame.jitterX) > 1e-6f || std::abs(frame.jitterY) > 1e-6f) {
                        std::fprintf(stdout, "Frame %d: Jitter offset = (%.4f, %.4f) pixels\n",
                                     frameInfo.frameNumber, frame.jitterX, frame.jitterY);
                    }
                } else {
                    frame.jitterX = 0.0f;
                    frame.jitterY = 0.0f;
                }

                VkCommandBuffer evalCmdBuf = VK_NULL_HANDLE;
                if (!allocateCommandBuffer(m_ctx, evalCmdBuf, frameError) ||
                    !beginCommandBuffer(evalCmdBuf, frameError)) {
                    throw std::runtime_error(frameError);
                }

                try {
                    transitionImage(evalCmdBuf,
                                    output.image,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_GENERAL);
                    if (!processor.evaluate(evalCmdBuf, frame, frameError)) {
                        throw std::runtime_error(frameError);
                    }
                    transitionImage(evalCmdBuf,
                                    output.image,
                                    VK_IMAGE_LAYOUT_GENERAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                } catch (...) {
                    vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &evalCmdBuf);
                    throw;
                }

                if (!submitAndWait(m_ctx, evalCmdBuf, frameError)) {
                    throw std::runtime_error(frameError);
                }

                std::vector<float> outputData = m_texturePipeline.download(output);
                forceOpaqueAlpha(outputData);
                std::vector<float> r;
                std::vector<float> g;
                std::vector<float> b;
                std::vector<float> a;
                splitRgba(outputData, r, g, b, a);

                ExrWriter writer;
                const std::filesystem::path outputPath = std::filesystem::path(outputDir) / frameInfo.path.filename();
                if (!writer.create(outputPath.string(), expectedOutputWidth, expectedOutputHeight, frameError)) {
                    throw std::runtime_error(frameError);
                }
                writer.setCompression(config.exrCompression, config.exrDwaQuality);
                // TODO: Support writing additional output passes when input-resolution passthrough
                // channels can be reconciled with the upscaled beauty output resolution.
                if (!writer.addChannel("R", r.data()) ||
                    !writer.addChannel("G", g.data()) ||
                    !writer.addChannel("B", b.data()) ||
                    !writer.addChannel("A", a.data()) ||
                    !writer.write(frameError)) {
                    if (frameError.empty()) {
                        frameError = "Failed to write output EXR.";
                    }
                    throw std::runtime_error(frameError);
                }

                anyFrameSucceeded = true;
            } catch (const std::exception& ex) {
                frameError = ex.what();
                std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), frameError.c_str());
            }
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), ex.what());
        }
    }

    if (texturePool) {
        texturePool->releaseAll();
    }

    if (!anyFrameSucceeded) {
        errorMsg = "No frames were processed successfully.";
        return false;
    }

    if (config.encodeVideo) {
        std::filesystem::path videoOutputPath(config.videoOutputFile);
        if (videoOutputPath.is_relative()) {
            videoOutputPath = std::filesystem::path(outputDir) / videoOutputPath;
        }

        const std::filesystem::path inputPattern = std::filesystem::path(outputDir) / "frame_%04d.exr";
        std::ostringstream command;
        command << "ffmpeg -y -framerate " << config.fps << " -i " << quote(inputPattern)
                << " -c:v libx264 -pix_fmt yuv420p -crf 18 " << quote(videoOutputPath);
        const int ffmpegResult = std::system(command.str().c_str());
        if (ffmpegResult != 0) {
            std::fprintf(stderr, "Warning: ffmpeg encoding failed with exit code %d\n", ffmpegResult);
        }
    }

    return true;
}

bool SequenceProcessor::processDirectorySRFG(const std::string& inputDir,
                                             const std::string& outputDir,
                                             const AppConfig& config,
                                             std::string& errorMsg) {
    errorMsg.clear();
    std::fprintf(stdout, "[SRFG] Starting combined SR+FG pipeline\n");
    std::fflush(stdout);
    std::fprintf(stdout, "[SRFG]   input:  %s\n", inputDir.c_str());
    std::fflush(stdout);
    std::fprintf(stdout, "[SRFG]   output: %s\n", outputDir.c_str());
    std::fflush(stdout);
    std::fprintf(stdout, "[SRFG]   scale=%.2f  interpolate=%dx\n", config.scaleFactor, config.interpolateFactor);
    std::fflush(stdout);

    const bool requestedUnsupportedPasses = hasPass(config.outputPasses, OutputPass::Depth) ||
                                            hasPass(config.outputPasses, OutputPass::Normals);
    if (requestedUnsupportedPasses) {
        std::fprintf(stderr,
                     "Warning: only 'beauty' pass output is currently supported. Other passes will be ignored.\n");
    }

    std::vector<SequenceFrameInfo> frames;
    if (!scanAndSort(inputDir, frames, errorMsg)) {
        std::fprintf(stderr, "[SRFG] scanAndSort failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] Found %d EXR frames\n", static_cast<int>(frames.size()));
    std::fflush(stdout);

    if (frames.size() < 2) {
        errorMsg = "At least 2 frames required for interpolation";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        errorMsg = "Failed to create output directory: " + outputDir;
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }

    if (pathsAreEquivalent(inputDir, outputDir)) {
        errorMsg = "Input and output directories must be different for interpolation.";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }

    CameraDataLoader cameraLoader;
    std::fprintf(stdout, "[SRFG] Loading camera data: %s\n", config.cameraDataFile.c_str());
    std::fflush(stdout);
    if (!cameraLoader.load(config.cameraDataFile, errorMsg)) {
        std::fprintf(stderr, "[SRFG] Camera data load failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] Camera data loaded (%d frames)\n", cameraLoader.frameCount());
    std::fflush(stdout);

    for (const SequenceFrameInfo& frameInfo : frames) {
        if (frameInfo.frameNumber == std::numeric_limits<int>::max()) {
            errorMsg = "Input filename is missing a trailing frame number: " + frameInfo.path.filename().string();
            std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
            return false;
        }
        if (!cameraLoader.hasFrame(frameInfo.frameNumber)) {
            errorMsg = "Camera data missing for frame " + std::to_string(frameInfo.frameNumber);
            std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
            return false;
        }
    }
    std::fprintf(stdout, "[SRFG] All frames have matching camera data\n");
    std::fflush(stdout);

    unsigned int multiFrameCount = 0;
    if (config.interpolateFactor == 2) {
        multiFrameCount = 1;
    } else if (config.interpolateFactor == 4) {
        multiFrameCount = 3;
    } else {
        errorMsg = "Unsupported interpolation factor: " + std::to_string(config.interpolateFactor) +
                   ". Only 2x and 4x are supported.";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }

    if (!m_ngx.isDlssFGAvailable()) {
        errorMsg = "DLSS Frame Generation is not supported on this GPU. RTX 40+ required.";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] DLSS-FG available\n");
    std::fflush(stdout);

    if (!m_ngx.isDlssSRAvailable()) {
        errorMsg = m_ngx.unavailableReason();
        if (errorMsg.empty()) {
            errorMsg = "DLSS Super Resolution is not supported on this GPU.";
        }
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] DLSS-SR available\n");
    std::fflush(stdout);

    if (config.interpolateFactor == 4 && m_ngx.maxMultiFrameCount() < 3) {
        errorMsg = "4x frame generation requires RTX 50 series or newer. This GPU supports up to " +
                   std::to_string(m_ngx.maxMultiFrameCount() + 1) + "x.";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }

    ExrReader firstReader;
    if (!firstReader.open(frames.front().path.string(), errorMsg)) {
        std::fprintf(stderr, "[SRFG] Failed to open first EXR: %s\n", errorMsg.c_str());
        return false;
    }

    const int expectedInputWidth = firstReader.width();
    const int expectedInputHeight = firstReader.height();
    auto roundEven = [](double value) -> int {
        const int rounded = static_cast<int>(std::round(value));
        return (rounded % 2 != 0) ? rounded + 1 : rounded;
    };
    const int expectedOutputWidth =
        roundEven(static_cast<double>(expectedInputWidth) * static_cast<double>(config.scaleFactor));
    const int expectedOutputHeight =
        roundEven(static_cast<double>(expectedInputHeight) * static_cast<double>(config.scaleFactor));
    std::fprintf(stdout, "[SRFG] Input resolution: %dx%d -> Output: %dx%d\n",
                 expectedInputWidth, expectedInputHeight, expectedOutputWidth, expectedOutputHeight);
    std::fflush(stdout);

    DlssFeatureGuard featureGuardSR(m_ngx);
    DlssFgFeatureGuard featureGuardFG(m_ngx);

    std::fprintf(stdout, "[SRFG] Creating DLSS-SR feature...\n");
    std::fflush(stdout);
    VkCommandBuffer createCmdBufSR = VK_NULL_HANDLE;
    if (!allocateCommandBuffer(m_ctx, createCmdBufSR, errorMsg)) {
        std::fprintf(stderr, "[SRFG] Failed to allocate SR command buffer: %s\n", errorMsg.c_str());
        return false;
    }
    if (!beginCommandBuffer(createCmdBufSR, errorMsg) ||
        !m_ngx.createDlssSR(expectedInputWidth,
                            expectedInputHeight,
                            expectedOutputWidth,
                            expectedOutputHeight,
                            config.quality,
                            config.preset,
                            createCmdBufSR,
                            errorMsg)) {
        std::fprintf(stderr, "[SRFG] DLSS-SR feature creation failed: %s\n", errorMsg.c_str());
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &createCmdBufSR);
        return false;
    }
    if (!submitAndWait(m_ctx, createCmdBufSR, errorMsg)) {
        std::fprintf(stderr, "[SRFG] DLSS-SR feature submit failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] DLSS-SR feature created\n");
    std::fflush(stdout);

    std::fprintf(stdout, "[SRFG] Creating DLSS-FG feature...\n");
    std::fflush(stdout);
    VkCommandBuffer createCmdBufFG = VK_NULL_HANDLE;
    if (!allocateCommandBuffer(m_ctx, createCmdBufFG, errorMsg)) {
        std::fprintf(stderr, "[SRFG] Failed to allocate FG command buffer: %s\n", errorMsg.c_str());
        return false;
    }
    if (!beginCommandBuffer(createCmdBufFG, errorMsg) ||
        !m_ngx.createDlssFG(static_cast<uint32_t>(expectedOutputWidth),
                            static_cast<uint32_t>(expectedOutputHeight),
                            static_cast<unsigned int>(VK_FORMAT_R16G16B16A16_SFLOAT),
                            createCmdBufFG,
                            errorMsg)) {
        std::fprintf(stderr, "[SRFG] DLSS-FG feature creation failed: %s\n", errorMsg.c_str());
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &createCmdBufFG);
        return false;
    }
    if (!submitAndWait(m_ctx, createCmdBufFG, errorMsg)) {
        std::fprintf(stderr, "[SRFG] DLSS-FG feature submit failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] DLSS-FG feature created\n");
    std::fflush(stdout);

    ChannelMapper channelMapper;
    MvConverter mvConverter;
    DlssSRProcessor srProcessor(m_ctx, m_ngx);
    DlssFgProcessor fgProcessor(m_ctx, m_ngx);
    const std::filesystem::path outputPath(outputDir);
    const int64_t maxMemory = static_cast<int64_t>(config.memoryBudgetGB) * 1024LL * 1024LL * 1024LL;
    TexturePool pool(m_ctx, m_texturePipeline, maxMemory);

    // Initialize FG transport encoding (PQ or Custom LUT)
    std::fprintf(stdout, "[SRFG] Initializing FG transport...\n");
    std::fflush(stdout);
    PqTransferProcessor pqTransfer(m_ctx);
    bool pqReady = false;
    LutProcessor forwardLut(m_ctx);
    LutProcessor inverseLut(m_ctx);
    bool forwardLutReady = false;
    bool inverseLutReady = false;
    if (!initPqTransfer(m_ctx, config, pqTransfer, pqReady, errorMsg)) {
        std::fprintf(stderr, "[SRFG] PQ transfer init failed: %s\n", errorMsg.c_str());
        return false;
    }
    if (!initLutProcessors(m_ctx, config, forwardLut, inverseLut,
                           forwardLutReady, inverseLutReady, errorMsg)) {
        std::fprintf(stderr, "[SRFG] LUT init failed: %s\n", errorMsg.c_str());
        return false;
    }
    const bool transportReady = pqReady || forwardLutReady;
    const bool inverseReady = pqReady ? config.inverseTonemapEnabled : inverseLutReady;
    std::fprintf(stdout, "[SRFG] Transport init done (pq=%s, lut_fwd=%s, inverse=%s)\n",
                 pqReady ? "yes" : "no", forwardLutReady ? "yes" : "no",
                 inverseReady ? "yes" : "no");
    std::fflush(stdout);

    FramePrefetcher prefetcher(channelMapper, mvConverter, 3, expectedInputWidth, expectedInputHeight);
    prefetcher.start(frames);

    std::fprintf(stdout, "[SRFG] Prefetching first frame...\n");
    std::fflush(stdout);
    auto firstPrefetched = prefetcher.getNext();
    if (!firstPrefetched || !firstPrefetched->valid) {
        errorMsg = "Failed to prefetch first frame";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[SRFG] First frame prefetched OK\n");
    std::fflush(stdout);

    auto& firstMappedBuffers = firstPrefetched->mappedBuffers;
    const auto& firstMvResult = firstPrefetched->mvResult;
    const std::vector<float> firstDiffuseRgba = expandRgbToRgba(firstMappedBuffers.diffuseAlbedo, 1.0f);
    const std::vector<float> firstNormalsRgba = expandRgbToRgba(firstMappedBuffers.normals, 1.0f);
    const std::vector<float> srOutputInit(static_cast<size_t>(expectedOutputWidth) *
                                              static_cast<size_t>(expectedOutputHeight) *
                                              4u,
                                          0.0f);

    TextureHandle firstColor;
    TextureHandle firstDepth;
    TextureHandle firstMotion;
    TextureHandle firstDiffuse;
    TextureHandle firstNormals;
    TextureHandle firstRoughness;
    TextureHandle firstOutput;

    try {
        firstColor = pool.acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
        firstDepth = pool.acquire(expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT);
        firstMotion = pool.acquire(expectedInputWidth, expectedInputHeight, 2, VK_FORMAT_R16G16_SFLOAT);
        firstDiffuse = pool.acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
        firstNormals = pool.acquire(expectedInputWidth, expectedInputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
        firstRoughness = pool.acquire(expectedInputWidth, expectedInputHeight, 1, VK_FORMAT_R32_SFLOAT);
        firstOutput = pool.acquire(expectedOutputWidth, expectedOutputHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);

        pool.updateData(firstColor, firstMappedBuffers.color.data());
        pool.updateData(firstDepth, firstMappedBuffers.depth.data());
        pool.updateData(firstMotion, firstMvResult.mvXY.data());
        pool.updateData(firstDiffuse, firstDiffuseRgba.data());
        pool.updateData(firstNormals, firstNormalsRgba.data());
        pool.updateData(firstRoughness, firstMappedBuffers.roughness.data());
        pool.updateData(firstOutput, srOutputInit.data());
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }

    DlssSRFrameInput firstFrame{};
    firstFrame.color = firstColor.image;
    firstFrame.colorView = firstColor.view;
    firstFrame.depth = firstDepth.image;
    firstFrame.depthView = firstDepth.view;
    firstFrame.motionVectors = firstMotion.image;
    firstFrame.motionView = firstMotion.view;
    firstFrame.diffuseAlbedo = firstDiffuse.image;
    firstFrame.diffuseView = firstDiffuse.view;
    firstFrame.normals = firstNormals.image;
    firstFrame.normalsView = firstNormals.view;
    firstFrame.roughness = firstRoughness.image;
    firstFrame.roughnessView = firstRoughness.view;
    firstFrame.output = firstOutput.image;
    firstFrame.outputView = firstOutput.view;
    firstFrame.inputWidth = static_cast<uint32_t>(expectedInputWidth);
    firstFrame.inputHeight = static_cast<uint32_t>(expectedInputHeight);
    firstFrame.outputWidth = static_cast<uint32_t>(expectedOutputWidth);
    firstFrame.outputHeight = static_cast<uint32_t>(expectedOutputHeight);
    // DLSS expects MV in output-pixel units; Blender MV is in render-pixel units.
    firstFrame.mvScaleX = firstMvResult.scaleX * static_cast<float>(expectedOutputWidth) / static_cast<float>(expectedInputWidth);
    firstFrame.mvScaleY = firstMvResult.scaleY * static_cast<float>(expectedOutputHeight) / static_cast<float>(expectedInputHeight);
    firstFrame.reset = true;

    VkCommandBuffer firstEvalCmdBuf = VK_NULL_HANDLE;
    if (!allocateCommandBuffer(m_ctx, firstEvalCmdBuf, errorMsg)) {
        return false;
    }
    if (!beginCommandBuffer(firstEvalCmdBuf, errorMsg)) {
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &firstEvalCmdBuf);
        return false;
    }

    transitionImage(firstEvalCmdBuf,
                    firstOutput.image,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL);
    if (!srProcessor.evaluate(firstEvalCmdBuf, firstFrame, errorMsg)) {
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &firstEvalCmdBuf);
        return false;
    }
    transitionImage(firstEvalCmdBuf,
                    firstOutput.image,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!submitAndWait(m_ctx, firstEvalCmdBuf, errorMsg)) {
        return false;
    }

    int outputFrameCounter = 1;
    AsyncExrWriter asyncWriter;
    std::fprintf(stdout, "[SRFG] Async EXR writer started (%zu threads)\n",
                 static_cast<size_t>(std::thread::hardware_concurrency()));
    std::fflush(stdout);
    std::fprintf(stdout, "[SRFG] Downloading and submitting first SR output...\n");
    std::fflush(stdout);
    std::vector<float> firstOutputData = m_texturePipeline.download(firstOutput);
    forceOpaqueAlpha(firstOutputData);
    asyncWriter.submit(buildWriteJob(outputPath, outputFrameCounter,
                                     expectedOutputWidth, expectedOutputHeight,
                                     firstOutputData, config));
    ++outputFrameCounter;

    for (size_t i = 1; i < frames.size(); ++i) {
        const SequenceFrameInfo& previousFrameInfo = frames[i - 1];
        const SequenceFrameInfo& currentFrameInfo = frames[i];
        const std::string filename = currentFrameInfo.path.filename().string();
        std::fprintf(stdout,
                     "[SRFG] Processing frame pair %d/%d: %s -> %s\n",
                     static_cast<int>(i),
                     static_cast<int>(frames.size() - 1),
                     previousFrameInfo.path.filename().string().c_str(),
                     filename.c_str());
        std::fflush(stdout);

        DlssFgCameraParams camParams{};
        if (!cameraLoader.computePairParams(currentFrameInfo.frameNumber,
                                            previousFrameInfo.frameNumber,
                                            camParams,
                                            errorMsg)) {
            std::fprintf(stderr, "[SRFG] Camera pair params failed: %s\n", errorMsg.c_str());
            return false;
        }

        auto prefetched = prefetcher.getNext();
        if (!prefetched || !prefetched->valid) {
            errorMsg = "Failed to prefetch frame: " + currentFrameInfo.path.filename().string();
            std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
            return false;
        }

        const bool hasGap = previousFrameInfo.frameNumber != std::numeric_limits<int>::max() &&
                            currentFrameInfo.frameNumber != std::numeric_limits<int>::max() &&
                            (currentFrameInfo.frameNumber - previousFrameInfo.frameNumber) > 1;
        const bool resetFlag = (i == 1) || hasGap;
        if (hasGap) {
            std::fprintf(stderr,
                         "Warning: frame gap detected between %d and %d; resetting temporal history\n",
                         previousFrameInfo.frameNumber,
                         currentFrameInfo.frameNumber);
        }

        auto& mappedBuffers = prefetched->mappedBuffers;
        auto& mvResult = prefetched->mvResult;
        const std::vector<float> diffuseRgba = expandRgbToRgba(mappedBuffers.diffuseAlbedo, 1.0f);
        const std::vector<float> normalsRgba = expandRgbToRgba(mappedBuffers.normals, 1.0f);

        TextureHandle color;
        TextureHandle depth;
        TextureHandle motion;
        TextureHandle diffuse;
        TextureHandle normals;
        TextureHandle roughness;
        TextureHandle srOutput;

        color = firstColor;
        depth = firstDepth;
        motion = firstMotion;
        diffuse = firstDiffuse;
        normals = firstNormals;
        roughness = firstRoughness;
        srOutput = firstOutput;

        try {
            pool.updateData(color, mappedBuffers.color.data());
            
            // Convert Blender world-space depth to DLSS NDC depth [0, 1]
            const CameraFrameData& camFrame = cameraLoader.getFrame(currentFrameInfo.frameNumber);
            const std::vector<float> convertedDepth = convertWorldDepthToNdc(mappedBuffers.depth, &camFrame);
            pool.updateData(depth, convertedDepth.data());
            
            pool.updateData(motion, mvResult.mvXY.data());
            pool.updateData(diffuse, diffuseRgba.data());
            pool.updateData(normals, normalsRgba.data());
            pool.updateData(roughness, mappedBuffers.roughness.data());
            pool.updateData(srOutput, srOutputInit.data());
        } catch (const std::exception& ex) {
            errorMsg = ex.what();
            std::fprintf(stderr, "[SRFG] Upload failed: %s\n", errorMsg.c_str());
            return false;
        }

        DlssSRFrameInput srFrame{};
        srFrame.color = color.image;
        srFrame.colorView = color.view;
        srFrame.depth = depth.image;
        srFrame.depthView = depth.view;
        srFrame.motionVectors = motion.image;
        srFrame.motionView = motion.view;
        srFrame.diffuseAlbedo = diffuse.image;
        srFrame.diffuseView = diffuse.view;
        srFrame.normals = normals.image;
        srFrame.normalsView = normals.view;
        srFrame.roughness = roughness.image;
        srFrame.roughnessView = roughness.view;
        srFrame.output = srOutput.image;
        srFrame.outputView = srOutput.view;
        srFrame.inputWidth = static_cast<uint32_t>(expectedInputWidth);
        srFrame.inputHeight = static_cast<uint32_t>(expectedInputHeight);
        srFrame.outputWidth = static_cast<uint32_t>(expectedOutputWidth);
        srFrame.outputHeight = static_cast<uint32_t>(expectedOutputHeight);
        // DLSS expects MV in output-pixel units; Blender MV is in render-pixel units.
        srFrame.mvScaleX = mvResult.scaleX * static_cast<float>(expectedOutputWidth) / static_cast<float>(expectedInputWidth);
        srFrame.mvScaleY = mvResult.scaleY * static_cast<float>(expectedOutputHeight) / static_cast<float>(expectedInputHeight);
        srFrame.reset = resetFlag;

        VkCommandBuffer srEvalCmdBuf = VK_NULL_HANDLE;
        if (!allocateCommandBuffer(m_ctx, srEvalCmdBuf, errorMsg)) {
            return false;
        }
        if (!beginCommandBuffer(srEvalCmdBuf, errorMsg)) {
            vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &srEvalCmdBuf);
            return false;
        }

        transitionImage(srEvalCmdBuf,
                        srOutput.image,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_GENERAL);
        if (!srProcessor.evaluate(srEvalCmdBuf, srFrame, errorMsg)) {
            std::fprintf(stderr, "[SRFG] SR evaluate failed: %s\n", errorMsg.c_str());
            vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &srEvalCmdBuf);
            return false;
        }
        transitionImage(srEvalCmdBuf,
                        srOutput.image,
                        VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submitAndWait(m_ctx, srEvalCmdBuf, errorMsg)) {
            std::fprintf(stderr, "[SRFG] SR submit failed: %s\n", errorMsg.c_str());
            return false;
        }

        for (unsigned int multiFrameIndex = 1; multiFrameIndex <= multiFrameCount; ++multiFrameIndex) {
            TextureHandle outputInterp;
            outputInterp = m_texturePipeline.upload(srOutputInit.data(),
                                                    expectedOutputWidth,
                                                    expectedOutputHeight,
                                                    4,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);

            // Temporary texture for encoded backbuffer (PQ or forward LUT output)
            TextureHandle tonemappedBuf;
            if (transportReady) {
                tonemappedBuf = m_texturePipeline.upload(srOutputInit.data(),
                                                         expectedOutputWidth,
                                                         expectedOutputHeight,
                                                         4,
                                                         VK_FORMAT_R16G16B16A16_SFLOAT);
            }

            DlssFgFrameInput fgInput{};
            fgInput.backbuffer = transportReady ? tonemappedBuf.image : srOutput.image;
            fgInput.backbufferView = transportReady ? tonemappedBuf.view : srOutput.view;
            fgInput.depth = depth.image;
            fgInput.depthView = depth.view;
            fgInput.motionVectors = motion.image;
            fgInput.motionView = motion.view;
            fgInput.outputInterp = outputInterp.image;
            fgInput.outputInterpView = outputInterp.view;
            fgInput.width = static_cast<uint32_t>(expectedOutputWidth);
            fgInput.height = static_cast<uint32_t>(expectedOutputHeight);
            fgInput.reset = resetFlag && (multiFrameIndex == 1);
            fgInput.cameraParams = camParams;
            fgInput.multiFrameCount = multiFrameCount;
            fgInput.multiFrameIndex = multiFrameIndex;

            VkCommandBuffer fgEvalCmdBuf = VK_NULL_HANDLE;
            if (!allocateCommandBuffer(m_ctx, fgEvalCmdBuf, errorMsg)) {
                destroyHandle(m_texturePipeline, outputInterp);
                destroyHandle(m_texturePipeline, tonemappedBuf);
                return false;
            }
            if (!beginCommandBuffer(fgEvalCmdBuf, errorMsg)) {
                vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &fgEvalCmdBuf);
                destroyHandle(m_texturePipeline, outputInterp);
                destroyHandle(m_texturePipeline, tonemappedBuf);
                return false;
            }

            // Forward transport: srOutput (scene-linear) -> tonemappedBuf (PQ or display-referred)
            if (transportReady) {
                transitionImage(fgEvalCmdBuf,
                                srOutput.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_GENERAL);
                transitionImage(fgEvalCmdBuf,
                                tonemappedBuf.image,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_GENERAL);
                if (pqReady) {
                    pqTransfer.apply(fgEvalCmdBuf,
                                     srOutput.image,
                                     srOutput.view,
                                     tonemappedBuf.image,
                                     tonemappedBuf.view,
                                     expectedOutputWidth,
                                     expectedOutputHeight,
                                     true); // encode: scene-linear -> PQ
                } else {
                    forwardLut.apply(fgEvalCmdBuf,
                                     srOutput.image,
                                     srOutput.view,
                                     tonemappedBuf.image,
                                     tonemappedBuf.view,
                                     expectedOutputWidth,
                                     expectedOutputHeight);
                }
                // Transition srOutput back — NGX expects SHADER_READ_ONLY
                transitionImage(fgEvalCmdBuf,
                                srOutput.image,
                                VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                // tonemappedBuf stays GENERAL for NGX to read
            }

            transitionImage(fgEvalCmdBuf,
                            outputInterp.image,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_GENERAL);
            if (!fgProcessor.evaluate(fgEvalCmdBuf, fgInput, errorMsg)) {
                std::fprintf(stderr, "[SRFG] FG evaluate failed (multiframe %u): %s\n", multiFrameIndex, errorMsg.c_str());
                vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &fgEvalCmdBuf);
                destroyHandle(m_texturePipeline, outputInterp);
                destroyHandle(m_texturePipeline, tonemappedBuf);
                return false;
            }

            // Inverse transport: outputInterp (PQ or display-referred) -> tonemappedBuf (scene-linear)
            if (inverseReady && transportReady) {
                // tonemappedBuf is still GENERAL from forward pass
                // outputInterp is GENERAL from FG evaluate
                if (pqReady) {
                    pqTransfer.apply(fgEvalCmdBuf,
                                     outputInterp.image,
                                     outputInterp.view,
                                     tonemappedBuf.image,
                                     tonemappedBuf.view,
                                     expectedOutputWidth,
                                     expectedOutputHeight,
                                     false); // decode: PQ -> scene-linear
                } else {
                    inverseLut.apply(fgEvalCmdBuf,
                                     outputInterp.image,
                                     outputInterp.view,
                                     tonemappedBuf.image,
                                     tonemappedBuf.view,
                                     expectedOutputWidth,
                                     expectedOutputHeight,
                                     false); // display-referred input, no log2 shaper
                }
                transitionImage(fgEvalCmdBuf,
                                tonemappedBuf.image,
                                VK_IMAGE_LAYOUT_GENERAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }

            transitionImage(fgEvalCmdBuf,
                            outputInterp.image,
                            VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!submitAndWait(m_ctx, fgEvalCmdBuf, errorMsg)) {
                std::fprintf(stderr, "[SRFG] FG submit failed (multiframe %u): %s\n", multiFrameIndex, errorMsg.c_str());
                destroyHandle(m_texturePipeline, outputInterp);
                destroyHandle(m_texturePipeline, tonemappedBuf);
                return false;
            }

            // Download the result — from tonemappedBuf if inverse was applied, else outputInterp
            const TextureHandle& downloadSource = (inverseReady && transportReady) ? tonemappedBuf : outputInterp;
            std::vector<float> interpOutputData = m_texturePipeline.download(downloadSource);
            forceOpaqueAlpha(interpOutputData);

            asyncWriter.submit(buildWriteJob(outputPath, outputFrameCounter,
                                             expectedOutputWidth, expectedOutputHeight,
                                             interpOutputData, config));
            ++outputFrameCounter;
            destroyHandle(m_texturePipeline, outputInterp);
            destroyHandle(m_texturePipeline, tonemappedBuf);
        }

        std::vector<float> srOutputData = m_texturePipeline.download(srOutput);
        forceOpaqueAlpha(srOutputData);
        asyncWriter.submit(buildWriteJob(outputPath, outputFrameCounter,
                                         expectedOutputWidth, expectedOutputHeight,
                                         srOutputData, config));
        ++outputFrameCounter;
    }

    // Wait for all async writes to complete before releasing GPU resources
    std::fprintf(stdout, "[SRFG] Flushing async writer (%d errors so far)...\n", asyncWriter.errorCount());
    std::fflush(stdout);
    asyncWriter.flush();
    if (asyncWriter.errorCount() > 0) {
        errorMsg = "Async EXR writer encountered " + std::to_string(asyncWriter.errorCount()) + " write error(s)";
        std::fprintf(stderr, "[SRFG] %s\n", errorMsg.c_str());
        pool.releaseAll();
        return false;
    }
    pool.releaseAll();
    std::fprintf(stdout, "[SRFG] Done. %d output frames written.\n", outputFrameCounter - 1);
    std::fflush(stdout);

    if (config.encodeVideo) {
        std::filesystem::path videoOutputPath(config.videoOutputFile);
        if (videoOutputPath.is_relative()) {
            videoOutputPath = std::filesystem::path(outputDir) / videoOutputPath;
        }

        const std::filesystem::path inputPattern = std::filesystem::path(outputDir) / "frame_%04d.exr";
        std::ostringstream command;
        command << "ffmpeg -y -framerate " << config.fps << " -i " << quote(inputPattern)
                << " -c:v libx264 -pix_fmt yuv420p -crf 18 " << quote(videoOutputPath);
        const int ffmpegResult = std::system(command.str().c_str());
        if (ffmpegResult != 0) {
            std::fprintf(stderr, "Warning: ffmpeg encoding failed with exit code %d\n", ffmpegResult);
        }
    }

    return true;
}

bool SequenceProcessor::processDirectoryFG(const std::string& inputDir,
                                            const std::string& outputDir,
                                            const AppConfig& config,
                                            std::string& errorMsg) {
    errorMsg.clear();
    std::fprintf(stdout, "[FG] Starting FG-only pipeline\n");
    std::fflush(stdout);
    std::fprintf(stdout, "[FG]   input:  %s\n", inputDir.c_str());
    std::fflush(stdout);
    std::fprintf(stdout, "[FG]   output: %s\n", outputDir.c_str());
    std::fflush(stdout);
    std::fprintf(stdout, "[FG]   interpolate=%dx\n", config.interpolateFactor);
    std::fflush(stdout);

    std::vector<SequenceFrameInfo> frames;
    if (!scanAndSort(inputDir, frames, errorMsg)) {
        std::fprintf(stderr, "[FG] scanAndSort failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[FG] Found %d EXR frames\n", static_cast<int>(frames.size()));
    std::fflush(stdout);

    if (frames.size() < 2) {
        errorMsg = "At least 2 frames required for interpolation";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    if (ec) {
        errorMsg = "Failed to create output directory: " + outputDir;
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }

    if (pathsAreEquivalent(inputDir, outputDir)) {
        errorMsg = "Input and output directories must be different for interpolation.";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }

    CameraDataLoader cameraLoader;
    std::fprintf(stdout, "[FG] Loading camera data: %s\n", config.cameraDataFile.c_str());
    std::fflush(stdout);
    if (!cameraLoader.load(config.cameraDataFile, errorMsg)) {
        std::fprintf(stderr, "[FG] Camera data load failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[FG] Camera data loaded (%d frames)\n", cameraLoader.frameCount());
    std::fflush(stdout);

    for (const SequenceFrameInfo& frameInfo : frames) {
        if (frameInfo.frameNumber == std::numeric_limits<int>::max()) {
            errorMsg = "Input filename is missing a trailing frame number: " + frameInfo.path.filename().string();
            std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
            return false;
        }
        if (!cameraLoader.hasFrame(frameInfo.frameNumber)) {
            errorMsg = "Camera data missing for frame " + std::to_string(frameInfo.frameNumber);
            std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
            return false;
        }
    }

    unsigned int multiFrameCount = 0;
    if (config.interpolateFactor == 2) {
        multiFrameCount = 1;
    } else if (config.interpolateFactor == 4) {
        multiFrameCount = 3;
    } else {
        errorMsg = "Unsupported interpolation factor: " + std::to_string(config.interpolateFactor) +
                   ". Only 2x and 4x are supported.";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }

    if (!m_ngx.isDlssFGAvailable()) {
        errorMsg = "DLSS Frame Generation is not supported on this GPU. RTX 40+ required.";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[FG] DLSS-FG available\n");
    std::fflush(stdout);

    if (config.interpolateFactor == 4 && m_ngx.maxMultiFrameCount() < 3) {
        errorMsg = "4x frame generation requires RTX 50 series or newer. This GPU supports up to " +
                   std::to_string(m_ngx.maxMultiFrameCount() + 1) + "x.";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        return false;
    }

    ExrReader firstReader;
    if (!firstReader.open(frames.front().path.string(), errorMsg)) {
        std::fprintf(stderr, "[FG] Failed to open first EXR: %s\n", errorMsg.c_str());
        return false;
    }

    const int expectedWidth = firstReader.width();
    const int expectedHeight = firstReader.height();
    std::fprintf(stdout, "[FG] Resolution: %dx%d\n", expectedWidth, expectedHeight);
    std::fflush(stdout);

    std::fprintf(stdout, "[FG] Creating DLSS-FG feature...\n");
    std::fflush(stdout);
    VkCommandBuffer createCmdBuf = VK_NULL_HANDLE;
    if (!allocateCommandBuffer(m_ctx, createCmdBuf, errorMsg)) {
        std::fprintf(stderr, "[FG] Failed to allocate command buffer: %s\n", errorMsg.c_str());
        return false;
    }
    if (!beginCommandBuffer(createCmdBuf, errorMsg) ||
        !m_ngx.createDlssFG(static_cast<uint32_t>(expectedWidth),
                            static_cast<uint32_t>(expectedHeight),
                            static_cast<unsigned int>(VK_FORMAT_R16G16B16A16_SFLOAT),
                            createCmdBuf,
                            errorMsg)) {
        std::fprintf(stderr, "[FG] DLSS-FG feature creation failed: %s\n", errorMsg.c_str());
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &createCmdBuf);
        return false;
    }
    DlssFgFeatureGuard featureGuard(m_ngx);
    if (!submitAndWait(m_ctx, createCmdBuf, errorMsg)) {
        std::fprintf(stderr, "[FG] DLSS-FG feature submit failed: %s\n", errorMsg.c_str());
        return false;
    }
    std::fprintf(stdout, "[FG] DLSS-FG feature created\n");
    std::fflush(stdout);

    const int64_t maxMemory = static_cast<int64_t>(config.memoryBudgetGB) * 1024LL * 1024LL * 1024LL;
    TexturePool pool(m_ctx, m_texturePipeline, maxMemory);
    TextureHandle color;
    TextureHandle depth;
    TextureHandle motion;

    try {
        color = pool.acquire(expectedWidth, expectedHeight, 4, VK_FORMAT_R16G16B16A16_SFLOAT);
        depth = pool.acquire(expectedWidth, expectedHeight, 1, VK_FORMAT_R32_SFLOAT);
        motion = pool.acquire(expectedWidth, expectedHeight, 2, VK_FORMAT_R16G16_SFLOAT);
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }

    ChannelMapper channelMapper;
    MvConverter mvConverter;
    DlssFgProcessor fgProcessor(m_ctx, m_ngx);

    // Initialize FG transport encoding (PQ or Custom LUT)
    std::fprintf(stdout, "[FG] Initializing FG transport...\n");
    std::fflush(stdout);
    PqTransferProcessor pqTransferFG(m_ctx);
    bool pqReadyFG = false;
    LutProcessor forwardLutFG(m_ctx);
    LutProcessor inverseLutFG(m_ctx);
    bool forwardLutReadyFG = false;
    bool inverseLutReadyFG = false;
    if (!initPqTransfer(m_ctx, config, pqTransferFG, pqReadyFG, errorMsg)) {
        std::fprintf(stderr, "[FG] PQ transfer init failed: %s\n", errorMsg.c_str());
        return false;
    }
    if (!initLutProcessors(m_ctx, config, forwardLutFG, inverseLutFG,
                           forwardLutReadyFG, inverseLutReadyFG, errorMsg)) {
        std::fprintf(stderr, "[FG] LUT init failed: %s\n", errorMsg.c_str());
        return false;
    }
    const bool transportReadyFG = pqReadyFG || forwardLutReadyFG;
    const bool inverseReadyFG = pqReadyFG ? config.inverseTonemapEnabled : inverseLutReadyFG;
    std::fprintf(stdout, "[FG] Transport init done (pq=%s, lut_fwd=%s, inverse=%s)\n",
                 pqReadyFG ? "yes" : "no", forwardLutReadyFG ? "yes" : "no",
                 inverseReadyFG ? "yes" : "no");
    std::fflush(stdout);

    FramePrefetcher prefetcher(channelMapper, mvConverter, 3, expectedWidth, expectedHeight);
    prefetcher.start(frames);

    auto firstPrefetched = prefetcher.getNext();
    if (!firstPrefetched || !firstPrefetched->valid) {
        errorMsg = "Failed to prefetch first frame";
        return false;
    }

    auto& firstMappedBuffers = firstPrefetched->mappedBuffers;

    int outputFrameCounter = 1;
    const std::filesystem::path outputPath(outputDir);
    AsyncExrWriter asyncWriterFG;
    std::fprintf(stdout, "[FG] Async EXR writer started\n");
    std::fflush(stdout);
    asyncWriterFG.submit(buildWriteJobCopy(outputPath, outputFrameCounter,
                                           expectedWidth, expectedHeight,
                                           firstMappedBuffers.color, config));
    ++outputFrameCounter;

    for (size_t i = 1; i < frames.size(); ++i) {
        const SequenceFrameInfo& previousFrameInfo = frames[i - 1];
        const SequenceFrameInfo& currentFrameInfo = frames[i];
        const std::string filename = currentFrameInfo.path.filename().string();
        std::fprintf(stdout,
                     "[FG] Processing frame pair %d/%d: %s -> %s\n",
                     static_cast<int>(i),
                     static_cast<int>(frames.size() - 1),
                     previousFrameInfo.path.filename().string().c_str(),
                     filename.c_str());
        std::fflush(stdout);

        const bool hasGap = previousFrameInfo.frameNumber != std::numeric_limits<int>::max() &&
                            currentFrameInfo.frameNumber != std::numeric_limits<int>::max() &&
                            (currentFrameInfo.frameNumber - previousFrameInfo.frameNumber) > 1;
        const bool resetFlag = (i == 1) || hasGap;
        if (hasGap) {
            std::fprintf(stderr,
                         "Warning: frame gap detected between %d and %d; resetting temporal history\n",
                         previousFrameInfo.frameNumber,
                         currentFrameInfo.frameNumber);
        }

        if (!cameraLoader.hasFrame(previousFrameInfo.frameNumber)) {
            errorMsg = "Camera data missing for frame " + std::to_string(previousFrameInfo.frameNumber);
            return false;
        }
        if (!cameraLoader.hasFrame(currentFrameInfo.frameNumber)) {
            errorMsg = "Camera data missing for frame " + std::to_string(currentFrameInfo.frameNumber);
            return false;
        }

        auto prefetched = prefetcher.getNext();
        if (!prefetched || !prefetched->valid) {
            errorMsg = "Failed to prefetch frame: " + currentFrameInfo.path.filename().string();
            return false;
        }

        auto& mappedBuffers = prefetched->mappedBuffers;

        DlssFgCameraParams camParams{};
        if (!cameraLoader.computePairParams(currentFrameInfo.frameNumber,
                                            previousFrameInfo.frameNumber,
                                            camParams,
                                            errorMsg)) {
            return false;
        }

        const MvConvertResult& mvResult = prefetched->mvResult;

        try {
            pool.updateData(color, mappedBuffers.color.data());
            
            // Convert Blender world-space depth to DLSS NDC depth [0, 1]
            const CameraFrameData& camFrame = cameraLoader.getFrame(currentFrameInfo.frameNumber);
            const std::vector<float> convertedDepth = convertWorldDepthToNdc(mappedBuffers.depth, &camFrame);
            pool.updateData(depth, convertedDepth.data());
            
            pool.updateData(motion, mvResult.mvXY.data());

            for (unsigned int multiFrameIndex = 1; multiFrameIndex <= multiFrameCount; ++multiFrameIndex) {
                const std::vector<float> outputInit(static_cast<size_t>(expectedWidth) *
                                                        static_cast<size_t>(expectedHeight) *
                                                        4u,
                                                    0.0f);
                TextureHandle outputInterp;
                TextureHandle tonemappedBuf;

                try {
                    outputInterp = m_texturePipeline.upload(outputInit.data(),
                                                            expectedWidth,
                                                            expectedHeight,
                                                            4,
                                                            VK_FORMAT_R16G16B16A16_SFLOAT);

                    // Temporary texture for encoded backbuffer (PQ or forward LUT output)
                    if (transportReadyFG) {
                        tonemappedBuf = m_texturePipeline.upload(outputInit.data(),
                                                                  expectedWidth,
                                                                  expectedHeight,
                                                                  4,
                                                                  VK_FORMAT_R16G16B16A16_SFLOAT);
                    }

                    DlssFgFrameInput fgInput{};
                    fgInput.backbuffer = transportReadyFG ? tonemappedBuf.image : color.image;
                    fgInput.backbufferView = transportReadyFG ? tonemappedBuf.view : color.view;
                    fgInput.depth = depth.image;
                    fgInput.depthView = depth.view;
                    fgInput.motionVectors = motion.image;
                    fgInput.motionView = motion.view;
                    fgInput.outputInterp = outputInterp.image;
                    fgInput.outputInterpView = outputInterp.view;
                    fgInput.width = static_cast<uint32_t>(expectedWidth);
                    fgInput.height = static_cast<uint32_t>(expectedHeight);
                    fgInput.reset = resetFlag && (multiFrameIndex == 1);
                    fgInput.cameraParams = camParams;
                    fgInput.multiFrameCount = multiFrameCount;
                    fgInput.multiFrameIndex = multiFrameIndex;

                    VkCommandBuffer evalCmdBuf = VK_NULL_HANDLE;
                    if (!allocateCommandBuffer(m_ctx, evalCmdBuf, errorMsg)) {
                        throw std::runtime_error(errorMsg);
                    }
                    if (!beginCommandBuffer(evalCmdBuf, errorMsg)) {
                        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &evalCmdBuf);
                        throw std::runtime_error(errorMsg);
                    }

                    try {
                        // Forward transport: color (scene-linear) -> tonemappedBuf (PQ or display-referred)
                        if (transportReadyFG) {
                            transitionImage(evalCmdBuf,
                                            color.image,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL);
                            transitionImage(evalCmdBuf,
                                            tonemappedBuf.image,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL);
                            if (pqReadyFG) {
                                pqTransferFG.apply(evalCmdBuf,
                                                   color.image,
                                                   color.view,
                                                   tonemappedBuf.image,
                                                   tonemappedBuf.view,
                                                   expectedWidth,
                                                   expectedHeight,
                                                   true); // encode: scene-linear -> PQ
                            } else {
                                forwardLutFG.apply(evalCmdBuf,
                                                   color.image,
                                                   color.view,
                                                   tonemappedBuf.image,
                                                   tonemappedBuf.view,
                                                   expectedWidth,
                                                   expectedHeight);
                            }
                            // Transition color back — pool textures expect SHADER_READ_ONLY
                            transitionImage(evalCmdBuf,
                                            color.image,
                                            VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                            // tonemappedBuf stays GENERAL for NGX to read
                        }

                        transitionImage(evalCmdBuf,
                                        outputInterp.image,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        VK_IMAGE_LAYOUT_GENERAL);
                        if (!fgProcessor.evaluate(evalCmdBuf, fgInput, errorMsg)) {
                            throw std::runtime_error(errorMsg);
                        }

                        // Inverse transport: outputInterp (PQ or display-referred) -> tonemappedBuf (scene-linear)
                        if (inverseReadyFG && transportReadyFG) {
                            // tonemappedBuf is still GENERAL from forward pass
                            // outputInterp is GENERAL from FG evaluate
                            if (pqReadyFG) {
                                pqTransferFG.apply(evalCmdBuf,
                                                   outputInterp.image,
                                                   outputInterp.view,
                                                   tonemappedBuf.image,
                                                   tonemappedBuf.view,
                                                   expectedWidth,
                                                   expectedHeight,
                                                   false); // decode: PQ -> scene-linear
                            } else {
                                inverseLutFG.apply(evalCmdBuf,
                                                   outputInterp.image,
                                                   outputInterp.view,
                                                   tonemappedBuf.image,
                                                   tonemappedBuf.view,
                                                   expectedWidth,
                                                   expectedHeight,
                                                   false); // display-referred input, no log2 shaper
                            }
                            transitionImage(evalCmdBuf,
                                            tonemappedBuf.image,
                                            VK_IMAGE_LAYOUT_GENERAL,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        }

                        transitionImage(evalCmdBuf,
                                        outputInterp.image,
                                        VK_IMAGE_LAYOUT_GENERAL,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    } catch (...) {
                        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &evalCmdBuf);
                        throw;
                    }

                    if (!submitAndWait(m_ctx, evalCmdBuf, errorMsg)) {
                        throw std::runtime_error(errorMsg);
                    }

                    // Download the result — from tonemappedBuf if inverse was applied, else outputInterp
                    const TextureHandle& dlSource = (inverseReadyFG && transportReadyFG) ? tonemappedBuf : outputInterp;
                    std::vector<float> outputData = m_texturePipeline.download(dlSource);
                    forceOpaqueAlpha(outputData);

                    asyncWriterFG.submit(buildWriteJob(outputPath, outputFrameCounter,
                                                       expectedWidth, expectedHeight,
                                                       outputData, config));
                    ++outputFrameCounter;
                } catch (...) {
                    destroyHandle(m_texturePipeline, outputInterp);
                    destroyHandle(m_texturePipeline, tonemappedBuf);
                    throw;
                }

                destroyHandle(m_texturePipeline, outputInterp);
                destroyHandle(m_texturePipeline, tonemappedBuf);
            }
        } catch (const std::exception& ex) {
            errorMsg = ex.what();
            return false;
        }

        asyncWriterFG.submit(buildWriteJobCopy(outputPath, outputFrameCounter,
                                               expectedWidth, expectedHeight,
                                               mappedBuffers.color, config));
        ++outputFrameCounter;
    }

    // Wait for all async writes to complete before releasing GPU resources
    std::fprintf(stdout, "[FG] Flushing async writer (%d errors so far)...\n", asyncWriterFG.errorCount());
    std::fflush(stdout);
    asyncWriterFG.flush();
    if (asyncWriterFG.errorCount() > 0) {
        errorMsg = "Async EXR writer encountered " + std::to_string(asyncWriterFG.errorCount()) + " write error(s)";
        std::fprintf(stderr, "[FG] %s\n", errorMsg.c_str());
        pool.releaseAll();
        return false;
    }
    pool.releaseAll();
    std::fprintf(stdout, "[FG] Done. %d output frames written.\n", outputFrameCounter - 1);
    std::fflush(stdout);

    if (config.encodeVideo) {
        std::filesystem::path videoOutputPath(config.videoOutputFile);
        if (videoOutputPath.is_relative()) {
            videoOutputPath = std::filesystem::path(outputDir) / videoOutputPath;
        }

        const std::filesystem::path inputPattern = std::filesystem::path(outputDir) / "frame_%04d.exr";
        std::ostringstream command;
        command << "ffmpeg -y -framerate " << config.fps << " -i " << quote(inputPattern)
                << " -c:v libx264 -pix_fmt yuv420p -crf 18 " << quote(videoOutputPath);
        const int ffmpegResult = std::system(command.str().c_str());
        if (ffmpegResult != 0) {
            std::fprintf(stderr, "Warning: ffmpeg encoding failed with exit code %d\n", ffmpegResult);
        }
    }

    return true;
}
