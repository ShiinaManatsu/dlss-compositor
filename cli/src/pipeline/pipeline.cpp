// Pipeline — unified linear DLSS compositor pipeline implementation.
//
// Stage order (feature-gated):
//   [1] DepthConv + MvConv   (always: Blender → DLSS space)
//   [2] SR -or- RR           (kSR xor kRR)
//   [3] Tonemap               (always: scene-linear → display-ready)
//   [4] FG                    (kFG: frame interpolation)
//   [5] InverseTonemap        (when outputFormat == ExrLinear)
//   [6] Encode + Write        (AsyncFrameWriter / ProResWriter)

#include "pipeline.h"
#include "fg_camera.h"
#include "../core/log.h"
#include "../core/lut_loader.h"
#include "../gpu/passes/prores_convert_pass.h"
#include "../gpu/passes/preview_pass.h"
#include "../io/prores_writer.h"
#include "../io/channel_mapper.h"
#include "../io/rr_channel_mapper.h"
#include "../io/file_watcher.h"

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dlss {

// ─────────────────────────────────────────────────────────────────────────────
// Internal pass texture bundle — all textures for one frame in flight
// ─────────────────────────────────────────────────────────────────────────────

struct PassTextures {
    // SR path
    PooledTextureHandle sceneColorRender;   // scene-linear at render res
    PooledTextureHandle sceneColorOutput;   // SR/RR output at output res (scene-linear)
    PooledTextureHandle depthWorld;         // raw Blender depth
    PooledTextureHandle depthNdc;           // NDC depth (shared with FG)
    PooledTextureHandle mvBlender;          // Blender-space MV
    PooledTextureHandle mvDlss;             // DLSS-space MV (shared with FG)
    // RR extras
    PooledTextureHandle diffuseAlbedo;
    PooledTextureHandle specularAlbedo;
    PooledTextureHandle normals;
    PooledTextureHandle roughness;          // null when Packed
    PooledTextureHandle specularMotion;     // null when not available
    // Post-upscale
    PooledTextureHandle displayTex;         // tonemap output (display-ready)
    PooledTextureHandle nrIn;               // NR color input (RGBA8, packed from displayTex)
    PooledTextureHandle nrOut;              // NR color output (RGBA8)
    PooledTextureHandle displayNr;          // NR output unpacked back to rgb10_a2
    PooledTextureHandle sceneLinear;        // inverse-tonemap output (ExrLinear only)
    std::vector<PooledTextureHandle> interpFrames; // FG generated frames
};

namespace {

constexpr int kDefaultPrefetchDepth = 3;

// ─────────────────────────────────────────────────────────────────────────────
// Small utility helpers (replicated from sr_fg_pipeline.cpp private section)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::pair<int, std::string>> sortedFrameKeys(const DlssConfig& cfg) {
    std::vector<std::pair<int, std::string>> keys;
    keys.reserve(cfg.frames.size());
    for (const auto& [k, _] : cfg.frames) {
        try { keys.emplace_back(std::stoi(k), k); } catch (...) {}
    }
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b){ return a.first < b.first; });
    return keys;
}

std::vector<std::filesystem::path> buildFramePaths(
        const DlssConfig& cfg, const std::filesystem::path& baseDir) {
    std::vector<std::filesystem::path> paths;
    const std::string pattern = cfg.filename_pattern.empty() ? "####.exr" : cfg.filename_pattern;
    for (const auto& [num, _] : sortedFrameKeys(cfg)) {
        paths.emplace_back(baseDir / AsyncFrameWriter::formatPath(pattern, num));
    }
    return paths;
}

const CameraData* findCamera(const DlssConfig& cfg, const std::string& key) {
    auto it = cfg.frames.find(key);
    return it != cfg.frames.end() ? &it->second : nullptr;
}

