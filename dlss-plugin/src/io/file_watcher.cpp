// FileWatcher implementation — Windows ReadDirectoryChangesW.
// Windows headers MUST come before file_watcher.h (which forward-declares HANDLE).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "file_watcher.h"
#include "../core/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace dlss {

namespace {

std::string win32Msg(DWORD err) {
    char buf[512] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
    // Strip trailing \r\n
    auto len = std::strlen(buf);
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) buf[--len] = '\0';
    return buf;
}

} // namespace

FileWatcher::FileWatcher(FileWatcherConfig cfg, FileWatcherCallbacks cbs)
    : m_cfg(std::move(cfg)), m_cbs(std::move(cbs))
    , m_dirHandle(INVALID_HANDLE_VALUE), m_stopEvent(INVALID_HANDLE_VALUE)
{
    if (!std::filesystem::is_directory(m_cfg.directory)) {
        throw std::runtime_error(
            "FileWatcher: directory does not exist: " + m_cfg.directory.string());
    }

    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (m_stopEvent == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("FileWatcher: CreateEvent failed: " + win32Msg(GetLastError()));
    }

    m_dirHandle = CreateFileW(
        m_cfg.directory.wstring().c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (m_dirHandle == INVALID_HANDLE_VALUE) {
        CloseHandle(m_stopEvent);
        throw std::runtime_error(
            "FileWatcher: cannot open directory: " + win32Msg(GetLastError()));
    }

    m_running.store(true, std::memory_order_release);
    m_worker = std::thread([this]{ workerLoop(); });
    LOG_INFO("[FileWatcher] Watching {} for *{}", m_cfg.directory.string(), m_cfg.extension);
    scanExisting();
}

FileWatcher::~FileWatcher() {
    stop();
    if (m_worker.joinable()) m_worker.join();
    if (m_dirHandle  != INVALID_HANDLE_VALUE) CloseHandle(m_dirHandle);
    if (m_stopEvent  != INVALID_HANDLE_VALUE) CloseHandle(m_stopEvent);
}

void FileWatcher::stop() {
    m_stop.store(true, std::memory_order_release);
    if (m_stopEvent != INVALID_HANDLE_VALUE) SetEvent(m_stopEvent);
}

bool FileWatcher::isTarget(const std::wstring& name) const {
    if (name.size() < m_cfg.extension.size()) return false;
    // Convert wide filename to narrow for suffix comparison.
    // Use WideCharToMultiByte for proper conversion (avoids C4244 narrowing).
    const int needed = WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return false;
    std::string narrow(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, narrow.data(), needed, nullptr, nullptr);
    return narrow.size() >= m_cfg.extension.size() &&
           narrow.substr(narrow.size() - m_cfg.extension.size()) == m_cfg.extension;
}

bool FileWatcher::markSeen(const std::wstring& name) {
    std::lock_guard<std::mutex> lk(m_seenMu);
    return m_seen.insert(name).second;
}

void FileWatcher::scanExisting() {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(m_cfg.directory, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (m_stop.load(std::memory_order_acquire)) return;
        if (!it->is_regular_file(ec)) continue;
        const std::filesystem::path path = it->path();
        const std::wstring name = path.filename().wstring();
        if (!isTarget(name)) continue;
        waitForStability(path);
        if (m_stop.load(std::memory_order_acquire) || !std::filesystem::exists(path)) continue;
        if (!markSeen(name)) continue;
        FileEvent ev;
        ev.path = path;
        ev.stem = path.stem().string();
        ev.deleted = false;
        if (m_cbs.onFile) m_cbs.onFile(ev);
        m_delivered.fetch_add(1, std::memory_order_relaxed);
        if (!m_cfg.keepFiles) {
            std::error_code rec;
            std::filesystem::remove(path, rec);
            if (rec)
                LOG_WARN("[FileWatcher] Failed to delete {}: {}", path.string(), rec.message());
        }
    }
}

void FileWatcher::waitForStability(const std::filesystem::path& path) const {
    if (m_cfg.stabilityMs <= 0) return;
    // Poll file size until stable for stabilityMs.
    auto lastSize = static_cast<uintmax_t>(-1);
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(m_cfg.stabilityMs * 4); // max wait 4x
    while (!m_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_cfg.stabilityMs));
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) break; // file gone or unreadable
        if (size == lastSize) break; // stable
        lastSize = size;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }
}

