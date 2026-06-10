#include "sr_only_pipeline.h"

#include "../core/log.h"
#include "../gpu/passes/preview_pass.h"

#include <volk.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dlss {

namespace {

constexpr int kPrefetchDepth = 3;

float matrixDisplacement(const float* m1, const float* m2) {
    if (!m1 || !m2) return 0.0f;

    float sum = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const float d = m1[i] - m2[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

std::filesystem::path inferInputDir(const SrOnlyConfig& config) {
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
            LOG_WARN("[SR-only] Ignoring non-numeric frame key: {}", key);
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

uint16_t f32ToF16(float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    const uint32_t sign = (bits >> 31) & 0x1u;
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    const uint32_t mantissa = bits & 0x7FFFFFu;
    if (exponent == 255) return static_cast<uint16_t>((sign << 15) | 0x7C00u | (mantissa ? 0x200u : 0u));
    if (exponent == 0) return static_cast<uint16_t>(sign << 15);
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>((sign << 15) | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign << 15);
        const uint32_t m = (mantissa | 0x800000u) >> (1 - e);
        return static_cast<uint16_t>((sign << 15) | (m >> 13));
    }
    return static_cast<uint16_t>((sign << 15) | (static_cast<uint32_t>(e) << 10) | (mantissa >> 13));
}

std::vector<float> makeNdcDepth(const DecodedFrame& frame, const float projection[4][4]) {
    std::vector<float> out(frame.depth.size(), 0.0f);
    for (std::size_t i = 0; i < frame.depth.size(); ++i) {
        const float viewZ = -frame.depth[i];
        const float clipZ = projection[2][2] * viewZ + projection[3][2];
        const float clipW = projection[2][3] * viewZ + projection[3][3];
        float ndc = 0.0f;
        if (std::abs(clipW) > 1e-6f) {
            ndc = (clipZ / clipW) * 0.5f + 0.5f;
        }
        out[i] = std::clamp(ndc, 0.0f, 1.0f);
    }
    return out;
}

std::vector<uint16_t> makeDlssMv(const DecodedFrame& frame) {
    const std::size_t pixels = static_cast<std::size_t>(frame.width) * frame.height;
    std::vector<uint16_t> out(pixels * 2, 0);
    for (std::size_t i = 0; i < pixels; ++i) {
        const float x = frame.vector.size() >= pixels * 4 ? frame.vector[i * 4] : 0.0f;
        const float y = frame.vector.size() >= pixels * 4 ? -frame.vector[i * 4 + 1] : 0.0f;
        out[i * 2] = f32ToF16(x);
        out[i * 2 + 1] = f32ToF16(y);
    }
    return out;
}

void requirePipelinePointers(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher) {
    if (!ctx) throw std::invalid_argument("SrOnlyPipeline: VulkanContext must not be null");
    if (!ngx) throw std::invalid_argument("SrOnlyPipeline: NgxContext must not be null");
    if (!dispatcher) throw std::invalid_argument("SrOnlyPipeline: RpcDispatcher must not be null");
}

void transitionForSrEvaluate(VkCommandBuffer cmd,
                             const PooledTextureHandle& colorOutput,
                             const PooledTextureHandle& depthNdc,
                             const PooledTextureHandle& mvDlss) {
    VkImageMemoryBarrier2 barriers[3]{};

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

    barriers[2].sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[2].srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[2].srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barriers[2].dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barriers[2].dstAccessMask       = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[2].oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barriers[2].newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[2].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[2].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[2].image               = mvDlss->image;
    barriers[2].subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 3;
    dependency.pImageMemoryBarriers    = barriers;
    vkCmdPipelineBarrier2(cmd, &dependency);
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

} // namespace

SrOnlyPipeline::SrOnlyPipeline(VulkanContext* ctx, NgxContext* ngx, RpcDispatcher* dispatcher)
    : m_ctx(ctx), m_ngx(ngx), m_dispatcher(dispatcher) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
}

SrOnlyPipeline::~SrOnlyPipeline() = default;

bool SrOnlyPipeline::shouldReset(int frameIdx,
                                 const DecodedFrame& frame,
                                 const float* prevMatrix,
                                 const float* currMatrix) const {
    if (frameIdx == 0) return true;
    if (frame.has_gap_after) return true;

    // Camera-cut sentinel for T36: calculate discontinuity path but keep the
    // threshold at infinity until T38 calibrates a production value.
    const float cameraCutThreshold = INFINITY;
    return matrixDisplacement(prevMatrix, currMatrix) > cameraCutThreshold;
}

void SrOnlyPipeline::run(const SrOnlyConfig& config) {
    requirePipelinePointers(m_ctx, m_ngx, m_dispatcher);
    m_stop.store(false);

    const DlssConfig& dlssCfg = config.dlssConfig;
    const auto frameKeys = sortedFrameKeys(dlssCfg);
    const int totalFrames = static_cast<int>(frameKeys.size());
    if (totalFrames == 0) {
        throw std::runtime_error("SrOnlyPipeline: dlssConfig has no frames");
    }
    if (dlssCfg.fps <= 0) {
        throw std::runtime_error("SrOnlyPipeline: fps must be positive");
    }
    if (dlssCfg.filename_pattern.empty()) {
        throw std::runtime_error("SrOnlyPipeline: filename_pattern must not be empty");
    }

    LOG_INFO("[SR-only] Starting pipeline: {} frames, quality={}",
             totalFrames, static_cast<int>(dlssCfg.quality));

    m_dispatcher->progress().current_frame.store(0);
    m_dispatcher->progress().total_frames.store(totalFrames);
    m_dispatcher->progress().percent.store(0.0f);
    m_dispatcher->progress().dlss_current_frame.store(0);
    m_dispatcher->progress().dlss_total_frames.store(totalFrames);
    m_dispatcher->progress().dlss_percent.store(0.0f);
    m_dispatcher->progress().state.store(1);

    try {
        TexturePool pool(m_ctx);
        DlssSrPass srPass(m_ngx, m_ctx, &pool);
        PreviewPass previewPass(m_ctx);
        AsyncFrameWriter writer;

        srPass.create(dlssCfg.render_resolution.width,
                      dlssCfg.render_resolution.height,
                      dlssCfg.output_resolution.width,
                      dlssCfg.output_resolution.height,
                      dlssCfg.quality);

        FramePrefetcher prefetcher(buildFramePaths(dlssCfg, inferInputDir(config)), kPrefetchDepth);

        const VkImageUsageFlags sampledUpload =
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        TextureDesc colorRenderDesc{
            dlssCfg.render_resolution.width,
            dlssCfg.render_resolution.height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            sampledUpload};
        TextureDesc colorOutputDesc{
            dlssCfg.output_resolution.width,
            dlssCfg.output_resolution.height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc depthWorldDesc{
            dlssCfg.render_resolution.width,
            dlssCfg.render_resolution.height,
            VK_FORMAT_R32_SFLOAT,
            sampledUpload};
        TextureDesc depthNdcDesc{
            dlssCfg.render_resolution.width,
            dlssCfg.render_resolution.height,
            VK_FORMAT_R32_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
        TextureDesc mvBlenderDesc{
            dlssCfg.render_resolution.width,
            dlssCfg.render_resolution.height,
            VK_FORMAT_R16G16B16A16_SFLOAT,
            sampledUpload};
        TextureDesc mvDlssDesc{
            dlssCfg.render_resolution.width,
            dlssCfg.render_resolution.height,
            VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};

        const float frameTimeMs = 1000.0f / static_cast<float>(dlssCfg.fps);
        VkCommandPool cmdPool = m_ctx->createComputeCommandPool();

        int processedFrames = 0;
        std::atomic<int> writtenFrames{0};
        for (int frameIdx = 0; frameIdx < totalFrames && !m_stop.load(); ++frameIdx) {
            const auto frameResult = prefetcher.getNext();
            if (!frameResult.has_value()) {
                LOG_WARN("[SR-only] Frame {} read error: {}",
                         frameIdx + 1, frameResult.error().message);
                continue;
            }
            const DecodedFrame& decodedFrame = frameResult.value();

            const auto mapResult = ChannelMapper::map(decodedFrame);
            if (!mapResult.has_value()) {
                LOG_WARN("[SR-only] Frame {} map error: {}", frameIdx + 1, mapResult.error().message);
                continue;
            }
            const MappedBuffers& mapped = mapResult.value();

            auto colorRender = pool.acquire(colorRenderDesc);
            auto colorOutput = pool.acquire(colorOutputDesc);
            auto depthWorld  = pool.acquire(depthWorldDesc);
            auto depthNdc    = pool.acquire(depthNdcDesc);
            auto mvBlender   = pool.acquire(mvBlenderDesc);
            auto mvDlss      = pool.acquire(mvDlssDesc);

            const std::string& frameKey = frameKeys[static_cast<std::size_t>(frameIdx)].second;
            const CameraData* cam = findCamera(dlssCfg, frameKey);
            const CameraData* prevCam = frameIdx > 0
                ? findCamera(dlssCfg, frameKeys[static_cast<std::size_t>(frameIdx - 1)].second)
                : nullptr;

            float defaultProjection[4][4];
            identity(defaultProjection);
            const float (*projection)[4] = cam ? cam->projection : defaultProjection;
            const float* prevMatrix = prevCam ? &prevCam->matrix_world[0][0] : nullptr;
            const float* currMatrix = cam ? &cam->matrix_world[0][0] : nullptr;

            const auto ndcDepth = makeNdcDepth(decodedFrame, projection);
            const auto dlssMv = makeDlssMv(decodedFrame);

            UploadBatch batch(m_ctx);
            batch.addUpload(colorRender, mapped.sceneColor.data(), mapped.sceneColor.size() * sizeof(std::uint16_t));
            batch.addUpload(depthNdc, ndcDepth.data(), ndcDepth.size() * sizeof(float));
            batch.addUpload(mvDlss, dlssMv.data(), dlssMv.size() * sizeof(std::uint16_t));
            batch.submit();

            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
            transitionForSrEvaluate(cmd, colorOutput, depthNdc, mvDlss);

            SrInputs srIn{};
            srIn.sceneColorRenderRes = colorRender->view;
            srIn.sceneColorOutputRes = colorOutput->view;
            srIn.depthNdc            = depthNdc->view;
            srIn.motionVecDlss       = mvDlss->view;
            srIn.renderW             = dlssCfg.render_resolution.width;
            srIn.renderH             = dlssCfg.render_resolution.height;
            srIn.outputW             = dlssCfg.output_resolution.width;
            srIn.outputH             = dlssCfg.output_resolution.height;
            srIn.jitterX             = cam ? cam->jitter.x : 0.0f;
            srIn.jitterY             = cam ? cam->jitter.y : 0.0f;
            srIn.reset               = shouldReset(frameIdx, decodedFrame, prevMatrix, currMatrix);
            srIn.frameTimeDeltaMs    = frameTimeMs;

            srPass.evaluate(cmd, srIn);
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            if (config.previewRgba8Callback && (!config.wantsPreviewFrame || config.wantsPreviewFrame())) {
                PreviewReadback preview = previewPass.renderAndReadback(colorOutput, PreviewInputMode::SceneLinear, dlssCfg.is_hdr);
                if (!preview.rgba8.empty()) {
                    config.previewRgba8Callback(preview.rgba8, preview.width, preview.height);
                }
            }

            WriteJob writeJob;
            writeJob.format        = config.outputFormat;
            writeJob.outputPath    = std::filesystem::path(config.outputDir) /
                                     AsyncFrameWriter::formatPath(config.filenameTemplate, frameIdx + 1);
            writeJob.width         = dlssCfg.output_resolution.width;
            writeJob.height        = dlssCfg.output_resolution.height;
            writeJob.fps           = dlssCfg.fps;
            writeJob.is_hdr        = dlssCfg.is_hdr;
            writeJob.overwriteMode = config.overwriteMode;
            writeJob.pixels        = m_ctx->downloadImageRgba16f(colorOutput->image, writeJob.width, writeJob.height);
            writeJob.completionCallback = [this, totalFrames, &writtenFrames]() {
                const int written = writtenFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
                m_dispatcher->progress().current_frame.store(written);
                m_dispatcher->progress().percent.store(
                    static_cast<float>(written) / static_cast<float>(totalFrames));
            };
            writer.enqueue(std::move(writeJob));
            ++processedFrames;

            pool.release(colorRender);
            pool.release(colorOutput);
            pool.release(depthWorld);
            pool.release(depthNdc);
            pool.release(mvBlender);
            pool.release(mvDlss);

            m_dispatcher->progress().dlss_current_frame.store(frameIdx + 1);
            m_dispatcher->progress().dlss_percent.store(
                static_cast<float>(frameIdx + 1) / static_cast<float>(totalFrames));
            LOG_INFO("[SR-only] Frame {}/{} done", frameIdx + 1, totalFrames);
        }

        if (processedFrames != totalFrames) {
            throw std::runtime_error("SrOnlyPipeline: processed " + std::to_string(processedFrames) +
                                     " of " + std::to_string(totalFrames) + " input frames");
        }
        writer.flush();
        vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);

        m_dispatcher->progress().state.store(2);
        LOG_INFO("[SR-only] Pipeline complete: {} frames", totalFrames);
    } catch (...) {
        m_dispatcher->progress().state.store(3);
        throw;
    }
}

} // namespace dlss
