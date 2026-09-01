// NrToolSession implementation — see nr_tool_session.h.

#include "nr_tool_session.h"

#include "fg_camera.h"

#include "../core/log.h"
#include "../io/async_writer.h"
#include "../io/prores_writer.h"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

namespace dlss {

namespace {

/// PNG16 sequence as color source: enumerate *.png whose stem ends in a frame
/// number (e.g. "frame_0001.png", "0042.png"), sorted numerically. Our Stage-A
/// PNG16 exports are display-ready sRGB-encoded 16-bit — downshift to RGBA8
/// as-is (no color transform: NR consumes the display-domain signal).
std::vector<std::filesystem::path> enumeratePngSequence(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        throw std::runtime_error("NrTool: color source is not a file or directory: " + dir.string());
    }
    auto trailingNumber = [](const std::filesystem::path& p) -> int {
        const std::string stem = p.stem().string();
        std::size_t end = stem.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(stem[end - 1])) != 0) --end;
        if (end == stem.size()) return -1;  // no trailing digits
        try {
            return std::stoi(stem.substr(end));
        } catch (...) {
            return -1;
        }
    };
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".png") continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("dlssnrbuffer_", 0) == 0) continue;  // guidance, not color
        if (trailingNumber(entry.path()) < 0) continue;
        files.push_back(entry.path());
    }
    if (ec) {
        throw std::runtime_error("NrTool: cannot list " + dir.string() + ": " + ec.message());
    }
    std::sort(files.begin(), files.end(), [&trailingNumber](const auto& a, const auto& b) {
        return trailingNumber(a) < trailingNumber(b);
    });
    return files;
}