void FileWatcher::workerLoop() {
    const int bufSize = std::max(m_cfg.bufferSize, 4096);
    std::vector<BYTE> buf(static_cast<size_t>(bufSize));

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == INVALID_HANDLE_VALUE) {
        if (m_cbs.onError) m_cbs.onError("FileWatcher: CreateEvent for OVERLAPPED failed");
        m_running.store(false, std::memory_order_release);
        return;
    }

    HANDLE waitHandles[2] = { ov.hEvent, m_stopEvent };

    while (!m_stop.load(std::memory_order_acquire)) {
        ResetEvent(ov.hEvent);
        DWORD returned = 0;
        const BOOL ok = ReadDirectoryChangesW(
            m_dirHandle,
            buf.data(), static_cast<DWORD>(buf.size()),
            FALSE,  // not subtree
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            &returned,
            &ov,
            nullptr);

        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Normal async path — fall through to WaitForMultipleObjects.
            } else {
                if (m_cbs.onError)
                    m_cbs.onError("FileWatcher: ReadDirectoryChangesW failed: " + win32Msg(err));
                break;
            }
        }

        // Wait for a change notification or stop signal.
        const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (waitResult == WAIT_OBJECT_0 + 1 || m_stop.load(std::memory_order_acquire)) {
            CancelIo(m_dirHandle);
            break; // stop event
        }
        if (waitResult != WAIT_OBJECT_0) {
            if (m_cbs.onError)
                m_cbs.onError("FileWatcher: WaitForMultipleObjects error: " + win32Msg(GetLastError()));
            break;
        }

        if (!GetOverlappedResult(m_dirHandle, &ov, &returned, FALSE) || returned == 0) {
            continue; // overflow or error — re-issue
        }

        // Walk the notification records.
        const BYTE* ptr = buf.data();
        while (ptr < buf.data() + returned) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(ptr);
            const std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));

            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_MODIFIED) {
                if (isTarget(name) && !m_stop.load(std::memory_order_acquire)) {
                    const std::filesystem::path path = m_cfg.directory / name;
                    waitForStability(path);

                    if (!m_stop.load(std::memory_order_acquire) && std::filesystem::exists(path)
                        && markSeen(name)) {
                        FileEvent ev;
                        ev.path = path;
                        ev.stem = path.stem().string();
                        ev.deleted = false;

                        if (m_cbs.onFile) m_cbs.onFile(ev);
                        m_delivered.fetch_add(1, std::memory_order_relaxed);

                        // Delete after callback unless keepFiles is set.
                        if (!m_cfg.keepFiles) {
                            std::error_code ec;
                            std::filesystem::remove(path, ec);
                            if (ec)
                                LOG_WARN("[FileWatcher] Failed to delete {}: {}", path.string(), ec.message());
                            else
                                LOG_DEBUG("[FileWatcher] Deleted {}", path.string());
                        }
                    }
                }
            } else if (info->Action == FILE_ACTION_REMOVED) {
                if (isTarget(name) && m_cbs.onExternalDelete) {
                    m_cbs.onExternalDelete(m_cfg.directory / name);
                }
            }

            if (info->NextEntryOffset == 0) break;
            ptr += info->NextEntryOffset;
        }
    }

    CloseHandle(ov.hEvent);
    m_running.store(false, std::memory_order_release);
    LOG_INFO("[FileWatcher] Stopped. {} files delivered.", m_delivered.load());
}

} // namespace dlss