void identityMatrix(float (&m)[4][4]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m[r][c] = (r == c) ? 1.0f : 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Image layout transitions (identical to sr_fg_pipeline.cpp helpers)
// ─────────────────────────────────────────────────────────────────────────────

void barrierUndefinedToGeneral(VkCommandBuffer cmd, PooledTextureHandle t) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image         = t->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void barrierGeneralToShaderRead(VkCommandBuffer cmd, PooledTextureHandle t) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image         = t->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

// DLSS PG §3.4 Resource States:
//   inputs  = sampled image (VK_IMAGE_USAGE_SAMPLED_BIT, SHADER_READ_ONLY)
//   output  = storage image (VK_IMAGE_USAGE_STORAGE_BIT, GENERAL)
// NGX transitions them back to these states after evaluate.
void transitionForDlssEvaluate(VkCommandBuffer cmd,
                               const PooledTextureHandle& colorOutput,
                               const PooledTextureHandle& depthForNgx,
                               const PooledTextureHandle& mvDlss,
                               bool depthMvWrittenThisCb) {
    VkImageMemoryBarrier2 bs[3]{};
    uint32_t n = 0;
    auto init = [&](VkImageMemoryBarrier2& b) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    };
    init(bs[n]);
    bs[n].srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    bs[n].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bs[n].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bs[n].oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    bs[n].newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    bs[n].image         = colorOutput->image;
    ++n;
    if (depthMvWrittenThisCb) {
        init(bs[n]);
        bs[n].srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bs[n].srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        bs[n].dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        bs[n].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        bs[n].oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        bs[n].newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bs[n].image         = depthForNgx->image;
        ++n;
        bs[n] = bs[n-1];
        bs[n].image = mvDlss->image;
        ++n;
    }
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = n;
    dep.pImageMemoryBarriers    = bs;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void transitionForFgEvaluate(VkCommandBuffer cmd,
                             const PooledTextureHandle& display,
                             std::vector<PooledTextureHandle> interps) {
    std::vector<VkImageMemoryBarrier2> bs;
    bs.reserve(1 + interps.size());
    auto push = [&](VkImage img, VkImageLayout oldL, VkImageLayout newL,
                    VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAcc, VkAccessFlags2 dstAcc) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage; b.srcAccessMask = srcAcc;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.dstAccessMask = dstAcc;
        b.oldLayout    = oldL; b.newLayout = newL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        bs.push_back(b);
    };
    // FG backbuffer is sampled (readWrite=false). DLSS PG §3.4 — inputs SHADER_READ.
    push(display->image,
         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
         VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    for (const auto& h : interps)
        push(h->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
             VK_PIPELINE_STAGE_2_NONE, 0, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(bs.size());
    dep.pImageMemoryBarriers    = bs.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

void transitionDisplayAfterFg(VkCommandBuffer cmd, PooledTextureHandle display,
                              bool forInverseTonemap) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    // FG sampled the backbuffer; NGX returns inputs to SHADER_READ (DLSS PG §3.4).
    b.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (forInverseTonemap) {
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = display->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void transitionInterpOutputsForRead(VkCommandBuffer cmd,
                                    std::vector<PooledTextureHandle> interps,
                                    bool forInverseTonemap) {
    std::vector<VkImageMemoryBarrier2> bs;
    bs.reserve(interps.size());
    for (const auto& h : interps) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
        if (forInverseTonemap) {
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            b.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = h->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        bs.push_back(b);
    }
    if (bs.empty()) return;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = static_cast<uint32_t>(bs.size());
    dep.pImageMemoryBarriers    = bs.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU upload helper for RR guide buffers
// ─────────────────────────────────────────────────────────────────────────────

void uploadRrGuides(UploadBatch& batch, const RrMappedBuffers& mapped, PassTextures& tex) {
    batch.addUpload(tex.diffuseAlbedo,  mapped.diffuseAlbedoRgba.data(),
                    mapped.diffuseAlbedoRgba.size() * sizeof(uint16_t));
    batch.addUpload(tex.specularAlbedo, mapped.specularAlbedoRgba.data(),
                    mapped.specularAlbedoRgba.size() * sizeof(uint16_t));
    batch.addUpload(tex.normals,        mapped.normalsRgba.data(),
                    mapped.normalsRgba.size() * sizeof(uint16_t));
    if (!mapped.roughnessR32.empty() && tex.roughness)
        batch.addUpload(tex.roughness,  mapped.roughnessR32.data(),
                        mapped.roughnessR32.size() * sizeof(float));
    if (!mapped.specularMotionRg.empty() && tex.specularMotion)
        batch.addUpload(tex.specularMotion, mapped.specularMotionRg.data(),
                        mapped.specularMotionRg.size() * sizeof(uint16_t));
}

// ─────────────────────────────────────────────────────────────────────────────
// Debug helpers (only active in debug builds or when env var is set)
// ─────────────────────────────────────────────────────────────────────────────

bool shouldDumpGuides() {
#ifdef NDEBUG
    return false;  // no env-var check in release builds
#else
    const char* v = std::getenv("DLSS_COMPOSITOR_DUMP_GUIDES");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
#endif
}

bool shouldKeepInput() {
#ifdef NDEBUG
    return false;  // onInputConsumed always fires in release; no debug retention
#else
    const char* v = std::getenv("DLSS_COMPOSITOR_KEEP_INPUT");
    return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Small timing helper
// ─────────────────────────────────────────────────────────────────────────────

struct Stopwatch {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    float elapsedMs() const {
        return std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - start).count();
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Pipeline
// ─────────────────────────────────────────────────────────────────────────────

Pipeline::Pipeline(VulkanContext* ctx, NgxContext* ngx)
    : m_ctx(ctx), m_ngx(ngx) {
    if (!ctx) throw std::invalid_argument("Pipeline: VulkanContext must not be null");
    if (!ngx) throw std::invalid_argument("Pipeline: NgxContext must not be null");
}

bool Pipeline::shouldDumpGuides() { return dlss::shouldDumpGuides(); }

// ─────────────────────────────────────────────────────────────────────────────
// run()
// ─────────────────────────────────────────────────────────────────────────────

void Pipeline::run(const PipelineConfig& config) {
    // ── Validate ──────────────────────────────────────────────────────────────
    if (const auto err = validateFeatures(config.features); !err.empty())
        throw std::runtime_error("Pipeline::run: " + err);

    const bool useSR  = (config.features & kSR) != 0;
    const bool useFG  = (config.features & kFG) != 0;
    const bool useNR  = (config.features & kNR) != 0;
    const bool exrOut = (config.outputFormat == OutputFormat::ExrLinear);

    const DlssConfig& dlssCfg = config.dlssConfig;
    const auto frameKeys = sortedFrameKeys(dlssCfg);
    const int inputFrames = static_cast<int>(frameKeys.size());
    if (inputFrames == 0)
        throw std::runtime_error("Pipeline: dlssConfig has no frames");
    if (dlssCfg.fps <= 0)
        throw std::runtime_error("Pipeline: fps must be positive");

    const int fgMult = useFG ? std::max(2, std::min(6, config.fgMultiplier)) : 1;
    const int outputFrames = useFG ? (fgMult * inputFrames - (fgMult - 1)) : inputFrames;
    const float frameTimeMs = 1000.0f / static_cast<float>(dlssCfg.fps); // DLSS §3.14 — from fps

    if (useNR && dlssCfg.is_hdr) {
        throw std::runtime_error(
            "Pipeline: NR (feature 18) is SDR-only (RGBA8 display domain); "
            "HDR tasks cannot enable kNR");
    }

    const uint32_t rW = dlssCfg.render_resolution.width;
    const uint32_t rH = dlssCfg.render_resolution.height;
    const uint32_t oW = dlssCfg.output_resolution.width;
    const uint32_t oH = dlssCfg.output_resolution.height;

    std::filesystem::path inputDir = config.inputDir.empty()
        ? std::filesystem::current_path()
        : std::filesystem::path(config.inputDir);

    m_stop.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    auto& cb = config.callbacks;
    auto fireRunEvent = [&](RunEvent ev, const std::string& detail = {}) {
        if (cb.onRunEvent) cb.onRunEvent(ev, detail);
    };

    LOG_INFO("[Pipeline] Starting: features=0x{:x} {}+{}{}{} input={} output={} {}x{}->{}x{} fps={} rrPreset={}",
             config.features,
             useSR ? "SR" : "RR",
             exrOut ? "Tonemap+InvTonemap" : "Tonemap",
             useNR ? "+NR" : "",
             useFG ? "+FG(" + std::to_string(fgMult) + "x)" : "",
             inputFrames, outputFrames,
             rW, rH, oW, oH, dlssCfg.fps,
             useSR ? "-" : (config.rrPreset == RrPreset::F ? "F" : config.rrPreset == RrPreset::E ? "E" : "D"));

    try {
        fireRunEvent(RunEvent::Started);

        // ── Allocate GPU passes ────────────────────────────────────────────────
        TexturePool pool(m_ctx);

        // SR/RR — mutually exclusive
        std::unique_ptr<DlssSrPass> srPass;
        std::unique_ptr<DlssRrPass> rrPass;
        if (useSR) {
            srPass = std::make_unique<DlssSrPass>(m_ngx, m_ctx, &pool);
            srPass->create(rW, rH, oW, oH, dlssCfg.quality, config.srPreset);
        } else {
            rrPass = std::make_unique<DlssRrPass>(m_ngx, m_ctx, &pool);
            rrPass->create(rW, rH, oW, oH, dlssCfg.quality, config.rrPreset, config.rrRoughness);
        }

        // Always-on passes
        DepthConversionPass  depthPass(m_ctx);
        MvConversionPass     mvPass(m_ctx);
        TonemapPass          tonemapPass(m_ctx);
        InverseTonemapPass   inversePass(m_ctx);
        const TonemapMode    tonemapMode = dlssCfg.is_hdr ? TonemapMode::HDR : TonemapMode::LDR;

        // LUT-based tonemap (optional, overrides built-in if present)
        std::unique_ptr<LutTonemapPass>  lutTonemap;
        std::unique_ptr<LutInversePass>  lutInverse;
        if (!dlssCfg.tonemap_lut.empty()) {
            auto lut = LutLoader::LoadCube(dlssCfg.tonemap_lut);
            if (!lut) throw std::runtime_error("Pipeline: failed to load tonemap_lut: " + lut.error().message);
            lutTonemap = std::make_unique<LutTonemapPass>(m_ctx, lut.value());
        }
        if (!dlssCfg.inverse_lut.empty()) {
            auto lut = LutLoader::LoadCube(dlssCfg.inverse_lut);
            if (!lut) throw std::runtime_error("Pipeline: failed to load inverse_lut: " + lut.error().message);
            lutInverse = std::make_unique<LutInversePass>(m_ctx, lut.value());
        }

        // FG (optional)
        std::unique_ptr<DlssFgPass> fgPass;
        if (useFG) {
            fgPass = std::make_unique<DlssFgPass>(m_ngx, m_ctx, &pool);
            fgPass->create(rW, rH, oW, oH, fgMult);
        }

        // NR (experimental — feature 18). create() is the availability probe:
        // a missing nvngx_dlssnr.dll or failed feature creation throws and the
        // run errors out (fail-loud, no passthrough fallback).
        std::unique_ptr<DlssNrPass> nrPass;
        std::unique_ptr<NrFormatPass> nrPack;    // rgb10_a2 -> rgba8
        std::unique_ptr<NrFormatPass> nrUnpack;  // rgba8 -> rgb10_a2
        if (useNR) {
            nrPass  = std::make_unique<DlssNrPass>(m_ngx, m_ctx);
            nrPass->create(oW, oH,
                           static_cast<NrStyle>(config.nrStyle),
                           config.nrIntensity,
                           static_cast<NrGuidanceMode>(config.nrGuidanceMode));
            nrPack   = std::make_unique<NrFormatPass>(m_ctx, "pack");
            nrUnpack = std::make_unique<NrFormatPass>(m_ctx, "unpack");
        }

        // Preview
        PreviewPass previewPass(m_ctx);

        // Output writers
        const int writerThreads = std::clamp(config.writerThreads, 1, 32);
        const int writerQueueHigh = writerThreads * std::clamp(config.writerQueueMultiplier, 1, 64);
        AsyncFrameWriter frameWriter(writerThreads, writerQueueHigh, writerThreads);
        std::unique_ptr<ProResProgressWriter> proresWriter;
        std::unique_ptr<ProresConvertPass>    proresConvert;
        const bool useProRes = (config.outputFormat == OutputFormat::ProRes422HQ ||
                                config.outputFormat == OutputFormat::H265Mp4);
        if (useProRes) {
            const auto outPath = std::filesystem::path(config.outputDir) / config.filenameTemplate;
            proresWriter = std::make_unique<ProResProgressWriter>(
                outPath, oW, oH,
                dlssCfg.fps * fgMult,
                dlssCfg.is_hdr,
                PixelEncoding::DisplayReady,
                inferHdrMasteringNits(dlssCfg.view_transform),
                writerQueueHigh, writerThreads, writerThreads,
                outputFrames,
                [&cb, outputFrames](int cur, int total) {
                    if (cb.onProgress) cb.onProgress(total > 0 ? (float)cur / (float)total : 0.f);
                },
                config.outputFormat == OutputFormat::H265Mp4
                    ? VideoCodec::H265Hevc : VideoCodec::ProRes422HQ,
                config.h265Options);
            proresConvert = std::make_unique<ProresConvertPass>(m_ctx);
        }

        // ── Per-frame texture descriptors ──────────────────────────────────────
        // DLSS PG §3.4: inputs SAMPLED, output STORAGE.
        const VkImageUsageFlags sampledUpload = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkImageUsageFlags storageRw     = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                              | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        TextureDesc tdColorRender  {rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc tdColorOutput  {oW, oH, VK_FORMAT_R16G16B16A16_SFLOAT, storageRw};
        TextureDesc tdDepthWorld   {rW, rH, VK_FORMAT_R32_SFLOAT,          sampledUpload};
        TextureDesc tdDepthNdc     {rW, rH, VK_FORMAT_R32_SFLOAT,          storageRw};
        TextureDesc tdMvBlender    {rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc tdMvDlss       {rW, rH, VK_FORMAT_R16G16_SFLOAT,       storageRw};
        TextureDesc tdDisplay      {oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storageRw};
        TextureDesc tdInterp       {oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storageRw};
        TextureDesc tdSceneLinear  {oW, oH, VK_FORMAT_R16G16B16A16_SFLOAT, storageRw};
        // NR extras (feature 18 consumes SDR RGBA8, same resolution)
        TextureDesc tdNrIn         {oW, oH, VK_FORMAT_R8G8B8A8_UNORM,          storageRw};
        TextureDesc tdNrOut        {oW, oH, VK_FORMAT_R8G8B8A8_UNORM,          storageRw};
        TextureDesc tdDisplayNr    {oW, oH, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storageRw};
        // RR extras
        TextureDesc tdDiffAlbedo   {rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc tdSpecAlbedo   {rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc tdNormals      {rW, rH, VK_FORMAT_R16G16B16A16_SFLOAT, sampledUpload};
        TextureDesc tdRoughness    {rW, rH, VK_FORMAT_R32_SFLOAT,          sampledUpload};
        TextureDesc tdSpecMotion   {rW, rH, VK_FORMAT_R16G16_SFLOAT,       sampledUpload};

        VkCommandPool cmdPool = m_ctx->createComputeCommandPool();

        // ── Output frame counter / progress ───────────────────────────────────
        int  outputFrameIdx = 0;
        std::atomic<int> writtenFrames{0};

        const bool keepInput  = shouldKeepInput();
        [[maybe_unused]] const bool dumpGuides = shouldDumpGuides();

        // ── Inner lambdas ──────────────────────────────────────────────────────

        // Enqueue one display-ready or scene-linear frame for writing.
        // isGenerated: true for FG-interpolated frames (used by FrameEvent, not write path).
        auto enqueueOutput = [&](PooledTextureHandle src, VkImageLayout oldLayout, bool isGenerated) {
            (void)isGenerated; // consumed by FrameEvent only; write path is format-agnostic
            ++outputFrameIdx;
            const float progress = outputFrames > 0
                ? static_cast<float>(outputFrameIdx) / static_cast<float>(outputFrames) : 0.f;

            if (cb.onProgress) cb.onProgress(progress);

            if (proresWriter) {
                proresWriter->enqueuePackedYuv422Frame(
                    proresConvert->convertAndReadback(src, PixelEncoding::DisplayReady,
                                                      dlssCfg.is_hdr, oldLayout));
                return;
            }

            std::vector<uint8_t> pixels;
            PixelEncoding pixelEncoding;
            if (exrOut) {
                // Inverse tonemap: display-ready → scene-linear for EXR output.
                VkCommandBuffer cmd2 = m_ctx->beginOneTimeCommands(cmdPool);
                auto sceneLinearTex = pool.acquire(tdSceneLinear);
                barrierUndefinedToGeneral(cmd2, sceneLinearTex);
                if (lutInverse)
                    lutInverse->execute(cmd2, src, sceneLinearTex);
                else
                    inversePass.execute(cmd2, src, sceneLinearTex, tonemapMode);
                barrierGeneralToShaderRead(cmd2, sceneLinearTex);
                m_ctx->endOneTimeCommands(cmdPool, cmd2, m_ctx->computeQueue());
                pixels = m_ctx->downloadImageRgba16f(sceneLinearTex->image, oW, oH,
                                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                pixelEncoding = PixelEncoding::SceneLinear;
                pool.release(sceneLinearTex);
            } else {
                pixels = m_ctx->downloadImageA2B10ToRgba16f(src->image, oW, oH, oldLayout);
                pixelEncoding = PixelEncoding::DisplayReady;
            }

            WriteJob job;
            job.format         = config.outputFormat;
            job.outputPath     = std::filesystem::path(config.outputDir) /
                                 AsyncFrameWriter::formatPath(config.filenameTemplate, outputFrameIdx);
            job.width          = oW;
            job.height         = oH;
            job.fps            = dlssCfg.fps;
            job.is_hdr         = dlssCfg.is_hdr;
            job.pixelEncoding  = pixelEncoding;
            job.encoderOptions = config.encoderOptions;
            job.overwriteMode  = config.overwriteMode;
            job.pixels         = std::move(pixels);
            job.completionCallback = [&writtenFrames, outputFrames, &cb]() {
                const int w = writtenFrames.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (cb.onProgress)
                    cb.onProgress(outputFrames > 0 ? (float)w / (float)outputFrames : 0.f);
            };
            frameWriter.enqueue(std::move(job));
        };

        // ── Frame loop ────────────────────────────────────────────────────────
        // Batch mode: prefetchers read the files listed in dlssconfig.
        // Watch mode: same prefetch threads, but each worker waits for the EXR to
        // be fully written by Blender before decoding — EXR IO overlaps GPU work
        // instead of serializing in the frame loop. The pipeline still deletes via
        // onInputConsumed after CPU decode (RAM-disk watermark).
        const auto framePaths = buildFramePaths(dlssCfg, inputDir);
        const int prefetchDepth = std::max(1, config.prefetchDepth);

        std::unique_ptr<FramePrefetcher>   srPrefetcher;
        std::unique_ptr<RrFramePrefetcher> rrPrefetcher;
        if (useSR) srPrefetcher = std::make_unique<FramePrefetcher>(framePaths, prefetchDepth, config.watchInput, &m_stop);
        else       rrPrefetcher = std::make_unique<RrFramePrefetcher>(framePaths, prefetchDepth, config.watchInput, &m_stop);
        if (config.watchInput) {
            LOG_INFO("[Pipeline] Watch mode on {} (prefetch depth {})", inputDir.string(), prefetchDepth);
        }

        int processedInputs = 0;

        for (int fi = 0; fi < inputFrames && !m_stop.load(std::memory_order_acquire); ++fi) {
            Stopwatch frameSw;
            const auto t0 = std::chrono::steady_clock::now();

            // ── [1] Decode EXR ─────────────────────────────────────────────────
            std::filesystem::path sourcePath;
            bool hasGap = false;

            std::optional<MappedBuffers>   srMapped;
            std::optional<RrMappedBuffers> rrMapped;
            std::optional<DecodedFrame>    srDecoded;
            std::optional<RrDecodedFrame>  rrDecoded;

            if (useSR) {
                auto result = srPrefetcher->getNext();
                if (!result) {
                    if (m_stop.load(std::memory_order_acquire)) break;
                    LOG_WARN("[Pipeline] SR frame {} decode: {}", fi, result.error().message);
                    continue;
                }
                sourcePath = result->source_path;
                hasGap     = result->has_gap_after;
                srDecoded  = std::move(*result);
            } else {
                auto result = rrPrefetcher->getNext();
                if (!result) {
                    if (m_stop.load(std::memory_order_acquire)) break;
                    LOG_WARN("[Pipeline] RR frame {} decode: {}", fi, result.error().message);
                    continue;
                }
                sourcePath = result->source_path;
                hasGap     = result->has_gap_after;
                rrDecoded  = std::move(*result);
            }

            const auto tDecode = std::chrono::steady_clock::now();
            if (useSR) {
                auto mapped = ChannelMapper::map(*srDecoded);
                if (!mapped) { LOG_WARN("[Pipeline] SR frame {} map: {}", fi, mapped.error().message); continue; }
                if (mapped->width != rW || mapped->height != rH)
                    throw std::runtime_error("Pipeline: SR frame dimensions mismatch");
                srMapped = std::move(*mapped);
            } else {
                auto mapped = RrChannelMapper::map(*rrDecoded,
                    config.rrRoughness == RrRoughnessMode::Packed);
                if (mapped.width == 0)
                    throw std::runtime_error("Pipeline: RrChannelMapper returned empty frame");
                if (mapped.width != rW || mapped.height != rH)
                    throw std::runtime_error("Pipeline: RR frame dimensions mismatch");
                rrMapped = std::move(mapped);
            }
            // Fire onInputConsumed *after* CPU decode, before GPU upload.
            const auto tMap = std::chrono::steady_clock::now();
            if (cb.onInputConsumed && !keepInput) cb.onInputConsumed(sourcePath);

            // ── [2] Acquire + upload textures ──────────────────────────────────
            PassTextures tex;
            tex.sceneColorRender = pool.acquire(tdColorRender);
            tex.sceneColorOutput = pool.acquire(tdColorOutput);
            tex.depthWorld       = pool.acquire(tdDepthWorld);
            tex.depthNdc         = pool.acquire(tdDepthNdc);  // shared with FG
            tex.mvBlender        = pool.acquire(tdMvBlender);
            tex.mvDlss           = pool.acquire(tdMvDlss);    // shared with FG
            tex.displayTex       = pool.acquire(tdDisplay);
            if (useNR) {
                tex.nrIn      = pool.acquire(tdNrIn);
                tex.nrOut     = pool.acquire(tdNrOut);
                tex.displayNr = pool.acquire(tdDisplayNr);
            }

            UploadBatch batch(m_ctx);
            if (useSR) {
                batch.addUpload(tex.sceneColorRender, srMapped->sceneColor.data(),
                                srMapped->sceneColor.size() * sizeof(uint16_t));
                batch.addUpload(tex.depthWorld,       srMapped->depth.data(),
                                srMapped->depth.size() * sizeof(float));
                batch.addUpload(tex.mvBlender,        srMapped->motionVector.data(),
                                srMapped->motionVector.size() * sizeof(uint16_t));
            } else {
                batch.addUpload(tex.sceneColorRender, rrMapped->colorRgba.data(),
                                rrMapped->colorRgba.size() * sizeof(uint16_t));
                batch.addUpload(tex.depthWorld,       rrMapped->depthLinear.data(),
                                rrMapped->depthLinear.size() * sizeof(float));
                // Mapper already emits DLSS_MV = (X, -Y) as RG16. Do NOT upload into
                // the RGBA blender MV target (copy size mismatch → GPU overrun) and
                // do NOT run mvPass (would flip Y a second time).
                batch.addUpload(tex.mvDlss,           rrMapped->motionRg.data(),
                                rrMapped->motionRg.size() * sizeof(uint16_t));
                // RR guide buffers
                tex.diffuseAlbedo  = pool.acquire(tdDiffAlbedo);
                tex.specularAlbedo = pool.acquire(tdSpecAlbedo);
                tex.normals        = pool.acquire(tdNormals);
                if (config.rrRoughness == RrRoughnessMode::Unpacked)
                    tex.roughness  = pool.acquire(tdRoughness);
                if (rrMapped->hasSpecularMotion)
                    tex.specularMotion = pool.acquire(tdSpecMotion);
                uploadRrGuides(batch, *rrMapped, tex);
            }
            if (useFG)
                for (int j = 0; j < fgMult - 1; ++j)
                    tex.interpFrames.push_back(pool.acquire(tdInterp));
            batch.submit();
            const auto tUpload = std::chrono::steady_clock::now();

            // ── [3] Camera data ────────────────────────────────────────────────
            const std::string& fk  = frameKeys[static_cast<size_t>(fi)].second;
            const std::string& pfk = fi > 0 ? frameKeys[static_cast<size_t>(fi-1)].second : fk;
            const CameraData* cam  = findCamera(dlssCfg, fk);
            const CameraData* prev = fi > 0 ? findCamera(dlssCfg, pfk) : nullptr;
            float defaultProj[4][4]; identityMatrix(defaultProj);
            const float (*proj)[4] = cam ? cam->projection : defaultProj;

            const bool reset = (fi == 0) || hasGap;
            const float jX = cam ? -cam->jitter.x : 0.f;
            const float jY = cam ?  cam->jitter.y : 0.f;

            // ── [4] GPU: DepthConv + MvConv ────────────────────────────────────
            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(cmdPool);
            // SR wants HW/NDC depth. RR is created with Depth=Linear — pass world Z, skip NDC.
            // FG still needs NDC even on the RR path.
            const bool needNdcDepth = useSR || useFG;
            if (needNdcDepth) {
                barrierUndefinedToGeneral(cmd, tex.depthNdc);
                depthPass.execute(cmd, tex.depthWorld, tex.depthNdc,
                                  proj,
                                  cam ? cam->near_clip : 0.1f,
                                  cam ? cam->far_clip  : 100.0f);
            }
            if (useSR) {
                barrierUndefinedToGeneral(cmd, tex.mvDlss);
                mvPass.execute(cmd, tex.mvBlender, tex.mvDlss);
            }
            transitionForDlssEvaluate(cmd, tex.sceneColorOutput,
                                      needNdcDepth ? tex.depthNdc : tex.depthWorld,
                                      tex.mvDlss,
                                      /*depthMvWrittenThisCb=*/useSR);

            // ── [5] SR -or- RR ─────────────────────────────────────────────────
            if (useSR) {
                SrInputs srIn{};
                srIn.sceneColorRenderRes   = tex.sceneColorRender->view;
                srIn.sceneColorOutputRes   = tex.sceneColorOutput->view;
                srIn.depthNdc              = tex.depthNdc->view;
                srIn.motionVecDlss         = tex.mvDlss->view;
                srIn.sceneColorRenderImage = tex.sceneColorRender->image;
                srIn.sceneColorOutputImage = tex.sceneColorOutput->image;
                srIn.depthImage            = tex.depthNdc->image;
                srIn.motionVecImage        = tex.mvDlss->image;
                srIn.renderW = rW; srIn.renderH = rH;
                srIn.outputW = oW; srIn.outputH = oH;
                srIn.jitterX = jX; srIn.jitterY = jY;
                srIn.reset   = reset;
                srIn.frameTimeDeltaMs = frameTimeMs;
                srPass->evaluate(cmd, srIn);
            } else {
                // Guides were uploaded in GENERAL (NGX required layout). Do not
                // barrier GENERAL→SHADER_READ — that was a layout lie and DEVICE_LOST.

                RrInputs rrIn{};
                rrIn.colorRenderRes   = tex.sceneColorRender->view;
                rrIn.outputRes        = tex.sceneColorOutput->view;
                rrIn.depthLinear      = tex.depthWorld->view;
                rrIn.motionVectors    = tex.mvDlss->view;
                rrIn.colorImage       = tex.sceneColorRender->image;
                rrIn.outputImage      = tex.sceneColorOutput->image;
                rrIn.depthImage       = tex.depthWorld->image;
                rrIn.motionImage      = tex.mvDlss->image;
                rrIn.diffuseAlbedo    = tex.diffuseAlbedo  ? tex.diffuseAlbedo->view  : VK_NULL_HANDLE;
                rrIn.specularAlbedo   = tex.specularAlbedo ? tex.specularAlbedo->view : VK_NULL_HANDLE;
                rrIn.normals          = tex.normals         ? tex.normals->view         : VK_NULL_HANDLE;
                rrIn.roughness        = tex.roughness       ? tex.roughness->view       : VK_NULL_HANDLE;
                rrIn.diffuseAlbedoImage  = tex.diffuseAlbedo  ? tex.diffuseAlbedo->image  : VK_NULL_HANDLE;
                rrIn.specularAlbedoImage = tex.specularAlbedo ? tex.specularAlbedo->image : VK_NULL_HANDLE;
                rrIn.normalsImage        = tex.normals         ? tex.normals->image         : VK_NULL_HANDLE;
                rrIn.roughnessImage      = tex.roughness       ? tex.roughness->image       : VK_NULL_HANDLE;
                rrIn.specularMotion      = tex.specularMotion ? tex.specularMotion->view : VK_NULL_HANDLE;
                rrIn.specularMotionImage = tex.specularMotion ? tex.specularMotion->image: VK_NULL_HANDLE;
                rrIn.hasSpecularMotion   = tex.specularMotion != nullptr;
                rrIn.renderW = rW; rrIn.renderH = rH;
                rrIn.outputW = oW; rrIn.outputH = oH;
                rrIn.jitterX = jX; rrIn.jitterY = jY;
                rrIn.reset   = reset;
                rrIn.frameTimeDeltaMs = frameTimeMs;
                rrPass->evaluate(cmd, rrIn);
            }

            // ── [6] Tonemap ────────────────────────────────────────────────────
            // SR/RR output is scene-linear.  Tonemap to display-ready before FG.
            barrierGeneralToShaderRead(cmd, tex.sceneColorOutput);
            barrierUndefinedToGeneral(cmd, tex.displayTex);
            if (lutTonemap) lutTonemap->execute(cmd, tex.sceneColorOutput, tex.displayTex);
            else            tonemapPass.execute(cmd, tex.sceneColorOutput, tex.displayTex, tonemapMode);

            // ── [6.5] NR (experimental — display-domain, same-res) ─────────────
            // Pack rgb10_a2 → rgba8, run DLSSNR, unpack. The pipeline continues
            // from `displayOut` so FG/output/preview all see the denoised frame.
            PooledTextureHandle displayOut = tex.displayTex;
            if (useNR) {
                // Guidance selection by mode: 0 all / 1 zero / 2 motion / 3 depth.
                const int gMode = std::clamp(config.nrGuidanceMode, 0, 3);
                const bool passMotion = (gMode == 0 || gMode == 2);
                const bool passDepth  = (gMode == 0 || gMode == 3);

                // displayTex: tonemap wrote it (GENERAL storage) → SHADER_READ for the pack.
                barrierGeneralToShaderRead(cmd, tex.displayTex);
                // Pack writes nrIn (execute transitions UNDEFINED → GENERAL itself).
                nrPack->execute(cmd, tex.displayTex, tex.nrIn);

                // NGX eval contract (DLSS SR §3.4): inputs SHADER_READ, output GENERAL.
                // nrIn: GENERAL → SHADER_READ; nrOut: UNDEFINED → GENERAL.
                VkImageMemoryBarrier2 nb[2]{};
                auto ninit = [&](VkImageMemoryBarrier2& b, VkImage img,
                                 VkImageLayout oldL, VkImageLayout newL,
                                 VkAccessFlags2 srcAcc, VkAccessFlags2 dstAcc) {
                    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
                    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    b.srcAccessMask = srcAcc;
                    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    b.dstAccessMask = dstAcc;
                    b.oldLayout = oldL;
                    b.newLayout = newL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = img;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                };
                ninit(nb[0], tex.nrIn->image,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                ninit(nb[1], tex.nrOut->image,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_ACCESS_2_NONE, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                nb[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                {
                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dep.imageMemoryBarrierCount = 2;
                    dep.pImageMemoryBarriers = nb;
                    vkCmdPipelineBarrier2(cmd, &dep);
                }

                // Guides at NR-eval time:
                //   mvDlss    — already SHADER_READ on both paths (SR promoted it
                //               pre-eval; RR consumed it in NGX eval, which returns
                //               inputs to SHADER_READ per DLSS PG §3.4).
                //   depthNdc  — SHADER_READ on SR; on RR it was only written by
                //               depthPass (GENERAL) and RR never consumed it, so
                //               promote it here.
                std::vector<VkImageMemoryBarrier2> gb;
                if (!useSR && passDepth && tex.depthNdc) {
                    VkImageMemoryBarrier2 b{};
                    ninit(b, tex.depthNdc->image,
                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                    gb.push_back(b);
                }
                if (!gb.empty()) {
                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dep.imageMemoryBarrierCount = static_cast<uint32_t>(gb.size());
                    dep.pImageMemoryBarriers = gb.data();
                    vkCmdPipelineBarrier2(cmd, &dep);
                }

                NrInputs nrIn{};
                nrIn.colorView    = tex.nrIn->view;
                nrIn.colorImage   = tex.nrIn->image;
                nrIn.outputView   = tex.nrOut->view;
                nrIn.outputImage  = tex.nrOut->image;
                nrIn.mvView       = passMotion ? tex.mvDlss->view   : VK_NULL_HANDLE;
                nrIn.mvImage      = passMotion ? tex.mvDlss->image  : VK_NULL_HANDLE;
                nrIn.depthView    = passDepth  ? tex.depthNdc->view : VK_NULL_HANDLE;
                nrIn.depthImage   = passDepth  ? tex.depthNdc->image: VK_NULL_HANDLE;
                nrIn.renderWidth  = rW;
                nrIn.renderHeight = rH;
                nrIn.reset        = reset;
                nrPass->evaluate(cmd, nrIn);

                // After eval NGX returns inputs to SHADER_READ and output to GENERAL.
                // Unpack nrOut → displayNr (execute transitions displayNr UNDEFINED → GENERAL).
                nrUnpack->execute(cmd, tex.nrOut, tex.displayNr);
                displayOut = tex.displayNr;
            }

            // ── [7] FG (optional) ──────────────────────────────────────────────
            if (useFG) {
                // RR path skips NDC conversion into the NGX evaluate barrier; FG still
                // samples depthNdc so promote it to SHADER_READ before eval.
                if (!useSR && !useNR) barrierGeneralToShaderRead(cmd, tex.depthNdc);
                transitionForFgEvaluate(cmd, displayOut, tex.interpFrames);

                FgInputs fgIn{};
                fgIn.backbufferDisplayReady = displayOut->view;
                fgIn.depthNdcRenderRes      = tex.depthNdc->view;
                fgIn.motionVecDlssRenderRes = tex.mvDlss->view;
                fgIn.backbufferImage        = displayOut->image;
                fgIn.depthImage             = tex.depthNdc->image;
                fgIn.motionVecImage         = tex.mvDlss->image;
                for (const auto& h : tex.interpFrames) {
                    fgIn.outputInterp.push_back(h->view);
                    fgIn.outputInterpImages.push_back(h->image);
                }
                fgIn.backbufferFormat   = displayOut->desc.format;
                fgIn.depthFormat        = tex.depthNdc->desc.format;
                fgIn.motionVecFormat    = tex.mvDlss->desc.format;
                fgIn.width              = oW;      fgIn.height           = oH;
                fgIn.depthMotionWidth   = rW;      fgIn.depthMotionHeight = rH;
                fgIn.is_hdr             = dlssCfg.is_hdr;
                fgIn.reset              = reset;
                fgIn.multiplier         = fgMult;
                fillFgCameraInputs(fgIn, cam, prev);
                fgIn.jitterClipX = cam ? -2.f * cam->jitter.x / (float)rW : 0.f;
                fgIn.jitterClipY = cam ? -2.f * cam->jitter.y / (float)rH : 0.f;
                fgPass->evaluate(cmd, fgIn);

                transitionDisplayAfterFg(cmd, displayOut, exrOut);
                transitionInterpOutputsForRead(cmd, tex.interpFrames, exrOut);
            } else {
                // No FG: transition display for downstream read.
                if (exrOut) barrierGeneralToShaderRead(cmd, displayOut);
                else {
                    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                    b.srcAccessMask= VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                    b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                    b.dstAccessMask= VK_ACCESS_2_TRANSFER_READ_BIT;
                    b.oldLayout    = VK_IMAGE_LAYOUT_GENERAL;
                    b.newLayout    = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    b.image = displayOut->image;
                    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
                    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                    dep.imageMemoryBarrierCount = 1;
                    dep.pImageMemoryBarriers    = &b;
                    vkCmdPipelineBarrier2(cmd, &dep);
                }
            }

            m_ctx->endOneTimeCommands(cmdPool, cmd, m_ctx->computeQueue());
            // Compute and transfer queues may be different families. NGX/RR
            // writes on compute; PNG/EXR readback uses the transfer queue.
            // Drain the device so the image is idle before the layout-mismatched
            // cross-queue copy (otherwise vkQueueSubmit returns DEVICE_LOST).
            m_ctx->waitIdle();
            const auto tGpu = std::chrono::steady_clock::now();

            // ── [8] Preview (non-generated frames only) ────────────────────────
            const bool wantPreview = cb.wantsPreview && cb.wantsPreview();
            if (wantPreview && cb.onPreview && cb.encodePreview) {
                PreviewReadback prev2 = previewPass.renderAndReadback(
                    displayOut, PreviewInputMode::DisplayReady, dlssCfg.is_hdr,
                    exrOut ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
                if (!prev2.rgba8.empty()) {
                    auto jpeg = cb.encodePreview(prev2.width, prev2.height, prev2.rgba8);
                    if (!jpeg.empty()) cb.onPreview(std::move(jpeg));
                }
            }

            const auto tPreview = std::chrono::steady_clock::now();

            // ── [9] Write output frames ────────────────────────────────────────
            const bool writeInterp = useFG && fi > 0 && !reset;
            if (writeInterp) {
                for (auto& h : tex.interpFrames) {
                    enqueueOutput(h,
                        exrOut ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_GENERAL, /*isGenerated=*/true);
                }
            }
            enqueueOutput(displayOut,
                exrOut ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, /*isGenerated=*/false);

            // ── [10] Fire frame event ──────────────────────────────────────────
            if (cb.onFrame) {
                FrameEvent ev{};
                ev.inputIndex   = fi;
                ev.inputTotal   = inputFrames;
                ev.outputIndex  = std::max(0, outputFrameIdx - 1);  // FrameEvent is 0-based
                ev.outputTotal  = outputFrames;
                ev.isGenerated  = false;
                ev.wasReset     = reset;
                ev.frameEvalMs  = frameSw.elapsedMs();
                cb.onFrame(ev);
            }

            // ── Diagnostics ────────────────────────────────────────────────────
            if (cb.onFrameDiagnostics) {
                // Structured JSON string; front end can parse or log as-is.
                const std::string diag =
                    "{\"fi\":" + std::to_string(fi) +
                    ",\"out\":" + std::to_string(outputFrameIdx) +
                    ",\"reset\":" + (reset ? "true" : "false") +
                    ",\"evalMs\":" + std::to_string(frameSw.elapsedMs()) +
                    ",\"mode\":\"" + (useSR ? "SR" : "RR") + "\""
                    ",\"fg\":" + (useFG ? "true" : "false") +
                    ",\"nr\":" + (useNR ? "true" : "false") + "}";
                cb.onFrameDiagnostics(diag);
            }

            // ── [11] Release textures ──────────────────────────────────────────
            pool.release(tex.sceneColorRender);
            pool.release(tex.sceneColorOutput);
            pool.release(tex.depthWorld);
            pool.release(tex.depthNdc);
            pool.release(tex.mvBlender);
            pool.release(tex.mvDlss);
            pool.release(tex.displayTex);
            if (tex.nrIn)      pool.release(tex.nrIn);
            if (tex.nrOut)     pool.release(tex.nrOut);
            if (tex.displayNr) pool.release(tex.displayNr);
            if (tex.diffuseAlbedo)  pool.release(tex.diffuseAlbedo);
            if (tex.specularAlbedo) pool.release(tex.specularAlbedo);
            if (tex.normals)        pool.release(tex.normals);
            if (tex.roughness)      pool.release(tex.roughness);
            if (tex.specularMotion) pool.release(tex.specularMotion);
            for (auto& h : tex.interpFrames) pool.release(h);

            ++processedInputs;
            {
                const auto tWrite = std::chrono::steady_clock::now();
                const auto ms = [](auto a, auto b) {
                    return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
                };
                LOG_INFO("[Pipeline] timing input {}: decode={}ms map={}ms upload={}ms gpu={}ms preview={}ms write={}ms total={}ms",
                         fi + 1,
                         ms(t0, tDecode),
                         ms(tDecode, tMap),
                         ms(tMap, tUpload),
                         ms(tUpload, tGpu),
                         ms(tGpu, tPreview),
                         ms(tPreview, tWrite),
                         ms(t0, tWrite));
            }
            LOG_INFO("[Pipeline] {}/{} input -> {}/{} output ({}ms)",
                     fi+1, inputFrames, outputFrameIdx, outputFrames,
                     static_cast<int>(frameSw.elapsedMs()));
        } // frame loop

        // ── Finalize writers ───────────────────────────────────────────────────
        if (m_stop.load(std::memory_order_acquire)) {
            if (proresWriter) proresWriter->cancel();
            else              frameWriter.cancel();
            vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);
            m_running.store(false, std::memory_order_release);
            fireRunEvent(RunEvent::Cancelled,
                "stopped after " + std::to_string(processedInputs) + " frames");
            LOG_INFO("[Pipeline] Cancelled after {}/{} frames", processedInputs, inputFrames);
            return;
        }

        if (processedInputs != inputFrames) {
            throw std::runtime_error("Pipeline: processed " + std::to_string(processedInputs) +
                                     " of " + std::to_string(inputFrames) + " input frames");
        }

        if (proresWriter) proresWriter->close();
        else              frameWriter.flush();
        vkDestroyCommandPool(m_ctx->device(), cmdPool, nullptr);
        m_running.store(false, std::memory_order_release);
        fireRunEvent(RunEvent::Completed);
        LOG_INFO("[Pipeline] Complete: {} output frames", outputFrameIdx);

    } catch (const std::exception& e) {
        m_running.store(false, std::memory_order_release);
        fireRunEvent(RunEvent::Error, e.what());
        throw;
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        fireRunEvent(RunEvent::Error, "unknown exception");
        throw;
    }
}

} // namespace dlss