/// Load one PNG16 frame → RGBA8 (high byte, no color transform).
std::vector<std::uint8_t> loadPng16AsRgba8(const std::filesystem::path& p,
                                           uint32_t& w, uint32_t& h) {
    auto input = OIIO::ImageInput::open(p.string());
    if (!input) throw std::runtime_error("NrTool: cannot open " + p.string());
    const OIIO::ImageSpec& spec = input->spec();
    w = static_cast<uint32_t>(spec.width);
    h = static_cast<uint32_t>(spec.height);
    const std::size_t px = static_cast<std::size_t>(w) * h;
    std::vector<std::uint16_t> pixels(px * static_cast<std::size_t>(spec.nchannels));
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT16, pixels.data())) {
        const std::string err = input->geterror();
        input->close();
        throw std::runtime_error("NrTool: PNG read failed: " + p.string() +
                                 (err.empty() ? "" : ": " + err));
    }
    input->close();

    std::vector<std::uint8_t> rgba8(px * 4, 255);
    for (std::size_t i = 0; i < px; ++i) {
        for (int c = 0; c < 4; ++c) {
            const std::size_t s = i * static_cast<std::size_t>(spec.nchannels);
            // Missing alpha channel → opaque (set above); else take the high byte.
            if (c < spec.nchannels) {
                rgba8[i * 4 + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(pixels[s + static_cast<std::size_t>(c)] >> 8);
            }
        }
    }
    return rgba8;
}

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16;
    const std::uint32_t exp  = (h & 0x7C00u) >> 10;
    const std::uint32_t man  = h & 0x03FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            int e = -1;
            std::uint32_t m = man;
            do { ++e; m <<= 1; } while ((m & 0x0400u) == 0);
            bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23) | ((m & 0x03FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/// A2B10 download bytes (rgba16f, 4×half per pixel) → RGBA8 for JPEG/PNG paths.
std::vector<std::uint8_t> rgba16fBytesToRgba8(const std::vector<std::uint8_t>& f16,
                                              std::size_t pixelCount) {
    if (f16.size() < pixelCount * 8) {
        throw std::runtime_error("NrTool: FG readback short read (" +
                                 std::to_string(f16.size()) + " bytes)");
    }
    std::vector<std::uint8_t> rgba8(pixelCount * 4, 255);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        for (int c = 0; c < 3; ++c) {
            const std::uint16_t h = static_cast<std::uint16_t>(
                f16[i * 8 + static_cast<std::size_t>(c) * 2]) |
                (static_cast<std::uint16_t>(f16[i * 8 + static_cast<std::size_t>(c) * 2 + 1]) << 8);
            const float v = std::clamp(halfToFloat(h), 0.0f, 1.0f);
            rgba8[i * 4 + static_cast<std::size_t>(c)] =
                static_cast<std::uint8_t>(v * 255.0f + 0.5f);
        }
    }
    return rgba8;
}

void barrierUndefinedToGeneral(VkCommandBuffer cmd, const PooledTextureHandle& tex) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
    b.srcAccessMask = VK_ACCESS_2_NONE;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

/// GENERAL (compute storage write) → SHADER_READ (sampled read).
void barrierGeneralToShaderRead(VkCommandBuffer cmd, const PooledTextureHandle& tex) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout     = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = tex->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

NrToolSession::NrToolSession(VulkanContext* ctx, NgxContext* ngx,
                             std::filesystem::path colorSource,
                             std::filesystem::path bufferDir,
                             std::filesystem::path cameraConfig)
    : m_ctx(ctx), m_ngx(ngx), m_pool(ctx) {
    if (!ctx || !ngx) throw std::invalid_argument("NrToolSession: GPU contexts must not be null");

    // ── Color source: video file or PNG16 sequence ────────────────────────────
    std::error_code ec;
    if (std::filesystem::is_regular_file(colorSource, ec)) {
        m_video.open(colorSource);
        m_isVideo        = true;
        m_info.width     = m_video.width();
        m_info.height    = m_video.height();
        m_info.fps       = m_video.fps();
        m_info.frames    = static_cast<uint32_t>(m_video.frameCount());
        m_info.colorKind = "video";
    } else {
        m_pngFiles = enumeratePngSequence(colorSource);
        if (m_pngFiles.empty()) {
            throw std::runtime_error(
                "NrTool: no numbered .png frames in " + colorSource.string() +
                " — the color source must be a video file or a PNG16 sequence");
        }
        uint32_t w = 0, h = 0;
        loadPng16AsRgba8(m_pngFiles.front(), w, h);  // probe dimensions
        m_isVideo        = false;
        m_info.width     = w;
        m_info.height    = h;
        m_info.fps       = 0.0;  // sequences carry no fps; GUI supplies export fps
        m_info.frames    = static_cast<uint32_t>(m_pngFiles.size());
        m_info.colorKind = "png_sequence";
    }
    m_info.colorSource = colorSource.string();

    // ── Guidance: dlssnrbuffer sequence (optional — no buffer means the
    //    ForceZero blind filter only; enforced in evaluateFrame/evaluateFg) ────
    m_hasBuffer = !bufferDir.empty();
    if (m_hasBuffer) {
        m_buffer.open(bufferDir);
        m_info.bufferWidth  = m_buffer.width();
        m_info.bufferHeight = m_buffer.height();
    }
    m_info.bufferDir = bufferDir.string();

    // ── Camera data (dlssconfig.json): required by FG, ignored by NR-only ─────
    if (!cameraConfig.empty()) {
        auto loaded = DlssConfigLoader::Load(cameraConfig);
        if (!loaded) {
            throw std::runtime_error("NrTool: cannot load dlssconfig " + cameraConfig.string() +
                                     ": " + loaded.error().message);
        }
        m_camConfig = std::make_unique<DlssConfig>(std::move(loaded.value()));
        m_camKeys.reserve(m_camConfig->frames.size());
        for (const auto& [key, cam] : m_camConfig->frames) {
            (void)cam;
            m_camKeys.push_back(key);
        }
        std::sort(m_camKeys.begin(), m_camKeys.end(), [](const std::string& a, const std::string& b) {
            try {
                return std::stoi(a) < std::stoi(b);
            } catch (...) {
                return a < b;
            }
        });
    }
    m_info.cameraConfig = cameraConfig.string();

    if (m_info.frames == 0) {
        throw std::runtime_error("NrTool: color source frame count unknown");
    }
    if (m_hasBuffer && m_info.frames != m_buffer.frames()) {
        throw std::runtime_error(
            "NrTool: frame count mismatch — color has " + std::to_string(m_info.frames) +
            ", dlssnrbuffer has " + std::to_string(m_buffer.frames()) +
            ". Re-export the buffer with the same frame range.");
    }
    if (m_camConfig && m_camKeys.size() != m_info.frames) {
        throw std::runtime_error(
            "NrTool: frame count mismatch — color has " + std::to_string(m_info.frames) +
            ", dlssconfig has " + std::to_string(m_camKeys.size()) +
            " camera frames. Re-export with the same frame range.");
    }
}

NrToolSession::~NrToolSession() {
    LOG_INFO("[NrTool] session teardown begin");
    m_fgPass.reset();       // FG feature shutdown first
    m_nrPass.reset();       // feature 18 shutdown (snippet still loaded)
    m_nrUnpack.reset();
    m_proresConvert.reset();
    if (m_cmdPool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_ctx->device(), m_cmdPool, nullptr);
        m_cmdPool = VK_NULL_HANDLE;
    }
    LOG_INFO("[NrTool] session teardown done");
}

