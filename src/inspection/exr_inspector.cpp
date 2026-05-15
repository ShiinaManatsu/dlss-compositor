#include "inspection/exr_inspector.h"

#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfHeader.h>
#include <ImfMultiPartInputFile.h>

#include <exception>
#include <sstream>

namespace {

const char* compressionName(Imf::Compression c) {
    switch (c) {
        case Imf::NO_COMPRESSION:    return "NONE";
        case Imf::RLE_COMPRESSION:   return "RLE";
        case Imf::ZIPS_COMPRESSION:  return "ZIPS";
        case Imf::ZIP_COMPRESSION:   return "ZIP";
        case Imf::PIZ_COMPRESSION:   return "PIZ";
        case Imf::PXR24_COMPRESSION: return "PXR24";
        case Imf::B44_COMPRESSION:   return "B44";
        case Imf::B44A_COMPRESSION:  return "B44A";
        case Imf::DWAA_COMPRESSION:  return "DWAA";
        case Imf::DWAB_COMPRESSION:  return "DWAB";
        default:                     return "UNKNOWN";
    }
}

std::string escapeJsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

} // namespace

bool inspectExr(const std::filesystem::path& file, ExrMetadata& out, std::string& errorMsg) {
    out = ExrMetadata{};

    try {
        if (!std::filesystem::exists(file)) {
            errorMsg = "File does not exist: " + file.string();
            return false;
        }

        Imf::MultiPartInputFile mpFile(file.string().c_str());
        const int partCount = mpFile.parts();

        out.isMultiPart = (partCount > 1);

        // Read first part for width/height/compression metadata.
        const Imf::Header& header = mpFile.header(0);
        const Imath::Box2i dataWindow = header.dataWindow();
        out.width  = dataWindow.max.x - dataWindow.min.x + 1;
        out.height = dataWindow.max.y - dataWindow.min.y + 1;
        out.compression = compressionName(header.compression());

        // Collect channels from all parts.
        for (int i = 0; i < partCount; ++i) {
            const Imf::Header& partHeader = mpFile.header(i);
            const Imf::ChannelList& channels = partHeader.channels();
            for (auto it = channels.begin(); it != channels.end(); ++it) {
                out.channels.emplace_back(it.name());
            }
        }

        errorMsg.clear();
        return true;
    } catch (const std::exception& ex) {
        errorMsg = ex.what();
        return false;
    }
}

std::string toInspectJson(const ExrMetadata& meta) {
    std::ostringstream s;
    s << "{\"width\":" << meta.width
      << ",\"height\":" << meta.height
      << ",\"channels\":[";
    for (size_t i = 0; i < meta.channels.size(); ++i) {
        if (i > 0) s << ",";
        s << "\"" << escapeJsonString(meta.channels[i]) << "\"";
    }
    s << "],\"compression\":\"" << escapeJsonString(meta.compression) << "\""
      << ",\"is_multi_part\":" << (meta.isMultiPart ? "true" : "false")
      << "}";
    return s.str();
}
