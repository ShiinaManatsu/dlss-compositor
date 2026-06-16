#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iterator>
#include <string>

#include <spdlog/spdlog.h>

#include "core/log.h"

namespace {

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

class StreamRedirect {
public:
    StreamRedirect(FILE* stream, const std::filesystem::path& path)
        : stream_(stream), path_(path), originalFd_(_dup(_fileno(stream))) {
        std::fflush(stream_);

        if (originalFd_ < 0) {
            throw std::runtime_error("failed to duplicate stream");
        }

        FILE* redirected = nullptr;
        const auto fileName = path_.string();
        if (freopen_s(&redirected, fileName.c_str(), "w", stream_) != 0 || redirected == nullptr) {
            _close(originalFd_);
            throw std::runtime_error("failed to redirect stream");
        }
    }

    ~StreamRedirect() {
        std::fflush(stream_);
        if (originalFd_ >= 0) {
            _dup2(originalFd_, _fileno(stream_));
            _close(originalFd_);
        }
    }

private:
    FILE* stream_;
    std::filesystem::path path_;
    int originalFd_;
};

}  // namespace

TEST_CASE("spdlog - stdout clean", "[log]") {
    const auto tempDir = std::filesystem::temp_directory_path();
    const auto stdoutPath = tempDir / "dlss-log-stdout.txt";
    const auto stderrPath = tempDir / "dlss-log-stderr.txt";
    const auto logFilePath = std::filesystem::current_path() / "dlss-compositor-cli.log";
    constexpr auto kMessage = "test-msg-42";

    std::error_code ec;
    std::filesystem::remove(stdoutPath, ec);
    std::filesystem::remove(stderrPath, ec);
    std::filesystem::remove(logFilePath, ec);

    _putenv_s("DLSS_COMPOSITOR_LOG", "info");
    spdlog::drop("dlss");

    {
        StreamRedirect stdoutRedirect(stdout, stdoutPath);
        StreamRedirect stderrRedirect(stderr, stderrPath);

        dlss::log::init(true);
        LOG_INFO(kMessage);

        std::fflush(stdout);
        std::fflush(stderr);
    }

    spdlog::drop("dlss");

    const auto stdoutContent = readFile(stdoutPath);
    const auto stderrContent = readFile(stderrPath);
    const auto fileContent = readFile(logFilePath);

    REQUIRE(stdoutContent.empty());
    REQUIRE(stderrContent.find(kMessage) != std::string::npos);
    REQUIRE(fileContent.find(kMessage) != std::string::npos);
}
