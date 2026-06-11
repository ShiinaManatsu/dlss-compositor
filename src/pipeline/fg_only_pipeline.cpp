#include "fg_only_pipeline.h"

#include "../core/log.h"
#include "../gpu/passes/preview_pass.h"
#include "../io/prores_writer.h"
#include "fg_camera.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dlss {

namespace {

constexpr int kPrefetchDepth = 3;

void requirePipelinePointers(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher) {
    if (!ctx) throw std::invalid_argument("FgOnlyPipeline: VulkanContext must not be null");
    if (!ngx) throw std::invalid_argument("FgOnlyPipeline: NgxContext must not be null");
    if (!dispatcher) throw std::invalid_argument("FgOnlyPipeline: RpcDispatcher must not be null");
}

std::filesystem::path inferInputDir(const FgOnlyConfig& config) {
    if (!config.inputDir.empty()) {
        return std::filesystem::path(config.inputDir);
    }
    if (!config.outputDir.empty()) {
        return std::filesystem::path(config.outputDir);
    }
    return std::filesystem::current_path();
}

std::vector<std::pair<int, std::string>> sortedFrameKeys(const DlssConfig& config) {
    std::vector<std::pair<int, std::string>> frames;
    frames.reserve(config.frames.size());

    for (const auto& [key, camera] : config.frames) {
        (void)camera;
        try {
            frames.emplace_back(std::stoi(key), key);
        } catch (...) {
            LOG_WARN("[FG-only] Ignoring non-numeric frame key: {}", key);
        }
    }

    std::sort(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });
    return frames;
}

std::string inputPattern(const std::string& pattern) {
    if (pattern == R"(^\d{4}\.exr$)" || pattern == R"(\d{4}\.exr)") {
        return "####.exr";
    }
    if (pattern == R"(^\d{4}\.png$)" || pattern == R"(\d{4}\.png)") {
        return "####.png";
    }
    return pattern;
}

std::vector<std::filesystem::path> buildFramePaths(
    const DlssConfig& config,
    const std::filesystem::path& baseDir) {
    std::vector<std::filesystem::path> paths;
    const std::string pattern = inputPattern(config.filename_pattern);
    for (const auto& [frameNumber, frameKey] : sortedFrameKeys(config)) {
        (void)frameKey;
        paths.emplace_back(baseDir / AsyncFrameWriter::formatPath(pattern, frameNumber));
    }
    return paths;
}

const CameraData* findCamera(const DlssConfig& config, const std::string& key) {
    const auto it = config.frames.find(key);
    return it != config.frames.end() ? &it->second : nullptr;
}

void identity(float (&m)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            m[row][col] = (row == col) ? 1.0f : 0.0f;
        }
    }
}

void copyMatrix(float (&dst)[4][4], const float (&src)[4][4]) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            dst[row][col] = src[row][col];
        }
    }
}

