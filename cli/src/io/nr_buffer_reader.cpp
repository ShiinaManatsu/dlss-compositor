// NrBufferReader implementation — see nr_buffer_reader.h.

#include "nr_buffer_reader.h"
#include "nr_buffer_video.h"

#include "../core/log.h"

#include <OpenImageIO/imageio.h>

#include <Imath/half.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <string>

namespace dlss {

namespace {

/// Extract the trailing frame number of "dlssnrbuffer_####.exr" for sorting.
int frameNumberOf(const std::filesystem::path& p) {
    const std::string stem = p.stem().string();
    const auto underscore = stem.rfind('_');
    if (underscore == std::string::npos) return -1;
    try {
        return std::stoi(stem.substr(underscore + 1));
    } catch (...) {
        return -1;
    }
}

} // namespace

void NrBufferReader::open(const std::filesystem::path& dir) {
    m_files.clear();
    m_video.close();

    const auto videoPath = std::filesystem::is_regular_file(dir) ? dir : dir / kNrBufferVideoName;
    if (std::filesystem::is_regular_file(videoPath)) {
        m_video.open(videoPath, /*nrGuidance=*/true);
        if (m_video.frameCount() <= 0) throw std::runtime_error("NrBufferReader: guide video frame count unknown");
        m_w = m_video.width();
        m_h = m_video.height() / kNrBufferPlanes;
        return;
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        throw std::runtime_error("NrBufferReader: not a directory: " + dir.string());
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".exr") continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("dlssnrbuffer_", 0) != 0) continue;
        if (frameNumberOf(entry.path()) < 0) continue;
        m_files.push_back(entry.path());
    }
    if (ec) {
        throw std::runtime_error("NrBufferReader: cannot list " + dir.string() + ": " + ec.message());
    }
    if (m_files.empty()) {
        throw std::runtime_error(
            "NrBufferReader: no dlssnrbuffer.mkv or legacy EXR frames in " + dir.string() +
            " — run the Stage-A export with the DLSSNR 缓冲 option enabled");
    }
    std::sort(m_files.begin(), m_files.end(), [](const auto& a, const auto& b) {
        return frameNumberOf(a) < frameNumberOf(b);
    });

    auto input = OIIO::ImageInput::open(m_files.front().string());
    if (!input) {
        throw std::runtime_error("NrBufferReader: cannot open " + m_files.front().string());
    }
    const OIIO::ImageSpec& spec = input->spec();
    if (spec.nchannels < 3) {
        throw std::runtime_error("NrBufferReader: expected 3 channels in " +
                                 m_files.front().string());
    }
    m_w = static_cast<uint32_t>(spec.width);
    m_h = static_cast<uint32_t>(spec.height);

    // Resolve channels by NAME, not index — defensive hardening. OpenEXR sorts
    // the ChannelList alphabetically (B,G,R) on disk, though OIIO presents
    // channels in write order, so plain indices 0/1/2 were correct here too;
    // name resolution stays correct regardless of any writer variance. (The
    // actual 0.5.5 artifact root cause was the UINT16 read in readFrame, not
    // channel order — that conversion destroyed the fp16 values themselves.)
    chMvX    = spec.channelindex("R");
    chMvY    = spec.channelindex("G");
    chDepth  = spec.channelindex("B");
    if (chMvX < 0 || chMvY < 0 || chDepth < 0) {
        throw std::runtime_error(
            "NrBufferReader: missing R/G/B channels in " + m_files.front().string() +
            " — not a dlssnrbuffer sequence (first channel: " +
            std::string(spec.channel_name(0)) + ")");
    }
    input->close();

    LOG_INFO("[NrBufferReader] {} frames {}x{} from {}",
             m_files.size(), m_w, m_h, dir.string());
}

void NrBufferReader::readFrame(std::size_t idx,
                               std::vector<std::uint8_t>& mvOut,
                               std::vector<float>&        depthOut) {
    if (!isOpen()) throw std::runtime_error("NrBufferReader: not open");
    if (m_video.isOpen()) {
        m_video.readNrGuidance(idx, mvOut, depthOut);
        return;
    }
    if (idx >= m_files.size()) throw std::runtime_error("NrBufferReader: frame index out of range");

    auto input = OIIO::ImageInput::open(m_files[idx].string());
    if (!input) {
        throw std::runtime_error("NrBufferReader: cannot open " + m_files[idx].string());
    }
    const OIIO::ImageSpec& spec = input->spec();
    const std::size_t px = static_cast<std::size_t>(spec.width) * spec.height;

    std::vector<std::uint16_t> halves(px * static_cast<std::size_t>(spec.nchannels));
    // Request HALF, not UINT16: OIIO's half→uint16 conversion is VALUE-domain
    // ([0,1] scaled to [0,65535], clamped) — reading these motion/depth bits as
    // UINT16 destroyed every value (depth 0.97→0xF2xx = f16 NaN; |mv|>1px clamps
    // to 0xFFFF = NaN). TypeDesc::HALF keeps the raw bit patterns verbatim.
    if (!input->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::HALF, halves.data())) {
        const std::string err = input->geterror();
        input->close();
        throw std::runtime_error("NrBufferReader: read failed: " + m_files[idx].string() +
                                 (err.empty() ? "" : ": " + err));
    }
    input->close();

    // R,G carry fp16 motion bits — copy verbatim. B is NDC depth — widen to f32.
    // Indices are name-resolved in open() (OIIO exposes write order: R,G,B).
    mvOut.resize(px * 2 * sizeof(std::uint16_t));
    auto* mvHalf = reinterpret_cast<std::uint16_t*>(mvOut.data());
    depthOut.resize(px);
    for (std::size_t i = 0; i < px; ++i) {
        const std::size_t s = i * static_cast<std::size_t>(spec.nchannels);
        mvHalf[i * 2 + 0] = halves[s + static_cast<std::size_t>(chMvX)];
        mvHalf[i * 2 + 1] = halves[s + static_cast<std::size_t>(chMvY)];
        Imath::half h;
        h.setBits(halves[s + static_cast<std::size_t>(chDepth)]);
        depthOut[i] = static_cast<float>(h);
    }
}

} // namespace dlss
