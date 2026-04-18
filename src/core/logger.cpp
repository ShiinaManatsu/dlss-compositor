#include "core/logger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::mutex g_mutex;
FILE* g_logFile = nullptr;
std::string g_logPath;

std::filesystem::path getExeDir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return fs::path(buf).parent_path();
    }
#endif
    return fs::current_path();
}

/// Write current timestamp "[YYYY-MM-DD HH:MM:SS] " into the log file.
void writeTimestamp() {
    if (g_logFile == nullptr) return;
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &tt);
#else
    localtime_r(&tt, &local_tm);
#endif
    std::fprintf(g_logFile, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
}

} // anonymous namespace

bool Log::init() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr) return true; // already initialized

    namespace fs = std::filesystem;
    try {
        const fs::path logDir = getExeDir() / "log";

        // Clear previous logs.
        if (fs::exists(logDir)) {
            for (auto& entry : fs::directory_iterator(logDir)) {
                try { fs::remove(entry); } catch (...) {}
            }
        } else {
            fs::create_directories(logDir);
        }

        const fs::path logFile = logDir / "dlss-compositor.log";
        g_logPath = logFile.string();

#ifdef _WIN32
        g_logFile = _wfopen(logFile.wstring().c_str(), L"w");
#else
        g_logFile = std::fopen(g_logPath.c_str(), "w");
#endif
        if (g_logFile == nullptr) return false;

        // Header line.
        writeTimestamp();
        std::fprintf(g_logFile, "=== dlss-compositor log started ===\n");
        std::fflush(g_logFile);
        return true;
    } catch (...) {
        return false;
    }
}

void Log::shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr) {
        writeTimestamp();
        std::fprintf(g_logFile, "=== dlss-compositor log ended ===\n");
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void Log::error(const char* fmt, ...) {
    va_list args;

    // stderr
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);

    // log file
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr) {
        writeTimestamp();
        va_start(args, fmt);
        std::vfprintf(g_logFile, fmt, args);
        va_end(args);
        std::fflush(g_logFile);
    }
}

void Log::info(const char* fmt, ...) {
    va_list args;

    // stdout
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fflush(stdout);

    // log file
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr) {
        writeTimestamp();
        va_start(args, fmt);
        std::vfprintf(g_logFile, fmt, args);
        va_end(args);
        std::fflush(g_logFile);
    }
}

void Log::vwrite(FILE* stream, const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    std::vfprintf(stream, fmt, copy);
    va_end(copy);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_logFile != nullptr) {
        writeTimestamp();
        va_copy(copy, args);
        std::vfprintf(g_logFile, fmt, copy);
        va_end(copy);
        std::fflush(g_logFile);
    }
}
