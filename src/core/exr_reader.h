#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct ExrChannel {
    std::string name;
    int pixelType;
    std::vector<float> data;
};

class ExrReader {
public:
    ExrReader();
    ~ExrReader();

    bool open(const std::string& path, std::string& errorMsg);
    void close();

    int width() const;
    int height() const;

    std::vector<std::string> listChannels() const;
    const float* readChannel(const std::string& name) const;
    std::vector<float> readRGBA(const std::string& r, const std::string& g,
                                const std::string& b, const std::string& a) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
