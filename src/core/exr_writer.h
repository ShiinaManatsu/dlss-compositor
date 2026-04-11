#pragma once

#include <memory>
#include <string>

namespace Imf {
enum Compression;
}

enum class ExrCompression {
    None,
    Zip,
    Zips,
    Piz,
    Dwaa,
    Dwab
};

class ExrWriter {
public:
    ExrWriter();
    ~ExrWriter();

    bool create(const std::string& path, int width, int height, std::string& errorMsg);
    void setCompression(ExrCompression compression, float dwaQuality);
    bool addChannel(const std::string& name, const float* data);
    bool write(std::string& errorMsg);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
