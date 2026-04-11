#include "pipeline/sequence_processor.h"

#include "cli/config.h"
#include "core/channel_mapper.h"
#include "core/exr_reader.h"
#include "core/exr_writer.h"
#include "core/mv_converter.h"
#include "dlss/dlss_rr_processor.h"
#include "dlss/ngx_wrapper.h"
#include "gpu/texture_pipeline.h"
#include "gpu/vulkan_context.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {

struct DlssFeatureGuard {
    explicit DlssFeatureGuard(NgxContext& ngxIn) : ngx(ngxIn) {}

    ~DlssFeatureGuard() {
        ngx.releaseDlssRR();
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

} // namespace

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

    const bool requestedUnsupportedPasses = hasPass(config.outputPasses, OutputPass::Depth) ||
                                            hasPass(config.outputPasses, OutputPass::Normals);
    if (requestedUnsupportedPasses) {
        std::fprintf(stderr,
                     "Warning: only 'beauty' pass output is currently supported. Other passes will be ignored.\n");
    }

    if (config.scaleFactor <= 0) {
        errorMsg = "Scale factor must be positive.";
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
    DlssRRProcessor processor(m_ctx, m_ngx);
    DlssFeatureGuard featureGuard(m_ngx);

    bool featureCreated = false;
    bool anyFrameSucceeded = false;
    int expectedInputWidth = 0;
    int expectedInputHeight = 0;
    int expectedOutputWidth = 0;
    int expectedOutputHeight = 0;

    for (size_t index = 0; index < frames.size(); ++index) {
        const SequenceFrameInfo& frameInfo = frames[index];
        const std::string filename = frameInfo.path.filename().string();
        std::fprintf(stdout, "Processing frame %d/%d: %s\n",
                     static_cast<int>(index + 1),
                     static_cast<int>(frames.size()),
                     filename.c_str());

        if (index > 0 && resetFlags[index]) {
            std::fprintf(stderr,
                         "Warning: frame gap detected between %d and %d; resetting temporal history\n",
                         frames[index - 1].frameNumber,
                         frameInfo.frameNumber);
        }

        std::string frameError;

        try {
            ExrReader reader;
            if (!reader.open(frameInfo.path.string(), frameError)) {
                std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), frameError.c_str());
                continue;
            }

            if (!featureCreated) {
                expectedInputWidth = reader.width();
                expectedInputHeight = reader.height();
                expectedOutputWidth = expectedInputWidth * config.scaleFactor;
                expectedOutputHeight = expectedInputHeight * config.scaleFactor;

                VkCommandBuffer createCmdBuf = VK_NULL_HANDLE;
                if (!allocateCommandBuffer(m_ctx, createCmdBuf, frameError) ||
                    !beginCommandBuffer(createCmdBuf, frameError) ||
                    !m_ngx.createDlssRR(expectedInputWidth,
                                        expectedInputHeight,
                                        expectedOutputWidth,
                                        expectedOutputHeight,
                                        config.quality,
                                        createCmdBuf,
                                        frameError) ||
                    !submitAndWait(m_ctx, createCmdBuf, frameError)) {
                    if (createCmdBuf != VK_NULL_HANDLE && !frameError.empty()) {
                        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), 1, &createCmdBuf);
                    }
                    std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), frameError.c_str());
                    continue;
                }

                featureCreated = true;
            }

            if (reader.width() != expectedInputWidth || reader.height() != expectedInputHeight) {
                std::ostringstream message;
                message << "Input resolution mismatch. Expected " << expectedInputWidth << "x" << expectedInputHeight
                        << ", got " << reader.width() << "x" << reader.height();
                std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), message.str().c_str());
                continue;
            }

            MappedBuffers mappedBuffers;
            if (!channelMapper.mapFromExr(reader, mappedBuffers, frameError)) {
                std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), frameError.c_str());
                continue;
            }

            const MvConvertResult mvResult = MvConverter::convert(mappedBuffers.motionVectors.data(),
                                                                  expectedInputWidth,
                                                                  expectedInputHeight);
            const std::vector<float> diffuseRgba = expandRgbToRgba(mappedBuffers.diffuseAlbedo, 1.0f);
            const std::vector<float> specularRgba = expandRgbToRgba(mappedBuffers.specularAlbedo, 1.0f);
            const std::vector<float> normalsRgba = expandRgbToRgba(mappedBuffers.normals, 1.0f);
            const std::vector<float> outputInit(static_cast<size_t>(expectedOutputWidth) *
                                                    static_cast<size_t>(expectedOutputHeight) *
                                                    4u,
                                                0.0f);

            TextureHandle color;
            TextureHandle depth;
            TextureHandle motion;
            TextureHandle diffuse;
            TextureHandle specular;
            TextureHandle normals;
            TextureHandle roughness;
            TextureHandle output;

            try {
                color = m_texturePipeline.upload(mappedBuffers.color.data(),
                                                 expectedInputWidth,
                                                 expectedInputHeight,
                                                 4,
                                                 VK_FORMAT_R16G16B16A16_SFLOAT);
                depth = m_texturePipeline.upload(mappedBuffers.depth.data(),
                                                 expectedInputWidth,
                                                 expectedInputHeight,
                                                 1,
                                                 VK_FORMAT_R32_SFLOAT);
                motion = m_texturePipeline.upload(mvResult.mvXY.data(),
                                                  expectedInputWidth,
                                                  expectedInputHeight,
                                                  2,
                                                  VK_FORMAT_R16G16_SFLOAT);
                diffuse = m_texturePipeline.upload(diffuseRgba.data(),
                                                   expectedInputWidth,
                                                   expectedInputHeight,
                                                   4,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
                specular = m_texturePipeline.upload(specularRgba.data(),
                                                    expectedInputWidth,
                                                    expectedInputHeight,
                                                    4,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);
                normals = m_texturePipeline.upload(normalsRgba.data(),
                                                   expectedInputWidth,
                                                   expectedInputHeight,
                                                   4,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
                roughness = m_texturePipeline.upload(mappedBuffers.roughness.data(),
                                                     expectedInputWidth,
                                                     expectedInputHeight,
                                                     1,
                                                     VK_FORMAT_R32_SFLOAT);
                output = m_texturePipeline.upload(outputInit.data(),
                                                  expectedOutputWidth,
                                                  expectedOutputHeight,
                                                  4,
                                                  VK_FORMAT_R16G16B16A16_SFLOAT);

                DlssFrameInput frame{};
                frame.color = color.image;
                frame.colorView = color.view;
                frame.depth = depth.image;
                frame.depthView = depth.view;
                frame.motionVectors = motion.image;
                frame.motionView = motion.view;
                frame.diffuseAlbedo = diffuse.image;
                frame.diffuseView = diffuse.view;
                frame.specularAlbedo = specular.image;
                frame.specularView = specular.view;
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
                frame.mvScaleX = mvResult.scaleX;
                frame.mvScaleY = mvResult.scaleY;
                frame.reset = resetFlags[index] || !anyFrameSucceeded;

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

                const std::vector<float> outputData = m_texturePipeline.download(output);
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

            destroyHandle(m_texturePipeline, output);
            destroyHandle(m_texturePipeline, roughness);
            destroyHandle(m_texturePipeline, normals);
            destroyHandle(m_texturePipeline, specular);
            destroyHandle(m_texturePipeline, diffuse);
            destroyHandle(m_texturePipeline, motion);
            destroyHandle(m_texturePipeline, depth);
            destroyHandle(m_texturePipeline, color);
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "Frame %s failed: %s\n", filename.c_str(), ex.what());
        }
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
