#include "sr_fg_pipeline.h"

#include "../core/log.h"
#include "../gpu/passes/prores_convert_pass.h"
#include "../gpu/passes/preview_pass.h"
#include "../io/prores_writer.h"
#include "fg_camera.h"

#include <volk.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfPixelType.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
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

void requireMappedDimensions(const MappedBuffers& mapped,
                             uint32_t expectedW,
                             uint32_t expectedH) {
    if (mapped.width != expectedW || mapped.height != expectedH) {
        throw std::runtime_error(
            "SrFgPipeline: EXR dimensions " + std::to_string(mapped.width) + "x" +
            std::to_string(mapped.height) + " do not match dlssconfig render_resolution " +
            std::to_string(expectedW) + "x" + std::to_string(expectedH));
    }
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
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(1 + interpOutputs.size());

    auto addBarrier = [&](const PooledTextureHandle& texture,
                          VkImageLayout oldLayout,
                          VkImageLayout newLayout,
                          VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess,
                          VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2& barrier = barriers.emplace_back();
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
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionFgOutputsToTransferSrc(VkCommandBuffer cmd,
                                      const std::vector<PooledTextureHandle>& interpOutputs) {
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(interpOutputs.size());
    for (const auto& interp : interpOutputs) {
        VkImageMemoryBarrier2& barrier = barriers.emplace_back();
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = interp->image;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    }

    if (barriers.empty()) return;
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionDisplayAfterFg(VkCommandBuffer cmd,
                              const PooledTextureHandle& display,
                              bool shaderRead) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask       = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask        = shaderRead
        ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        : VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask       = shaderRead
        ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
        : VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout           = shaderRead
        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        : VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = display->image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers    = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void transitionFgOutputsForInverse(VkCommandBuffer cmd,
                                   const std::vector<PooledTextureHandle>& interpOutputs) {
    std::vector<VkImageMemoryBarrier2> barriers;
    barriers.reserve(interpOutputs.size());
    for (const auto& interp : interpOutputs) {
        VkImageMemoryBarrier2& barrier = barriers.emplace_back();
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

    if (barriers.empty()) return;
    VkDependencyInfo dependency{};
    dependency.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers    = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);
}

bool shouldDumpGuideBuffers() {
    const char* value = std::getenv("DLSS_COMPOSITOR_DUMP_GUIDES");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open guide dump: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint8_t clampByte(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 255.0f));
}

float halfToFloat(std::uint16_t bits) {
    const std::uint32_t sign = static_cast<std::uint32_t>(bits & 0x8000u) << 16;
    std::uint32_t exp = (bits >> 10) & 0x1fu;
    std::uint32_t mant = bits & 0x03ffu;

    std::uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 127u - 15u + 1u;
            while ((mant & 0x0400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x03ffu;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        out = sign | 0x7f800000u | (mant << 13);
    } else {
        exp = exp + (127u - 15u);
        out = sign | (exp << 23) | (mant << 13);
    }

    float value = 0.0f;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

void writePgm16(const std::filesystem::path& path,
                const std::vector<std::uint16_t>& pixels,
                uint32_t width,
                uint32_t height) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open guide image: " + path.string());
    }
    out << "P5\n" << width << " " << height << "\n65535\n";
    for (std::uint16_t value : pixels) {
        const char bytes[2] = {
            static_cast<char>((value >> 8) & 0xffu),
            static_cast<char>(value & 0xffu),
        };
        out.write(bytes, sizeof(bytes));
    }
}

void writePpm8(const std::filesystem::path& path,
               const std::vector<std::uint8_t>& rgb,
               uint32_t width,
               uint32_t height) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open guide image: " + path.string());
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
}

void writeFloatExr(const std::filesystem::path& path,
                   uint32_t width,
                   uint32_t height,
                   const std::vector<std::pair<std::string, std::vector<float>>>& channels) {
    if (channels.empty()) {
        throw std::runtime_error("writeFloatExr: no channels for " + path.string());
    }
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    for (const auto& [name, pixels] : channels) {
        if (pixels.size() != pixelCount) {
            throw std::runtime_error("writeFloatExr: channel size mismatch for " + name);
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    Imf::Header header(static_cast<int>(width), static_cast<int>(height));
    for (const auto& [name, pixels] : channels) {
        (void)pixels;
        header.channels().insert(name.c_str(), Imf::Channel(Imf::FLOAT));
    }

    Imf::FrameBuffer fb;
    const std::size_t xStride = sizeof(float);
    const std::size_t yStride = xStride * static_cast<std::size_t>(width);
    for (const auto& [name, pixels] : channels) {
        fb.insert(name.c_str(),
                  Imf::Slice(Imf::FLOAT,
                             const_cast<char*>(reinterpret_cast<const char*>(pixels.data())),
                             xStride,
                             yStride));
    }

    Imf::OutputFile file(path.string().c_str(), header);
    file.setFrameBuffer(fb);
    file.writePixels(static_cast<int>(height));
}

void dumpGuideBuffers(VulkanContext* ctx,
                      const std::filesystem::path& outputDir,
                      int frameNumber,
                      uint32_t width,
                      uint32_t height,
                      const PooledTextureHandle& depthNdc,
                      const PooledTextureHandle& mvDlss) {
    const auto guideDir = outputDir / "guide_buffers";
    const auto stem = AsyncFrameWriter::formatPath("frame_####", frameNumber);
    const auto depth = ctx->downloadImageRaw(depthNdc->image, width, height, sizeof(float), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const auto mv = ctx->downloadImageRaw(mvDlss->image, width, height, 2u * sizeof(std::uint16_t), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    writeBytes(guideDir / (stem + "_depth_r32f.raw"), depth);
    writeBytes(guideDir / (stem + "_mv_rg16f.raw"), mv);

    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    std::vector<float> depthFloat(pixelCount);
    const float* depthFloats = reinterpret_cast<const float*>(depth.data());
    std::copy(depthFloats, depthFloats + pixelCount, depthFloat.begin());
    writeFloatExr(guideDir / (stem + "_depth_float.exr"),
                  width,
                  height,
                  {{"R", depthFloat}, {"G", depthFloat}, {"B", depthFloat}, {"Z", depthFloat}});

    std::vector<std::uint16_t> depthPreview(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < depthPreview.size(); ++i) {
        depthPreview[i] = static_cast<std::uint16_t>(std::clamp(depthFloats[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
    }
    writePgm16(guideDir / (stem + "_depth_u16.pgm"), depthPreview, width, height);

    std::vector<std::uint16_t> mvMag(static_cast<std::size_t>(width) * height);
    std::vector<std::uint8_t> mvXy(static_cast<std::size_t>(width) * height * 3u);
    std::vector<float> mvX(pixelCount);
    std::vector<float> mvY(pixelCount);
    std::vector<float> mvMagnitude(pixelCount);
    const std::uint16_t* mvHalfs = reinterpret_cast<const std::uint16_t*>(mv.data());
    for (std::size_t i = 0; i < mvMag.size(); ++i) {
        const float x = halfToFloat(mvHalfs[i * 2u + 0u]);
        const float y = halfToFloat(mvHalfs[i * 2u + 1u]);
        const float mag = std::sqrt(x * x + y * y);
        mvX[i] = x;
        mvY[i] = y;
        mvMagnitude[i] = mag;
        mvMag[i] = static_cast<std::uint16_t>(std::clamp(mag / 128.0f, 0.0f, 1.0f) * 65535.0f + 0.5f);
        mvXy[i * 3u + 0u] = clampByte(128.0f + x * 2.0f);
        mvXy[i * 3u + 1u] = clampByte(128.0f + y * 2.0f);
        mvXy[i * 3u + 2u] = clampByte(std::min(mag, 127.0f) * 2.0f);
    }
    writeFloatExr(guideDir / (stem + "_mv_float.exr"),
                  width,
                  height,
                  {{"R", mvX}, {"G", mvY}, {"B", mvMagnitude}, {"X", mvX}, {"Y", mvY}, {"Magnitude", mvMagnitude}});
    writePgm16(guideDir / (stem + "_mv_mag_u16.pgm"), mvMag, width, height);
    writePpm8(guideDir / (stem + "_mv_xy.ppm"), mvXy, width, height);
    LOG_INFO("[SR+FG] Dumped guide buffers for frame {}: {}x{} to {}", frameNumber, width, height, guideDir.string());
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
    if (config.fgMultiplier < 2 || config.fgMultiplier > 6) {
        throw std::runtime_error("SrFgPipeline: fgMultiplier must be between 2 and 6");
    }
    // Output frame count: SR outputs N frames; FG generates (mult-1) interp per input pair.
    const int outputFrames = config.fgMultiplier * inputFrames - (config.fgMultiplier - 1);

    LOG_INFO("[SR+FG] Starting: {} input -> {} output ({}x), quality={}, is_hdr={}",
             inputFrames, outputFrames, config.fgMultiplier,
             static_cast<int>(dlssCfg.quality), dlssCfg.is_hdr);

    m_dispatcher->progress().current_frame.store(0);
    m_dispatcher->progress().total_frames.store(outputFrames);
    m_dispatcher->progress().percent.store(0.0f);
    m_dispatcher->progress().dlss_current_frame.store(0);
    m_dispatcher->progress().dlss_total_frames.store(outputFrames);
    m_dispatcher->progress().dlss_percent.store(0.0f);
    m_dispatcher->progress().state.store(1);

    try {
        TexturePool pool(m_ctx);
        DlssSrPass srPass(m_ngx, m_ctx, &pool);       // Stage 2
        DlssFgPass fgPass(m_ngx, m_ctx, &pool);       // Stage 5
        DepthConversionPass depthPass(m_ctx);         // Stage 1 sub-pass
        MvConversionPass mvPass(m_ctx);               // Stage 1 sub-pass
        TonemapMode tonemapMode = dlssCfg.is_hdr ? TonemapMode::HDR : TonemapMode::LDR;
        TonemapPass tonemapPass(m_ctx);               // Stage 4
        InverseTonemapPass inversePass(m_ctx);         // Stage 6 for scene-linear EXR
        std::unique_ptr<LutTonemapPass> lutTonemapPass;
        if (!dlssCfg.tonemap_lut.empty()) {
            auto lut = LutLoader::LoadCube(dlssCfg.tonemap_lut);
            if (!lut.has_value()) {
                throw std::runtime_error("SrFgPipeline: failed to load tonemap_lut: " + lut.error().message);
            }
            LOG_INFO("[SR+FG] Using tonemap LUT: {} size={} domain=[{}, {}]",
                     dlssCfg.tonemap_lut,
                     lut->size,
                     lut->domain_min[0],
                     lut->domain_max[0]);
            lutTonemapPass = std::make_unique<LutTonemapPass>(m_ctx, lut.value());
        } else {
            LOG_INFO("[SR+FG] No tonemap LUT configured; using built-in tonemap shader");
        }
        PreviewPass previewPass(m_ctx);
        const int writerThreads = std::clamp(config.writerThreads, 1, 32);
        const int writerQueueMultiplier = std::clamp(config.writerQueueMultiplier, 1, 64);
        const int writerQueueLow = writerThreads;
        const int writerQueueHigh = writerThreads * writerQueueMultiplier;
        AsyncFrameWriter writer(writerThreads, writerQueueHigh, writerQueueLow); // Stage 7
        std::unique_ptr<ProResProgressWriter> proresWriter;
        std::unique_ptr<ProresConvertPass> proresConvertPass;
        std::unique_ptr<LutInversePass> lutInversePass;
        if (!dlssCfg.inverse_lut.empty()) {
            auto inverseLut = LutLoader::LoadCube(dlssCfg.inverse_lut);
            if (!inverseLut.has_value()) {
                throw std::runtime_error("SrFgPipeline: failed to load inverse_lut: " + inverseLut.error().message);
            }
            LOG_INFO("[SR+FG] Using inverse LUT: {} size={} domain=[{}, {}]",
                     dlssCfg.inverse_lut,
                     inverseLut->size,
                     inverseLut->domain_min[0],
                     inverseLut->domain_max[0]);
            lutInversePass = std::make_unique<LutInversePass>(m_ctx, inverseLut.value());
        } else {
            LOG_INFO("[SR+FG] No inverse LUT configured; using built-in inverse shader for EXR output");
        }

        const uint32_t rW = dlssCfg.render_resolution.width;
        const uint32_t rH = dlssCfg.render_resolution.height;
        const uint32_t oW = dlssCfg.output_resolution.width;
        const uint32_t oH = dlssCfg.output_resolution.height;
        if (config.outputFormat == OutputFormat::ProRes422HQ || config.outputFormat == OutputFormat::H265Mp4) {
            proresWriter = std::make_unique<ProResProgressWriter>(
                std::filesystem::path(config.outputDir) / config.filenameTemplate,
                oW,
                oH,
                dlssCfg.fps * config.fgMultiplier,
                dlssCfg.is_hdr,
                PixelEncoding::DisplayReady,
                inferHdrMasteringNits(dlssCfg.view_transform),
                writerQueueHigh,
                writerQueueLow,
                writerThreads,
                outputFrames,
                [this](int current, int total) {
                    m_dispatcher->progress().current_frame.store(current);
                    m_dispatcher->progress().percent.store(
                        total > 0 ? static_cast<float>(current) / static_cast<float>(total) : 0.0f);
                },
                config.outputFormat == OutputFormat::H265Mp4 ? VideoCodec::H265Hevc : VideoCodec::ProRes422HQ,
                config.h265Options);
            proresConvertPass = std::make_unique<ProresConvertPass>(m_ctx);
        }

        srPass.create(rW, rH, oW, oH, dlssCfg.quality, config.presetOverride);
        fgPass.create(rW, rH, oW, oH, config.fgMultiplier);

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
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        TextureDesc mvBlenderDesc{rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc mvDlssDesc{
            rW,
            rH,
            VK_FORMAT_R16G16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        TextureDesc displayDesc{oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        TextureDesc interpDesc{oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        TextureDesc sceneLinearDesc{oW, oH, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};

        VkCommandPool cmdPool = m_ctx->createComputeCommandPool();
        const float frameTimeMs = 1000.0f / static_cast<float>(dlssCfg.fps);  // DLSS SR §3.14 — from fps, NEVER hardcoded
        int outputFrameIdx = 0;
        std::atomic<int> writtenFrames{0};
        const bool dumpGuides = shouldDumpGuideBuffers();

        auto enqueuePixels = [&](std::vector<std::uint8_t> pixels, PixelEncoding pixelEncoding) {
            ++outputFrameIdx;
            if (proresWriter) {
                proresWriter->enqueueFrame(std::move(pixels));
                m_dispatcher->progress().dlss_current_frame.store(outputFrameIdx);
                m_dispatcher->progress().dlss_percent.store(
                    static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
                return;
            }

            WriteJob job;
            job.format        = config.outputFormat;
            job.outputPath    = std::filesystem::path(config.outputDir) /
                                AsyncFrameWriter::formatPath(config.filenameTemplate, outputFrameIdx);
            job.width         = oW;
            job.height        = oH;
            job.fps           = dlssCfg.fps;
            job.is_hdr        = dlssCfg.is_hdr;
            job.pixelEncoding = pixelEncoding;
            job.encoderOptions = config.encoderOptions;
            job.overwriteMode = config.overwriteMode;
            job.pixels        = std::move(pixels);
            job.completionCallback = [this, outputFrames, &writtenFrames]() {
                const int written = writtenFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
                m_dispatcher->progress().current_frame.store(written);
                m_dispatcher->progress().percent.store(
                    static_cast<float>(written) / static_cast<float>(outputFrames));
            };
            writer.enqueue(std::move(job));

            m_dispatcher->progress().dlss_current_frame.store(outputFrameIdx);
            m_dispatcher->progress().dlss_percent.store(
                static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
        };

        auto writeDisplayOrSceneLinear = [&](PooledTextureHandle srcDisplay, VkImageLayout oldLayout) {
            if (config.previewRgba8Callback && (!config.wantsPreviewFrame || config.wantsPreviewFrame())) {
                PreviewReadback preview = previewPass.renderAndReadback(
                    srcDisplay,
                    PreviewInputMode::DisplayReady,
                    dlssCfg.is_hdr,
                    oldLayout);
                if (!preview.rgba8.empty()) {
                    config.previewRgba8Callback(preview.rgba8, preview.width, preview.height);
                }
            }

            if (config.outputFormat == OutputFormat::ExrLinear) {
                VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
                auto sceneLinear = pool.acquire(sceneLinearDesc);
                transitionStorageImageForWrite(cmd, sceneLinear);
                if (lutInversePass) {
                    lutInversePass->execute(cmd, srcDisplay, sceneLinear);
                } else {
                    inversePass.execute(cmd, srcDisplay, sceneLinear, tonemapMode);
                }
                transitionStorageToShaderRead(cmd, sceneLinear);
                m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());
                enqueuePixels(
                    m_ctx->downloadImageRgba16f(
                        sceneLinear->image,
                        oW,
                        oH,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
                    PixelEncoding::SceneLinear);
                pool.release(sceneLinear);
                return;
            }

            if (proresWriter) {
                ++outputFrameIdx;
                proresWriter->enqueuePackedYuv422Frame(
                    proresConvertPass->convertAndReadback(
                        srcDisplay,
                        PixelEncoding::DisplayReady,
                        dlssCfg.is_hdr,
                        oldLayout));
                m_dispatcher->progress().dlss_current_frame.store(outputFrameIdx);
                m_dispatcher->progress().dlss_percent.store(
                    static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames));
                return;
            }

            enqueuePixels(
                m_ctx->downloadImageA2B10ToRgba16f(srcDisplay->image, oW, oH, oldLayout),
                PixelEncoding::DisplayReady);
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
            requireMappedDimensions(mapped, rW, rH);

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
            transitionForSrEvaluate(cmd, sceneColorOutput, depthNdc, mvDlss);

            // === STAGE 2: DLSS-SR Eval ===
            // SR MUST run BEFORE tonemap (DLSS SR §3.1 — input must be scene-linear).
            const bool reset = shouldReset(frameIdx, decodedFrame);
            SrInputs srIn{};
            srIn.sceneColorRenderRes = sceneColorRender->view;
            srIn.sceneColorOutputRes = sceneColorOutput->view;
            srIn.depthNdc            = depthNdc->view;  // SAME handle used by FG Stage 5.
            srIn.motionVecDlss       = mvDlss->view;   // SAME handle used by FG Stage 5.
            srIn.sceneColorRenderImage = sceneColorRender->image;
            srIn.sceneColorOutputImage = sceneColorOutput->image;
            srIn.depthImage            = depthNdc->image;
            srIn.motionVecImage        = mvDlss->image;
            srIn.renderW             = rW;
            srIn.renderH             = rH;
            srIn.outputW             = oW;
            srIn.outputH             = oH;
            // Blender Camera.shift produces a screen-space offset of (-X,+Y)
            // for the nominal jitter stored in dlssconfig. DLSS expects the
            // actual render-pixel offset applied to the input image.
            srIn.jitterX             = cam ? -cam->jitter.x : 0.0f;
            srIn.jitterY             = cam ?  cam->jitter.y : 0.0f;
            srIn.reset               = reset;
            srIn.frameTimeDeltaMs    = frameTimeMs;
            srPass.evaluate(cmd, srIn);

            // === STAGE 4: Tonemap + EOTF ===
            // SR output (scene-linear) -> display-ready. Tonemap AFTER SR, BEFORE FG.
            transitionStorageToShaderRead(cmd, sceneColorOutput);
            transitionStorageImageForWrite(cmd, displayTex);
            if (lutTonemapPass) {
                lutTonemapPass->execute(cmd, sceneColorOutput, displayTex);
            } else {
                tonemapPass.execute(cmd, sceneColorOutput, displayTex, tonemapMode);
            }

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
            fillFgCameraInputs(fgIn, cam, prevCam);
            // DLSS FG expects clip-space jitter while its camera matrices are
            // explicitly unjittered. Convert the actual Blender screen offset
            // (-jitter.x,+jitter.y) to clip space: (+X right,+Y up).
            fgIn.jitterClipX = cam ? -2.0f * cam->jitter.x / static_cast<float>(rW) : 0.0f;
            fgIn.jitterClipY = cam ? -2.0f * cam->jitter.y / static_cast<float>(rH) : 0.0f;

            fgPass.evaluate(cmd, fgIn);
            if (config.outputFormat == OutputFormat::ExrLinear) {
                transitionDisplayAfterFg(cmd, displayTex, true);
                transitionFgOutputsForInverse(cmd, interpHandles);
            } else {
                transitionDisplayAfterFg(cmd, displayTex, false);
                transitionFgOutputsToTransferSrc(cmd, interpHandles);
            }
            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());

            if (dumpGuides && frameIdx < 3) {
                dumpGuideBuffers(m_ctx,
                                 std::filesystem::path(config.outputDir),
                                 frameIdx + 1,
                                 rW,
                                 rH,
                                 depthNdc,
                                 mvDlss);
            }

            // === STAGE 6: Inverse Tonemap + STAGE 7: Write ===
            const bool writeGeneratedFrames = frameIdx > 0 && !reset;
            if (writeGeneratedFrames) {
                for (const auto& handle : interpHandles) {
                    writeDisplayOrSceneLinear(
                        handle,
                        config.outputFormat == OutputFormat::ExrLinear
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_GENERAL);
                }
            }
            writeDisplayOrSceneLinear(
                displayTex,
                config.outputFormat == OutputFormat::ExrLinear
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_GENERAL);
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
            if (m_stop.load()) {
                if (proresWriter) {
                    proresWriter->cancel();
                } else {
                    writer.cancel();
                }
                vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);
                m_dispatcher->progress().state.store(0);
                LOG_INFO("[SR+FG] Cancelled after {} input frame(s), {} output frame(s)",
                         processedInputs, outputFrameIdx);
                return;
            }
            throw std::runtime_error("SrFgPipeline: processed " + std::to_string(processedInputs) +
                                     " of " + std::to_string(inputFrames) + " input frames");
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
        LOG_INFO("[SR+FG] Complete: {} total output frames", outputFrameIdx);
    } catch (...) {
        m_dispatcher->progress().state.store(3);
        throw;
    }
}

} // namespace dlss