void NrToolSession::ensureCreated() {
    if (m_nrPass) return;

    const TextureDesc tdColorIn  {m_info.width,  m_info.height,  VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    const VkImageUsageFlags storageRw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                      | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const TextureDesc tdNrOut    {m_info.width, m_info.height, VK_FORMAT_R8G8B8A8_UNORM, storageRw};
    const TextureDesc tdDisplayNr{m_info.width, m_info.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storageRw};

    m_colorIn  = m_pool.acquire(tdColorIn);
    if (m_hasBuffer) {
        const TextureDesc tdMv    {m_info.bufferWidth, m_info.bufferHeight, VK_FORMAT_R16G16_SFLOAT,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        const TextureDesc tdDepth {m_info.bufferWidth, m_info.bufferHeight, VK_FORMAT_R32_SFLOAT,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        m_mvTex    = m_pool.acquire(tdMv);
        m_depthTex = m_pool.acquire(tdDepth);
    }
    m_nrOut    = m_pool.acquire(tdNrOut);
    m_displayNr= m_pool.acquire(tdDisplayNr);

    // Feature creation is the availability probe — fail loud here.
    m_nrPass = std::make_unique<DlssNrPass>(m_ngx, m_ctx);
    m_nrPass->create(m_info.width, m_info.height, m_settings);
    m_nrUnpack      = std::make_unique<NrFormatPass>(m_ctx, "unpack");
    m_proresConvert = std::make_unique<ProresConvertPass>(m_ctx);
    m_cmdPool       = m_ctx->createComputeCommandPool();

    LOG_INFO("[NrTool] session ready: {} frames {}x{} (buffer {}x{})",
             m_info.frames, m_info.width, m_info.height,
             m_info.bufferWidth, m_info.bufferHeight);
}

void NrToolSession::ensureFgCreated(int multiplier) {
    if (m_fgPass && m_fgMultiplier == multiplier) return;
    if (m_fgPass) {
        m_fgPass.reset();
        m_fgMultiplier = 0;
    }
    // Pipeline parity (sr_fg_pipeline): internal/render dims = the MV/depth
    // resolution, output dims = the backbuffer (color) resolution. Declaring
    // internal = color res while the MV subrect is render res makes FG
    // misinterpret MV scale — interpolation degrades to a cross-fade.
    m_fgPass = std::make_unique<DlssFgPass>(m_ngx, m_ctx, &m_pool);
    m_fgPass->create(m_info.bufferWidth, m_info.bufferHeight,
                     m_info.width, m_info.height, multiplier);
    m_fgMultiplier = multiplier;
    LOG_INFO("[NrTool] FG feature created ({}x{} -> {}x{}, {}x)",
             m_info.bufferWidth, m_info.bufferHeight,
             m_info.width, m_info.height, multiplier);
}

void NrToolSession::evaluateFrame(std::int64_t frameIndex, bool nrEnabled, bool needGuidance) {
    ensureCreated();
    if (nrEnabled && !m_hasBuffer && m_settings.guidanceMode != NrGuidanceMode::ForceZero) {
        throw std::runtime_error(
            "NrTool: guidance needs a dlssnrbuffer session — reopen with buffer_dir "
            "or set guidance mode to 无");
    }
    LOG_DEBUG("[NrTool] eval frame {} begin (nr={} guide={})", frameIndex, nrEnabled, needGuidance);

    // ── Decode color ──────────────────────────────────────────────────────────
    std::vector<std::uint8_t> colorRgba8;
    if (m_isVideo) {
        m_video.readFrameRgba8(frameIndex, colorRgba8);
    } else {
        uint32_t w = 0, h = 0;
        colorRgba8 = loadPng16AsRgba8(m_pngFiles[static_cast<std::size_t>(frameIndex)], w, h);
    }

    // ── Upload (NGX contract: inputs SHADER_READ) ─────────────────────────────
    UploadBatch batch(m_ctx);
    batch.addUpload(m_colorIn, colorRgba8.data(), colorRgba8.size());

    // ── Decode guidance ───────────────────────────────────────────────────────
    std::vector<std::uint8_t> mvBytes;
    std::vector<float>        depth;
    if (m_hasBuffer && needGuidance) {
        m_buffer.readFrame(static_cast<std::size_t>(frameIndex), mvBytes, depth);
        batch.addUpload(m_mvTex,    mvBytes.data(),    mvBytes.size());
        batch.addUpload(m_depthTex, depth.data(),      depth.size() * sizeof(float));
    }
    batch.submit();

    // ── Evaluate ──────────────────────────────────────────────────────────────
    if (nrEnabled) {
        const bool reset = (m_lastFrame < 0) || (frameIndex != m_lastFrame + 1);

        VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(m_cmdPool);
        // Output layout: UNDEFINED→GENERAL each eval (legal discard; pipeline parity).
        VkImageMemoryBarrier2 ob{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ob.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        ob.srcAccessMask = VK_ACCESS_2_NONE;
        ob.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        ob.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        ob.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ob.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ob.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ob.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ob.image = m_nrOut->image;
        ob.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &ob;
            vkCmdPipelineBarrier2(cmd, &dep);
        }

        NrInputs in{};
        in.colorView    = m_colorIn->view;
        in.colorImage   = m_colorIn->image;
        in.outputView   = m_nrOut->view;
        in.outputImage  = m_nrOut->image;
        if (m_hasBuffer) {
            in.mvView       = m_mvTex->view;
            in.mvImage      = m_mvTex->image;
            in.depthView    = m_depthTex->view;
            in.depthImage   = m_depthTex->image;
            in.renderWidth  = m_info.bufferWidth;
            in.renderHeight = m_info.bufferHeight;
        }
        in.reset        = reset;
        m_nrPass->updateSettings(m_settings);
        m_nrPass->evaluate(cmd, in);
        m_ctx->endOneTimeCommands(m_cmdPool, cmd, m_ctx->computeQueue());
        // Pipeline parity: NGX eval may run on the snippet's own internal queue —
        // drain the device before any readback (downloadImageRaw submits its own).
        m_ctx->waitIdle();
        LOG_DEBUG("[NrTool] eval frame {} done (reset={})", frameIndex, reset);
    }

    m_lastFrame = frameIndex;
}

std::vector<PooledTextureHandle> NrToolSession::evaluateFg(std::int64_t frameIndex, bool reset,
                                                           const FgSettings& fg) {
    if (!m_hasBuffer) {
        throw std::runtime_error(
            "NrTool: FG 需要 dlssnrbuffer（MV/深度引导）— 开启 NR buffer 重新导出");
    }
    if (!m_camConfig) {
        throw std::runtime_error(
            "NrTool: FG 需要 dlssconfig.json（逐帧相机数据）— 缓冲目录缺失该文件");
    }
    if (static_cast<std::size_t>(frameIndex) >= m_camKeys.size()) {
        throw std::runtime_error("NrTool: FG frame " + std::to_string(frameIndex) +
                                 " beyond dlssconfig camera keys");
    }
    ensureFgCreated(fg.multiplier);

    // Interpolated outputs (multiplier-1 per input), written GENERAL by FG.
    const VkImageUsageFlags fgOutUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                       | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const TextureDesc tdInterp{m_info.width, m_info.height,
                               VK_FORMAT_A2B10G10R10_UNORM_PACK32, fgOutUsage};
    std::vector<PooledTextureHandle> interps;
    interps.reserve(static_cast<std::size_t>(fg.multiplier - 1));
    for (int j = 0; j < fg.multiplier - 1; ++j) {
        interps.push_back(m_pool.acquire(tdInterp));
    }

    // FG backbuffer = m_displayNr (A2B10G10R10): unpack the processed color into it.
    const PooledTextureHandle& colorSrc = m_settings.enabled ? m_nrOut : m_colorIn;

    VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(m_cmdPool);
    if (m_settings.enabled) {
        barrierGeneralToShaderRead(cmd, m_nrOut);  // unpack reads it sampled
    }
    m_nrUnpack->execute(cmd, colorSrc, m_displayNr);  // dst UNDEFINED→GENERAL inside
    barrierGeneralToShaderRead(cmd, m_displayNr);     // FG backbuffer input
    for (const auto& h : interps) {
        barrierUndefinedToGeneral(cmd, h);
    }

    FgInputs in{};
    in.backbufferDisplayReady = m_displayNr->view;
    in.backbufferImage        = m_displayNr->image;
    in.depthNdcRenderRes      = m_depthTex->view;
    in.depthImage             = m_depthTex->image;
    in.motionVecDlssRenderRes = m_mvTex->view;
    in.motionVecImage         = m_mvTex->image;
    in.outputInterp.reserve(interps.size());
    in.outputInterpImages.reserve(interps.size());
    for (const auto& h : interps) {
        in.outputInterp.push_back(h->view);
        in.outputInterpImages.push_back(h->image);
    }
    in.backbufferFormat  = m_displayNr->desc.format;
    in.depthFormat       = m_depthTex->desc.format;
    in.motionVecFormat   = m_mvTex->desc.format;
    in.width             = m_info.width;
    in.height            = m_info.height;
    in.depthMotionWidth  = m_info.bufferWidth;
    in.depthMotionHeight = m_info.bufferHeight;
    in.is_hdr            = false;
    in.reset             = reset;
    in.multiplier        = fg.multiplier;

    const CameraData& cam = m_camConfig->frames.at(m_camKeys[static_cast<std::size_t>(frameIndex)]);
    const CameraData* prevCam = frameIndex > 0
        ? &m_camConfig->frames.at(m_camKeys[static_cast<std::size_t>(frameIndex) - 1])
        : nullptr;
    fillFgCameraInputs(in, &cam, prevCam);
    // DLSS FG §6: clip-space jitter, separate from unjittered matrices.
    // Blender jitter is in render-res pixel units — normalize by the buffer
    // (render) dims, same as sr_fg_pipeline's /rW.
    in.jitterClipX = -2.0f * cam.jitter.x / static_cast<float>(m_info.bufferWidth);
    in.jitterClipY = -2.0f * cam.jitter.y / static_cast<float>(m_info.bufferHeight);

    // Debug facility: DLSS_NRTOOL_DUMP_DIR=<dir> dumps the exact FG eval inputs
    // (frames 1..2) as raw device bytes — pre_mv RG16F, pre_depth R32F,
    // pre_color RGBA8 (sanity: known color-source content). Proves the uploads
    // landed before eval; this hook is what exposed the 0.5.5 reader corruption.
    // Zero cost when the env var is unset. Post-eval mv/depth are deliberately
    // NOT dumped: NGX dilates MVs in place, so post-eval state is overwritten.
    if (const char* dumpDir = std::getenv("DLSS_NRTOOL_DUMP_DIR")) {
        if (frameIndex >= 1 && frameIndex <= 2) {
            const std::filesystem::path dir{dumpDir};
            std::filesystem::create_directories(dir);
            debugDumpRaw(dir, frameIndex, "pre_mv",    m_mvTex->image,    m_info.bufferWidth, m_info.bufferHeight);
            debugDumpRaw(dir, frameIndex, "pre_depth", m_depthTex->image, m_info.bufferWidth, m_info.bufferHeight);
            debugDumpRaw(dir, frameIndex, "pre_color", m_colorIn->image,  m_info.width, m_info.height);
            LOG_INFO("[NrTool] dumped pre-eval guidance for frame {}", frameIndex);
        }
    }

    m_fgPass->evaluate(cmd, in);
    m_ctx->endOneTimeCommands(m_cmdPool, cmd, m_ctx->computeQueue());
    m_ctx->waitIdle();
    LOG_DEBUG("[NrTool] FG frame {} done (reset={}, {}x)", frameIndex, reset, fg.multiplier);

    // Debug facility (see pre-eval note): post-eval interp output, A2B10.
    if (const char* dumpDir = std::getenv("DLSS_NRTOOL_DUMP_DIR")) {
        if (frameIndex >= 1 && frameIndex <= 2) {
            debugDumpRaw(std::filesystem::path{dumpDir}, frameIndex, "interp",
                         interps[0]->image, m_info.width, m_info.height);
            LOG_INFO("[NrTool] dumped FG interp for frame {}", frameIndex);
        }
    }
    return interps;
}

void NrToolSession::debugDumpRaw(const std::filesystem::path& dir, std::int64_t frameIndex,
                                 const char* tag, VkImage image, uint32_t w, uint32_t h) {
    auto bytes = m_ctx->downloadImageRaw(image, w, h, 4,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, /*restoreLayout=*/true);
    std::ofstream f(dir / (std::string{"f"} + std::to_string(frameIndex) + "_" + tag + ".raw"),
                    std::ios::binary);
    if (!f) {
        throw std::runtime_error("NrTool: cannot open debug dump file in " + dir.string());
    }
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

NrToolSession::PreviewResult NrToolSession::preview(std::int64_t frameIndex, const NrSettings& settings,
                                                    const FgSettings& fg) {
    m_settings = settings;
    // Same reset rule as evaluateFrame/evaluateFg — captured before it updates m_lastFrame.
    const bool reset = (m_lastFrame < 0) || (frameIndex != m_lastFrame + 1);
    const auto t0 = std::chrono::steady_clock::now();

    const bool needGuidance = settings.enabled || fg.enabled;
    evaluateFrame(frameIndex, settings.enabled, needGuidance);

    // Readback the processed color BEFORE FG runs — evaluateFg re-layouts
    // m_nrOut to SHADER_READ. NR out is GENERAL, the raw color SHADER_READ.
    auto rgba8 = m_settings.enabled
        ? m_ctx->downloadImageRaw(m_nrOut->image, m_info.width, m_info.height, 4,
                                  VK_IMAGE_LAYOUT_GENERAL, /*restoreLayout=*/true)
        : m_ctx->downloadImageRaw(m_colorIn->image, m_info.width, m_info.height, 4,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, /*restoreLayout=*/true);
    auto jpeg = encodePreviewJpeg(m_info.width, m_info.height, rgba8, 80);

    std::vector<PooledTextureHandle> interps;
    if (fg.enabled) {
        interps = evaluateFg(frameIndex, reset, fg);
    }

    PreviewResult r;
    r.jpeg         = std::move(jpeg);
    r.width        = m_info.width;
    r.height       = m_info.height;
    if (fg.enabled && !reset && !interps.empty()) {
        // FG interp frames are A2B10 — convert via rgba16f bytes for JPEG.
        const std::size_t px = static_cast<std::size_t>(m_info.width) * m_info.height;
        auto f16 = m_ctx->downloadImageA2B10ToRgba16f(interps[0]->image, m_info.width,
                                                      m_info.height, VK_IMAGE_LAYOUT_GENERAL);
        r.interpJpeg = encodePreviewJpeg(m_info.width, m_info.height,
                                         rgba16fBytesToRgba8(f16, px), 80);
    }
    for (auto& h : interps) m_pool.release(h);
    r.evalMs       = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    r.resetApplied = reset;
    return r;
}

void NrToolSession::exportTo(const NrSettings& settings,
                             const FgSettings& fg,
                             OutputFormat format,
                             const std::filesystem::path& outputPath,
                             const std::filesystem::path& outputDir,
                             const std::string& templateName,
                             const std::function<void(float)>& onProgress,
                             std::atomic<bool>* stop) {
    m_settings = settings;

    const bool isVideo = format == OutputFormat::ProRes422HQ || format == OutputFormat::H265Mp4;
    if (!isVideo && format != OutputFormat::Png16) {
        throw std::runtime_error(
            "NrTool export: unsupported format — use prores_422hq, h265_hevc or png_16 "
            "(EXR is scene-linear; NR/FG need the display-domain export)");
    }
    if (fg.enabled && (fg.multiplier < 2 || fg.multiplier > 6)) {
        throw std::runtime_error("NrTool export: FG multiplier must be 2..6");
    }

    const uint32_t outTotal = outputFrameCount(fg);
    const double srcFps = m_info.fps > 0 ? m_info.fps : 30.0;
    const double outFps = fg.enabled ? srcFps * fg.multiplier : srcFps;

    // PNG16: write per-frame via OIIO (pixels are display-ready; plain 16-bit
    // upscale, no sRGB re-encode — mirrors the color-source read exactly).
    int pngIdx = 0;
    auto writePng = [&](const std::vector<std::uint8_t>& rgba8) {
        ++pngIdx;
        std::filesystem::path p = outputDir /
            AsyncFrameWriter::formatPath(templateName, pngIdx);
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
        std::vector<std::uint16_t> rgba16(static_cast<std::size_t>(m_info.width) * m_info.height * 4);
        for (std::size_t i = 0; i < rgba16.size(); ++i) {
            rgba16[i] = static_cast<std::uint16_t>(rgba8[i] << 8);
        }
        OIIO::ImageSpec spec(static_cast<int>(m_info.width), static_cast<int>(m_info.height),
                             4, OIIO::TypeDesc::UINT16);
        spec.channelnames = {"R", "G", "B", "A"};
        auto out = OIIO::ImageOutput::create(p.string());
        if (!out || !out->open(p.string(), spec)) {
            throw std::runtime_error("NrTool export: cannot open " + p.string());
        }
        if (!out->write_image(OIIO::TypeDesc::UINT16, rgba16.data())) {
            const std::string err = out->geterror();
            out->close();
            throw std::runtime_error("NrTool export: write failed: " + p.string() +
                                     (err.empty() ? "" : ": " + err));
        }
        out->close();
    };

    // Display-ready writer for the FG path (A2B10 real + interp frames).
    std::unique_ptr<ProResProgressWriter> prores;
    if (isVideo) {
        prores = std::make_unique<ProResProgressWriter>(
            outputPath, m_info.width, m_info.height,
            static_cast<int>(outFps),
            /*isHdr=*/false, PixelEncoding::DisplayReady,
            /*hdrMasteringNits=*/100.0f,
            /*queueHighWatermark=*/8, /*queueLowWatermark=*/4,
            /*encoderThreadCount=*/4,
            static_cast<int>(outTotal),
            [&onProgress](int cur, int total) {
                if (onProgress && total > 0) onProgress(static_cast<float>(cur) / total);
            },
            format == OutputFormat::H265Mp4 ? VideoCodec::H265Hevc : VideoCodec::ProRes422HQ,
            H265EncoderOptions{});
    }
    auto writeDisplay = [&](const PooledTextureHandle& tex, VkImageLayout layout) {
        if (isVideo) {
            prores->enqueuePackedYuv422Frame(m_proresConvert->convertAndReadback(
                tex, PixelEncoding::DisplayReady, /*hdr=*/false, layout));
        } else {
            const std::size_t px = static_cast<std::size_t>(m_info.width) * m_info.height;
            auto f16 = m_ctx->downloadImageA2B10ToRgba16f(tex->image, m_info.width,
                                                          m_info.height, layout);
            writePng(rgba16fBytesToRgba8(f16, px));
        }
    };

    uint32_t outIdx = 0;
    // Progress = frames actually written: video via the ProRes writer's encode
    // callback (fires on the encoder thread), PNG after each synchronous write.
    // Also reporting the enqueue counter would run the GUI bar back and forth
    // between two paces (the enqueue side runs up to the queue watermark ahead).
    auto tickOutput = [&]() {
        ++outIdx;
        if (!isVideo && onProgress && outTotal > 0)
            onProgress(static_cast<float>(outIdx) / outTotal);
    };

    for (uint32_t i = 0; i < m_info.frames; ++i) {
        if (stop && stop->load(std::memory_order_acquire)) {
            throw std::runtime_error("NrTool export: cancelled");
        }
        const std::int64_t frame = static_cast<std::int64_t>(i);
        const bool reset = (m_lastFrame < 0) || (frame != m_lastFrame + 1);
        const bool needGuidance = settings.enabled || fg.enabled;
        evaluateFrame(frame, settings.enabled, needGuidance);

        if (fg.enabled) {
            auto interps = evaluateFg(frame, reset, fg);
            // Write A, generated(A->B), B — the first eval warms FG history.
            if (i > 0) {
                for (const auto& h : interps) {
                    writeDisplay(h, VK_IMAGE_LAYOUT_GENERAL);
                    tickOutput();
                }
            }
            writeDisplay(m_displayNr, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            tickOutput();
            for (auto& h : interps) m_pool.release(h);
        } else if (isVideo) {
            // colorSrc (SHADER_READ) → unpack → displayNr GENERAL → YUV pack.
            const PooledTextureHandle& colorSrc = settings.enabled ? m_nrOut : m_colorIn;
            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(m_cmdPool);
            if (settings.enabled) {
                barrierGeneralToShaderRead(cmd, m_nrOut);
            }
            m_nrUnpack->execute(cmd, colorSrc, m_displayNr);
            m_ctx->endOneTimeCommands(m_cmdPool, cmd, m_ctx->computeQueue());

            // Sampler reads displayNr in GENERAL; convertAndReadback leaves
            // it layout-intact for the next iteration's UNDEFINED discard.
            prores->enqueuePackedYuv422Frame(m_proresConvert->convertAndReadback(
                m_displayNr, PixelEncoding::DisplayReady, /*hdr=*/false,
                VK_IMAGE_LAYOUT_GENERAL));
            tickOutput();
        } else {
            auto rgba8 = settings.enabled
                ? m_ctx->downloadImageRaw(m_nrOut->image, m_info.width, m_info.height, 4,
                                          VK_IMAGE_LAYOUT_GENERAL, /*restoreLayout=*/true)
                : m_ctx->downloadImageRaw(m_colorIn->image, m_info.width, m_info.height, 4,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                          /*restoreLayout=*/true);
            writePng(rgba8);
            tickOutput();
        }

        LOG_INFO("[NrTool] export output {}/{}", outIdx, outTotal);
    }

    if (prores) prores->close();
    m_lastFrame = -1;  // next preview restarts temporal state
}

} // namespace dlss