void transitionStorageImageForWrite(VkCommandBuffer cmd, const PooledTextureHandle& texture) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barrier.srcAccessMask       = 0;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = texture->image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionStorageToShaderRead(VkCommandBuffer cmd, const PooledTextureHandle& texture) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = texture->image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionForFgEvaluate(VkCommandBuffer cmd,
                             const PooledTextureHandle& display,
                             const PooledTextureHandle& depthNdc,
                             const PooledTextureHandle& mvDlss,
                             const std::vector<PooledTextureHandle>& interpOutputs) {
    std::array<VkImageMemoryBarrier2, 7> storage{};
    std::size_t count = 0;

    auto addBarrier = [&](const PooledTextureHandle& texture,
                          VkImageLayout oldLayout,
                          VkImageLayout newLayout,
                          VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess,
                          VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2& barrier = storage[count++];
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask        = srcStage;
        barrier.srcAccessMask       = srcAccess;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask       = dstAccess;
        barrier.oldLayout           = oldLayout;
        barrier.newLayout           = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = texture->image;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    };

    addBarrier(display,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    addBarrier(depthNdc,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    addBarrier(mvDlss,
               VK_IMAGE_LAYOUT_GENERAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (const auto& interp : interpOutputs) {
        addBarrier(interp,
                   VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_GENERAL,
                   VK_PIPELINE_STAGE_2_NONE,
                   0,
                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(count);
    dependency.pImageMemoryBarriers    = storage.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionFgOutputsForInverse(VkCommandBuffer cmd,
                                   const std::vector<PooledTextureHandle>& interpOutputs) {
    std::array<VkImageMemoryBarrier2, 4> storage{};
    std::size_t count = 0;
    for (const auto& interp : interpOutputs) {
        VkImageMemoryBarrier2& barrier = storage[count++];
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = interp->image;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }

    if (count == 0) return;
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(count);
    dependency.pImageMemoryBarriers    = storage.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

} // namespace

FgOnlyPipeline::FgOnlyPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher)
    : m_ctx(ctx), m_ngx(ngx), m_dispatcher(dispatcher) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
}

FgOnlyPipeline::~FgOnlyPipeline() = default;

void FgOnlyPipeline::run(const FgOnlyConfig& config) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
    m_stop.store(false);

    const DlssConfig& dlssCfg = config.dlssConfig;
    const auto frameKeys = sortedFrameKeys(dlssCfg);
    const int totalFrames = static_cast<int>(frameKeys.size());
    if (totalFrames == 0) {
        throw std::runtime_error("FgOnlyPipeline: dlssConfig has no frames");
    }
    if (dlssCfg.fps <= 0) {
        throw std::runtime_error("FgOnlyPipeline: fps must be positive");
    }
    if (dlssCfg.filename_pattern.empty()) {
        throw std::runtime_error("FgOnlyPipeline: filename_pattern must not be empty");
    }
    if (config.multiplier < 2 || config.multiplier > 6) {
        throw std::runtime_error("FgOnlyPipeline: multiplier must be between 2 and 6");
    }

    const int outputFrames = config.multiplier * totalFrames - (config.multiplier - 1);
    LOG_INFO("[FG-only] Starting pipeline: {} input frames -> {} output frames ({}x)",
             totalFrames, outputFrames, config.multiplier);

    m_dispatcher->progress().current_frame.store(0);
    m_dispatcher->progress().total_frames.store(outputFrames);
    m_dispatcher->progress().percent.store(0.0f);
    m_dispatcher->progress().dlss_current_frame.store(0);
    m_dispatcher->progress().dlss_total_frames.store(outputFrames);
    m_dispatcher->progress().dlss_percent.store(0.0f);
    m_dispatcher->progress().state.store(1);

    try {
        TexturePool pool(m_ctx);
        DlssFgPass fgPass(m_ngx, m_ctx, &pool);
        DepthConversionPass depthPass(m_ctx);
        MvConversionPass mvPass(m_ctx);
        TonemapMode tonemapMode = dlssCfg.is_hdr ? TonemapMode::HDR : TonemapMode::LDR;
        TonemapPass tonemapPass(m_ctx);
        InverseTonemapPass inversePass(m_ctx);
        PreviewPass previewPass(m_ctx);
        AsyncFrameWriter writer;
        std::unique_ptr<ProResProgressWriter> proresWriter;

        const uint32_t w = dlssCfg.output_resolution.width;
        const uint32_t h = dlssCfg.output_resolution.height;
        if (config.outputFormat == OutputFormat::ProRes422HQ) {
            proresWriter = std::make_unique<ProResProgressWriter>(
                std::filesystem::path(config.outputDir) / config.filenameTemplate,
                w,
                h,
                dlssCfg.fps,
                dlssCfg.is_hdr,
                outputFrames,
                [this](int current, int total) {
                    m_dispatcher->progress().current_frame.store(current);
                    m_dispatcher->progress().percent.store(
                        total > 0 ? static_cast<float>(current) / static_cast<float>(total) : 0.0f);
                });
        }

        fgPass.create(w, h, w, h, config.multiplier);
        FramePrefetcher prefetcher(buildFramePaths(dlssCfg, inferInputDir(config)), kPrefetchDepth);

        const VkImageUsageFlags sampledUpload =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        TextureDesc colorDesc{
            w,
            h,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            sampledUpload};
        TextureDesc displayDesc{
            w,
            h,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
        TextureDesc depthWorldDesc{
            w,
            h,
            VK_FORMAT_R32_SFLOAT,
            sampledUpload};
        TextureDesc depthNdcDesc{
            w,
            h,
            VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc mvBlenderDesc{
            w,
            h,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            sampledUpload};
        TextureDesc mvDlssDesc{
            w,
            h,
            VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc interpDesc{
            w,
            h,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc sceneLinearDesc{
            w,
            h,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};

        VkCommandPool cmdPool = m_ctx->createComputeCommandPool();
        int outputFrameIdx = 0;
        std::atomic<int> writtenFrames{0};

        auto enqueueSceneLinear = [&](PooledTextureHandle sceneLinear) {
            ++outputFrameIdx;
            WriteJob writeJob;
            writeJob.format        = config.outputFormat;
            writeJob.outputPath    = std::filesystem::path(config.outputDir) /
                                     AsyncFrameWriter::formatPath(config.filenameTemplate, outputFrameIdx);
            writeJob.width         = w;
            writeJob.height        = h;
            writeJob.fps           = dlssCfg.fps;
            writeJob.is_hdr        = dlssCfg.is_hdr;
            writeJob.encoderOptions = config.encoderOptions;
            writeJob.overwriteMode = config.overwriteMode;
            writeJob.pixels        = m_ctx->downloadImageRgba16f(sceneLinear->image, w, h);
            writeJob.completionCallback = [this, outputFrames, &writtenFrames]() {
                const int written = writtenFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
                m_dispatcher->progress().current_frame.store(written);
                m_dispatcher->progress().percent.store(
                    static_cast<float>(written) / static_cast<float>(outputFrames));
            };
            writer.enqueue(std::move(writeJob));

            m_dispatcher->progress().dlss_current_frame.store(outputFrameIdx);
            m_dispatcher->progress().dlss_percent.store(
                static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
        };

        auto writeFrameSceneLinear = [&](PooledTextureHandle srcDisplay) {
            if (config.previewRgba8Callback && (!config.wantsPreviewFrame || config.wantsPreviewFrame())) {
                PreviewReadback preview = previewPass.renderAndReadback(srcDisplay, PreviewInputMode::DisplayReady, dlssCfg.is_hdr);
                if (!preview.rgba8.empty()) {
                    config.previewRgba8Callback(preview.rgba8, preview.width, preview.height);
                }
            }

            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
            auto sceneLinear = pool.acquire(sceneLinearDesc);
            transitionStorageImageForWrite(cmd, sceneLinear);
            inversePass.execute(cmd, srcDisplay, sceneLinear, tonemapMode);
            transitionStorageToShaderRead(cmd, sceneLinear);
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            // Stage 7 dispatch: image sequence calls writer.enqueue through
            // enqueueSceneLinear, video writes the frame into the ProRes muxer.
            if (proresWriter) {
                ++outputFrameIdx;
                proresWriter->writeFrame(m_ctx->downloadImageRgba16f(sceneLinear->image, w, h));
                m_dispatcher->progress().dlss_current_frame.store(outputFrameIdx);
                m_dispatcher->progress().dlss_percent.store(
                    static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
            } else {
                // Stage 7 image-sequence path calls writer.enqueue via enqueueSceneLinear.
                enqueueSceneLinear(sceneLinear);
            }

            pool.release(sceneLinear);
        };

        int processedInputs = 0;
        for (int frameIdx = 0; frameIdx < totalFrames && !m_stop.load(); ++frameIdx) {
            const auto frameResult = prefetcher.getNext();
            if (!frameResult.has_value()) {
                LOG_WARN("[FG-only] Frame {} read error: {}",
                         frameIdx + 1, frameResult.error().message);
                continue;
            }

            const auto mapResult = ChannelMapper::map(frameResult.value());
            if (!mapResult.has_value()) {
                LOG_WARN("[FG-only] Frame {} map error: {}", frameIdx + 1, mapResult.error().message);
                continue;
            }
            const MappedBuffers& mapped = mapResult.value();

            auto colorTex   = pool.acquire(colorDesc);
            auto depthWorld = pool.acquire(depthWorldDesc);
            auto depthNdc   = pool.acquire(depthNdcDesc);
            auto mvBlender  = pool.acquire(mvBlenderDesc);
            auto mvDlss     = pool.acquire(mvDlssDesc);
            auto displayTex = pool.acquire(displayDesc);

            UploadBatch batch(m_ctx);
            batch.addUpload(colorTex, mapped.sceneColor.data(), mapped.sceneColor.size() * sizeof(std::uint16_t));
            batch.addUpload(depthWorld, mapped.depth.data(), mapped.depth.size() * sizeof(float));
            batch.addUpload(mvBlender, mapped.motionVector.data(), mapped.motionVector.size() * sizeof(std::uint16_t));
            batch.submit();

            std::vector<PooledTextureHandle> interpHandles;
            for (int j = 0; j < config.multiplier - 1; ++j) {
                interpHandles.push_back(pool.acquire(interpDesc));
            }

            const std::string& frameKey = frameKeys[static_cast<std::size_t>(frameIdx)].second;
            const CameraData* cam = findCamera(dlssCfg, frameKey);
            const CameraData* prevCam = frameIdx > 0
                ? findCamera(dlssCfg, frameKeys[static_cast<std::size_t>(frameIdx - 1)].second)
                : nullptr;
            float defaultProjection[4][4];
            identity(defaultProjection);
            const float (*projection)[4] = cam ? cam->projection : defaultProjection;

            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
            transitionStorageImageForWrite(cmd, depthNdc);
            depthPass.execute(cmd,
                              depthWorld,
                              depthNdc,
                              projection,
                              cam ? cam->near_clip : 0.1f,
                              cam ? cam->far_clip : 100.0f);
            mvPass.execute(cmd, mvBlender, mvDlss);
            transitionStorageImageForWrite(cmd, displayTex);

            // Stage 4: tonemap scene-linear input to the display-ready backbuffer FG requires.
            tonemapPass.execute(cmd, colorTex, displayTex, tonemapMode);

            transitionForFgEvaluate(cmd, displayTex, depthNdc, mvDlss, interpHandles);

            FgInputs fgIn{};
            fgIn.backbufferDisplayReady = displayTex->view;
            fgIn.depthNdcRenderRes      = depthNdc->view;
            fgIn.motionVecDlssRenderRes = mvDlss->view;
            fgIn.backbufferImage        = displayTex->image;
            fgIn.depthImage             = depthNdc->image;
            fgIn.motionVecImage         = mvDlss->image;
            fgIn.outputInterpImages.reserve(interpHandles.size());
            fgIn.outputInterp.reserve(interpHandles.size());
            for (const auto& handle : interpHandles) {
                fgIn.outputInterp.push_back(handle->view);
                fgIn.outputInterpImages.push_back(handle->image);
            }
            fgIn.backbufferFormat = displayTex->desc.format;
            fgIn.depthFormat      = depthNdc->desc.format;
            fgIn.motionVecFormat  = mvDlss->desc.format;
            fgIn.width            = w;
            fgIn.height           = h;
            fgIn.is_hdr           = dlssCfg.is_hdr;
            fgIn.reset            = (frameIdx == 0);
            fgIn.multiplier       = config.multiplier;
            fillFgCameraInputs(fgIn, cam, prevCam);

            // Stage 5: multi-frame FG eval writes multiplier-1 interpolated frames.
            fgPass.evaluate(cmd, fgIn);
            transitionFgOutputsForInverse(cmd, interpHandles);
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            // Stage 6 + 7: write A, generated(A→B), B. The first eval warms FG history.
            if (frameIdx > 0) {
                for (const auto& handle : interpHandles) {
                    writeFrameSceneLinear(handle);
                }
            }
            if (config.previewRgba8Callback && (!config.wantsPreviewFrame || config.wantsPreviewFrame())) {
                PreviewReadback preview = previewPass.renderAndReadback(displayTex, PreviewInputMode::DisplayReady, dlssCfg.is_hdr);
                if (!preview.rgba8.empty()) {
                    config.previewRgba8Callback(preview.rgba8, preview.width, preview.height);
                }
            }
            enqueueSceneLinear(colorTex);
            ++processedInputs;

            pool.release(colorTex);
            pool.release(depthWorld);
            pool.release(depthNdc);
            pool.release(mvBlender);
            pool.release(mvDlss);
            pool.release(displayTex);
            for (auto& handle : interpHandles) {
                pool.release(handle);
            }

            LOG_INFO("[FG-only] Input frame {}/{} done, output idx {}",
                     frameIdx + 1, totalFrames, outputFrameIdx);
        }

        if (processedInputs != totalFrames) {
            throw std::runtime_error("FgOnlyPipeline: processed " + std::to_string(processedInputs) +
                                     " of " + std::to_string(totalFrames) + " input frames");
        }
        if (proresWriter) {
            proresWriter->close();
        } else {
            writer.flush();
        }
        m_dispatcher->progress().current_frame.store(outputFrames);
        m_dispatcher->progress().percent.store(1.0f);
        m_dispatcher->progress().dlss_current_frame.store(outputFrames);
        m_dispatcher->progress().dlss_percent.store(1.0f);
        vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);

        m_dispatcher->progress().state.store(2);
        LOG_INFO("[FG-only] Pipeline complete: {} output frames", outputFrameIdx);
    } catch (...) {
        m_dispatcher->progress().state.store(3);
        throw;
    }
}

} // namespace dlss
