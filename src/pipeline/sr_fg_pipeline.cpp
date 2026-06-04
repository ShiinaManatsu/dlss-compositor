#include "sr_fg_pipeline.h"

#include "../core/log.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dlss {

namespace {

constexpr int kPrefetchDepth = 3;

void requirePipelinePointers(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher) {
    if (!ctx) throw std::invalid_argument("SrFgPipeline: VulkanContext must not be null");
    if (!ngx) throw std::invalid_argument("SrFgPipeline: NgxContext must not be null");
    if (!dispatcher) throw std::invalid_argument("SrFgPipeline: RpcDispatcher must not be null");
}

std::filesystem::path inferInputDir(const SrFgConfig& config) {
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
            LOG_WARN("[SR+FG] Ignoring non-numeric frame key: {}", key);
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

void transitionForSrEvaluate(VkCommandBuffer cmd,
                             const PooledTextureHandle& colorOutput,
                             const PooledTextureHandle& depthNdc,
                             const PooledTextureHandle& mvDlss) {
    std::array<VkImageMemoryBarrier2, 3> barriers{};

    barriers[0].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask        = VK_PIPELINE_STAGE_2_NONE;
    barriers[0].srcAccessMask       = 0;
    barriers[0].dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[0].dstAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[0].oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image               = colorOutput->image;
    barriers[0].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[1].srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[1].srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[1].dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[1].dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[1].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barriers[1].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].image               = depthNdc->image;
    barriers[1].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[2] = barriers[1];
    barriers[2].image = mvDlss->image;

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionForFgEvaluate(VkCommandBuffer cmd,
                             const PooledTextureHandle& display,
                             const std::vector<PooledTextureHandle>& interpOutputs) {
    std::array<VkImageMemoryBarrier2, 4> barriers{};
    std::size_t count = 0;

    auto addBarrier = [&](const PooledTextureHandle& texture,
                          VkImageLayout oldLayout,
                          VkImageLayout newLayout,
                          VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess,
                          VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2& barrier = barriers[count++];
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
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionFgOutputsForInverse(VkCommandBuffer cmd,
                                   const std::vector<PooledTextureHandle>& interpOutputs) {
    std::array<VkImageMemoryBarrier2, 3> barriers{};
    std::size_t count = 0;
    for (const auto& interp : interpOutputs) {
        VkImageMemoryBarrier2& barrier = barriers[count++];
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
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void fillFgCameraInputs(FgInputs& fgIn, const CameraData* cam) {
    float fallback[4][4];
    identity(fallback);

    if (cam) {
        copyMatrix(fgIn.viewToClip, cam->projection);
    } else {
        copyMatrix(fgIn.viewToClip, fallback);
    }
    copyMatrix(fgIn.clipToPrevClip, fallback);
    copyMatrix(fgIn.prevClipToClip, fallback);

    fgIn.camNear     = cam ? cam->near_clip : 0.1f;
    fgIn.camFar      = cam ? cam->far_clip : 100.0f;
    fgIn.fov         = cam ? cam->fov : 1.0471976f;
    fgIn.aspectRatio = cam ? cam->aspect_ratio : 1.0f;
    fgIn.camPosX = fgIn.camPosY = fgIn.camPosZ = 0.0f;
    fgIn.camUpX = 0.0f;
    fgIn.camUpY = 1.0f;
    fgIn.camUpZ = 0.0f;
    fgIn.camRightX = 1.0f;
    fgIn.camRightY = 0.0f;
    fgIn.camRightZ = 0.0f;
    fgIn.camFwdX = 0.0f;
    fgIn.camFwdY = 0.0f;
    fgIn.camFwdZ = -1.0f;
}

} // namespace

SrFgPipeline::SrFgPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher)
    : m_ctx(ctx), m_ngx(ngx), m_dispatcher(dispatcher) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
}

SrFgPipeline::~SrFgPipeline() = default;

bool SrFgPipeline::shouldReset(int frameIdx, const DecodedFrame& frame) const {
    if (frameIdx == 0) return true;           // First frame always resets
    if (frame.has_gap_after) return true;     // Gap in frame sequence
    // Camera cut via matrix discontinuity: threshold TBD (sentinel = no trigger)
    // Will be calibrated in integration testing
    return false;
}

void SrFgPipeline::run(const SrFgConfig& config) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
    m_stop.store(false);

    const DlssConfig& dlssCfg = config.dlssConfig;
    const auto frameKeys = sortedFrameKeys(dlssCfg);
    const int inputFrames = static_cast<int>(frameKeys.size());
    if (inputFrames == 0) {
        throw std::runtime_error("SrFgPipeline: dlssConfig has no frames");
    }
    if (dlssCfg.fps <= 0) {
        throw std::runtime_error("SrFgPipeline: fps must be positive");
    }
    if (dlssCfg.filename_pattern.empty()) {
        throw std::runtime_error("SrFgPipeline: filename_pattern must not be empty");
    }
    if (config.fgMultiplier < 2 || config.fgMultiplier > 4) {
        throw std::runtime_error("SrFgPipeline: fgMultiplier must be 2, 3, or 4");
    }

    // Output frame count: SR outputs N frames; FG generates (mult-1) interp per input pair.
    const int outputFrames = config.fgMultiplier * inputFrames - (config.fgMultiplier - 1);

    LOG_INFO("[SR+FG] Starting: {} input -> {} output ({}x), quality={}, is_hdr={}",
             inputFrames, outputFrames, config.fgMultiplier,
             static_cast<int>(dlssCfg.quality), dlssCfg.is_hdr);

    m_dispatcher->progress().current_frame.store(0);
    m_dispatcher->progress().total_frames.store(outputFrames);
    m_dispatcher->progress().percent.store(0.0f);
    m_dispatcher->progress().state.store(1);

    try {
        TexturePool pool(m_ctx);
        DlssSrPass srPass(m_ngx, m_ctx, &pool);       // Stage 2
        DlssFgPass fgPass(m_ngx, m_ctx, &pool);       // Stage 5
        DepthConversionPass depthPass(m_ctx);         // Stage 1 sub-pass
        MvConversionPass mvPass(m_ctx);               // Stage 1 sub-pass
        TonemapMode tonemapMode = dlssCfg.is_hdr ? TonemapMode::HDR : TonemapMode::LDR;
        TonemapPass tonemapPass(m_ctx);               // Stage 4
        InverseTonemapPass inversePass(m_ctx);         // Stage 6
        AsyncFrameWriter writer;                       // Stage 7

        const uint32_t rW = dlssCfg.render_resolution.width;
        const uint32_t rH = dlssCfg.render_resolution.height;
        const uint32_t oW = dlssCfg.output_resolution.width;
        const uint32_t oH = dlssCfg.output_resolution.height;

        srPass.create(rW, rH, oW, oH, dlssCfg.quality);
        fgPass.create(oW, oH, oW, oH, config.fgMultiplier);

        FramePrefetcher prefetcher(buildFramePaths(dlssCfg, inferInputDir(config)), kPrefetchDepth);

        const VkImageUsageFlags sampledUpload =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        TextureDesc sceneColorRenderDesc{rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc sceneColorOutputDesc{
            oW,
            oH,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc depthWorldDesc{rW, rH, VK_FORMAT_R32_SFLOAT, sampledUpload};
        TextureDesc depthNdcDesc{
            rW,
            rH,
            VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc mvBlenderDesc{rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc mvDlssDesc{
            rW,
            rH,
            VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc displayDesc{oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT};
        TextureDesc interpDesc{oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc sceneLinearDesc{oW, oH, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};

        VkCommandPool cmdPool = m_ctx->createComputeCommandPool();
        const float frameTimeMs = 1000.0f / static_cast<float>(dlssCfg.fps);  // DLSS SR §3.14 — from fps, NEVER hardcoded
        int outputFrameIdx = 0;

        auto writeWithInverse = [&](PooledTextureHandle srcDisplay) {
            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
            auto sceneLinear = pool.acquire(sceneLinearDesc);
            transitionStorageImageForWrite(cmd, sceneLinear);
            inversePass.execute(cmd, srcDisplay, sceneLinear, tonemapMode);
            transitionStorageToShaderRead(cmd, sceneLinear);
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            ++outputFrameIdx;
            WriteJob job;
            job.format        = config.outputFormat;
            job.outputPath    = std::filesystem::path(config.outputDir) /
                                AsyncFrameWriter::formatPath(config.filenameTemplate, outputFrameIdx);
            job.width         = oW;
            job.height        = oH;
            job.fps           = dlssCfg.fps;
            job.is_hdr        = dlssCfg.is_hdr;
            job.overwriteMode = config.overwriteMode;
            job.previewCallback = config.previewCallback;
            job.pixels        = m_ctx->downloadImageRgba16f(sceneLinear->image, oW, oH);
            writer.enqueue(std::move(job));

            pool.release(sceneLinear);
            m_dispatcher->progress().current_frame.store(outputFrameIdx);
            m_dispatcher->progress().percent.store(
                static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
        };

        int processedInputs = 0;
        for (int frameIdx = 0; frameIdx < inputFrames && !m_stop.load(); ++frameIdx) {
            // === STAGE 1: Prefetch + Channel Map + Upload + Depth/MV Convert ===
            const auto frameResult = prefetcher.getNext();
            if (!frameResult.has_value()) {
                LOG_WARN("[SR+FG] Frame {} prefetch error: {}", frameIdx + 1, frameResult.error().message);
                continue;
            }
            const DecodedFrame& decodedFrame = frameResult.value();

            const auto mapResult = ChannelMapper::map(decodedFrame);
            if (!mapResult.has_value()) {
                LOG_WARN("[SR+FG] Frame {} channel map error: {}", frameIdx + 1, mapResult.error().message);
                continue;
            }
            const MappedBuffers& mapped = mapResult.value();

            auto sceneColorRender = pool.acquire(sceneColorRenderDesc);
            auto sceneColorOutput = pool.acquire(sceneColorOutputDesc);
            auto depthWorld       = pool.acquire(depthWorldDesc);
            // CRITICAL INVARIANT: depthNdc and mvDlss are acquired ONCE and are passed to
            // BOTH Stage 2 (SR) and Stage 5 (FG) — same PooledTextureHandle objects.
            auto depthNdc         = pool.acquire(depthNdcDesc);  // Shared depth — FG §6 L545
            auto mvBlender        = pool.acquire(mvBlenderDesc);
            auto mvDlss           = pool.acquire(mvDlssDesc);    // Shared MV — FG §6 L555
            auto displayTex       = pool.acquire(displayDesc);

            UploadBatch batch(m_ctx);
            batch.addUpload(sceneColorRender, mapped.sceneColor.data(), mapped.sceneColor.size() * sizeof(std::uint16_t));
            batch.addUpload(depthWorld, mapped.depth.data(), mapped.depth.size() * sizeof(float));
            batch.addUpload(mvBlender, mapped.motionVector.data(), mapped.motionVector.size() * sizeof(std::uint16_t));
            batch.submit();

            std::vector<PooledTextureHandle> interpHandles;
            for (int j = 0; j < config.fgMultiplier - 1; ++j) {
                interpHandles.push_back(pool.acquire(interpDesc));
            }

            const std::string& frameKey = frameKeys[static_cast<std::size_t>(frameIdx)].second;
            const CameraData* cam = findCamera(dlssCfg, frameKey);
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
            transitionForSrEvaluate(cmd, sceneColorOutput, depthNdc, mvDlss);

            // === STAGE 2: DLSS-SR Eval ===
            // SR MUST run BEFORE tonemap (DLSS SR §3.1 — input must be scene-linear).
            const bool reset = shouldReset(frameIdx, decodedFrame);
            SrInputs srIn{};
            srIn.sceneColorRenderRes = sceneColorRender->view;
            srIn.sceneColorOutputRes = sceneColorOutput->view;
            srIn.depthNdc            = depthNdc->view;  // SAME handle used by FG Stage 5.
            srIn.motionVecDlss       = mvDlss->view;   // SAME handle used by FG Stage 5.
            srIn.renderW             = rW;
            srIn.renderH             = rH;
            srIn.outputW             = oW;
            srIn.outputH             = oH;
            srIn.jitterX             = cam ? cam->jitter.x : 0.0f;
            srIn.jitterY             = cam ? cam->jitter.y : 0.0f;
            srIn.reset               = reset;
            srIn.frameTimeDeltaMs    = frameTimeMs;
            srPass.evaluate(cmd, srIn);

            // === STAGE 4: Tonemap + EOTF ===
            // SR output (scene-linear) -> display-ready. Tonemap AFTER SR, BEFORE FG.
            transitionStorageToShaderRead(cmd, sceneColorOutput);
            transitionStorageImageForWrite(cmd, displayTex);
            tonemapPass.execute(cmd, sceneColorOutput, displayTex, tonemapMode);

            // === STAGE 5: DLSS-FG Eval ===
            // FG stores previous pBackbuffer internally. Evaluate every rendered frame;
            // write generated frames only after a valid previous frame exists, before
            // writing the current real frame: A, interp(A→B), B.
            transitionForFgEvaluate(cmd, displayTex, interpHandles);

            FgInputs fgIn{};
            fgIn.backbufferDisplayReady = displayTex->view;
            fgIn.depthNdcRenderRes      = depthNdc->view;  // SAME PooledTextureHandle as SR.
            fgIn.motionVecDlssRenderRes = mvDlss->view;   // SAME PooledTextureHandle as SR.
            fgIn.backbufferImage        = displayTex->image;
            fgIn.depthImage             = depthNdc->image;
            fgIn.motionVecImage         = mvDlss->image;
            fgIn.outputInterp.reserve(interpHandles.size());
            fgIn.outputInterpImages.reserve(interpHandles.size());
            for (const auto& handle : interpHandles) {
                fgIn.outputInterp.push_back(handle->view);
                fgIn.outputInterpImages.push_back(handle->image);
            }
            fgIn.backbufferFormat = displayTex->desc.format;
            fgIn.depthFormat      = depthNdc->desc.format;
            fgIn.motionVecFormat  = mvDlss->desc.format;
            fgIn.width            = oW;
            fgIn.height           = oH;
            fgIn.depthMotionWidth = rW;
            fgIn.depthMotionHeight = rH;
            fgIn.is_hdr           = dlssCfg.is_hdr;
            fgIn.reset            = reset;
            fgIn.multiplier       = config.fgMultiplier;
            fillFgCameraInputs(fgIn, cam);

            fgPass.evaluate(cmd, fgIn);
            transitionFgOutputsForInverse(cmd, interpHandles);
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            // === STAGE 6: Inverse Tonemap + STAGE 7: Write ===
            const bool writeGeneratedFrames = frameIdx > 0 && !reset;
            if (writeGeneratedFrames) {
                for (const auto& handle : interpHandles) {
                    writeWithInverse(handle);
                }
            }
            writeWithInverse(displayTex);
            ++processedInputs;
            if (!writeGeneratedFrames && frameIdx > 0) {
                LOG_WARN("[SR+FG] Skipped generated frames at reset/gap input {}", frameIdx + 1);
            }

            pool.release(sceneColorRender);
            pool.release(sceneColorOutput);
            pool.release(depthWorld);
            pool.release(depthNdc);
            pool.release(mvBlender);
            pool.release(mvDlss);
            pool.release(displayTex);
            for (auto& handle : interpHandles) {
                pool.release(handle);
            }

            LOG_INFO("[SR+FG] Input {}/{}, total output {}/{}", frameIdx + 1, inputFrames,
                     outputFrameIdx, outputFrames);
        }

        if (processedInputs != inputFrames) {
            throw std::runtime_error("SrFgPipeline: processed " + std::to_string(processedInputs) +
                                     " of " + std::to_string(inputFrames) + " input frames");
        }
        writer.flush();
        vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);

        m_dispatcher->progress().state.store(2);
        LOG_INFO("[SR+FG] Complete: {} total output frames", outputFrameIdx);
    } catch (...) {
        m_dispatcher->progress().state.store(3);
        throw;
    }
}

} // namespace dlss
