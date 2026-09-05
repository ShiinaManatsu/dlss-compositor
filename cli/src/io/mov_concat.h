// mov_concat — stream-copy concatenation of same-codec video files.
//
// Segmented rendering produces one ProRes/H265 file per CLI instance; the
// final deliverable is their lossless concatenation. Every segment comes from
// our own writer with identical codec parameters and (for ProRes) all-intra
// GOPs, so packets can be copied verbatim between MOV/MP4 muxers — no decode,
// no re-encode, no quality loss.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dlss {

/// Concatenate `inputs` (in order) into `output` via packet stream copy.
/// Throws std::runtime_error with an actionable message on any mismatch
/// (missing file, codec/size/pix_fmt divergence, muxer failure).
/// Returns the total number of video frames written.
std::int64_t concatVideoFiles(const std::vector<std::filesystem::path>& inputs,
                              const std::filesystem::path& output);

} // namespace dlss
