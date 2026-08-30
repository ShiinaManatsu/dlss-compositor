// NrToolSession implementation — see nr_tool_session.h.

#include "nr_tool_session.h"

#include "../core/log.h"
#include "../io/async_writer.h"
#include "../io/prores_writer.h"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
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

} // namespace

NrToolSession::NrToolSession(VulkanContext* ctx, NgxContext* ngx,
                             std::filesystem::path colorSource,
                             std::filesystem::path bufferDir)
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

    // ── Guidance: dlssnrbuffer sequence (required — real guidance, no fallback)
    m_buffer.open(bufferDir);
    m_info.bufferDir    = bufferDir.string();
    m_info.bufferWidth  = m_buffer.width();
    m_info.bufferHeight = m_buffer.height();

    if (m_info.frames == 0) {
        throw std::runtime_error("NrTool: color source frame count unknown — "
                                 "dlssnrbuffer has " + std::to_string(m_buffer.frames()));
    }
    if (m_info.frames != m_buffer.frames()) {
        throw std::runtime_error(
            "NrTool: frame count mismatch — color has " + std::to_string(m_info.frames) +
            ", dlssnrbuffer has " + std::to_string(m_buffer.frames()) +
            ". Re-export the buffer with the same frame range.");
    }
}

NrToolSession::~NrToolSession() {
    LOG_INFO("[NrTool] session teardown begin");
    m_nrPass.reset();       // feature 18 shutdown first (snippet still loaded)
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
    const TextureDesc tdMv       {m_info.bufferWidth, m_info.bufferHeight, VK_FORMAT_R16G16_SFLOAT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    const TextureDesc tdDepth    {m_info.bufferWidth, m_info.bufferHeight, VK_FORMAT_R32_SFLOAT,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    const VkImageUsageFlags storageRw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                                      | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const TextureDesc tdNrOut    {m_info.width, m_info.height, VK_FORMAT_R8G8B8A8_UNORM, storageRw};
    const TextureDesc tdDisplayNr{m_info.width, m_info.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storageRw};

    m_colorIn  = m_pool.acquire(tdColorIn);
    m_mvTex    = m_pool.acquire(tdMv);
    m_depthTex = m_pool.acquire(tdDepth);
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

void NrToolSession::evaluateFrame(std::int64_t frameIndex) {
    ensureCreated();
    LOG_DEBUG("[NrTool] eval frame {} begin", frameIndex);

    // ── Decode color ──────────────────────────────────────────────────────────
    std::vector<std::uint8_t> colorRgba8;
    if (m_isVideo) {
        m_video.readFrameRgba8(frameIndex, colorRgba8);
    } else {
        uint32_t w = 0, h = 0;
        colorRgba8 = loadPng16AsRgba8(m_pngFiles[static_cast<std::size_t>(frameIndex)], w, h);
    }

    // ── Decode guidance ───────────────────────────────────────────────────────
    std::vector<std::uint8_t> mvBytes;
    std::vector<float>        depth;
    m_buffer.readFrame(static_cast<std::size_t>(frameIndex), mvBytes, depth);

    // ── Upload (NGX contract: inputs SHADER_READ) ─────────────────────────────
    UploadBatch batch(m_ctx);
    batch.addUpload(m_colorIn,  colorRgba8.data(), colorRgba8.size());
    batch.addUpload(m_mvTex,    mvBytes.data(),    mvBytes.size());
    batch.addUpload(m_depthTex, depth.data(),      depth.size() * sizeof(float));
    batch.submit();

    // ── Evaluate ──────────────────────────────────────────────────────────────
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
    in.mvView       = m_mvTex->view;
    in.mvImage      = m_mvTex->image;
    in.depthView    = m_depthTex->view;
    in.depthImage   = m_depthTex->image;
    in.renderWidth  = m_info.bufferWidth;
    in.renderHeight = m_info.bufferHeight;
    in.reset        = reset;
    m_nrPass->updateSettings(m_settings);
    m_nrPass->evaluate(cmd, in);
    m_ctx->endOneTimeCommands(m_cmdPool, cmd, m_ctx->computeQueue());
    // Pipeline parity: NGX eval may run on the snippet's own internal queue —
    // drain the device before any readback (downloadImageRaw submits its own).
    m_ctx->waitIdle();
    LOG_DEBUG("[NrTool] eval frame {} done (reset={})", frameIndex, reset);

    m_lastFrame = frameIndex;
}

NrToolSession::PreviewResult NrToolSession::preview(std::int64_t frameIndex, const NrSettings& settings) {
    m_settings = settings;
    // Same reset rule as evaluateFrame — captured before it updates m_lastFrame.
    const bool reset = (m_lastFrame < 0) || (frameIndex != m_lastFrame + 1);
    const auto t0 = std::chrono::steady_clock::now();

    evaluateFrame(frameIndex);

    // Readback RGBA8 from nrOut (still GENERAL; restore keeps it GENERAL).
    auto rgba8 = m_ctx->downloadImageRaw(m_nrOut->image, m_info.width, m_info.height, 4,
                                         VK_IMAGE_LAYOUT_GENERAL, /*restoreLayout=*/true);
    auto jpeg = encodePreviewJpeg(m_info.width, m_info.height, rgba8, 80);

    PreviewResult r;
    r.jpeg         = std::move(jpeg);
    r.width        = m_info.width;
    r.height       = m_info.height;
    r.evalMs       = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count();
    r.resetApplied = reset;
    return r;
}

void NrToolSession::exportTo(const NrSettings& settings,
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
            "(EXR is scene-linear; NR needs the display-domain export)");
    }

    // PNG16: write per-frame via OIIO (pixels are display-ready; plain 16-bit
    // upscale, no sRGB re-encode — mirrors the color-source read exactly).
    auto writePng = [&](const std::vector<std::uint8_t>& rgba8, int frameIdx1Based) {
        std::filesystem::path p = outputDir /
            AsyncFrameWriter::formatPath(templateName, frameIdx1Based);
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

    std::unique_ptr<ProResProgressWriter> prores;
    if (isVideo) {
        prores = std::make_unique<ProResProgressWriter>(
            outputPath, m_info.width, m_info.height,
            m_info.fps > 0 ? static_cast<int>(m_info.fps) : 30,
            /*isHdr=*/false, PixelEncoding::DisplayReady,
            /*hdrMasteringNits=*/100.0f,
            /*queueHighWatermark=*/8, /*queueLowWatermark=*/4,
            /*encoderThreadCount=*/4,
            static_cast<int>(m_info.frames),
            [&onProgress](int cur, int total) {
                if (onProgress && total > 0) onProgress(static_cast<float>(cur) / total);
            },
            format == OutputFormat::H265Mp4 ? VideoCodec::H265Hevc : VideoCodec::ProRes422HQ,
            H265EncoderOptions{});
    }

    for (uint32_t i = 0; i < m_info.frames; ++i) {
        if (stop && stop->load(std::memory_order_acquire)) {
            throw std::runtime_error("NrTool export: cancelled");
        }
        evaluateFrame(static_cast<std::int64_t>(i));

        if (isVideo) {
            // nrOut GENERAL → SHADER_READ → unpack → displayNr GENERAL → YUV pack.
            VkCommandBuffer cmd = m_ctx->beginOneTimeCommands(m_cmdPool);
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = m_nrOut->image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cmd, &dep);
            m_nrUnpack->execute(cmd, m_nrOut, m_displayNr);
            m_ctx->endOneTimeCommands(m_cmdPool, cmd, m_ctx->computeQueue());

            // Sampler reads displayNr in SHADER_READ; convertAndReadback leaves
            // it layout-intact for the next iteration's UNDEFINED discard.
            auto packed = m_proresConvert->convertAndReadback(
                m_displayNr, PixelEncoding::DisplayReady, /*hdr=*/false,
                VK_IMAGE_LAYOUT_GENERAL);
            prores->enqueuePackedYuv422Frame(std::move(packed));
        } else {
            auto rgba8 = m_ctx->downloadImageRaw(m_nrOut->image, m_info.width, m_info.height, 4,
                                                 VK_IMAGE_LAYOUT_GENERAL, /*restoreLayout=*/true);
            writePng(rgba8, static_cast<int>(i) + 1);
        }

        if (onProgress) onProgress(static_cast<float>(i + 1) / static_cast<float>(m_info.frames));
        LOG_INFO("[NrTool] export frame {}/{}", i + 1, m_info.frames);
    }

    if (prores) prores->close();
    m_lastFrame = -1;  // next preview restarts temporal state
}

} // namespace dlss
