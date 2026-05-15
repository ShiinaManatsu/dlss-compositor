#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ExrMetadata {
    int width = 0;
    int height = 0;
    std::vector<std::string> channels;
    std::string compression;  // "ZIP", "PIZ", "DWAA", "NONE", etc.
    bool isMultiPart = false;
};

// Reads only the header of the EXR file. Does NOT load pixel data.
bool inspectExr(const std::filesystem::path& file, ExrMetadata& out, std::string& errorMsg);

// Converts ExrMetadata to a compact JSON string.
// Example: {"width":1920,"height":1080,"channels":["R","G","B","A","Z"],"compression":"ZIP","is_multi_part":false}
std::string toInspectJson(const ExrMetadata& meta);
