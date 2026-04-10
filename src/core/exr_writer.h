#pragma once

#include <memory>
#include <string>

class ExrWriter {
public:
    ExrWriter();
    ~ExrWriter();

    bool create(const std::string& path, int width, int height, std::string& errorMsg);
    bool addChannel(const std::string& name, const float* data);
    bool write(std::string& errorMsg);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
