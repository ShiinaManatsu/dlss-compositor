#pragma once

#include <cstdarg>
#include <cstdio>

/// Minimal file logger that writes to <exe_dir>/log/.
/// All public functions are thread-safe.  Call Log::init() once at the start
/// of main() and Log::shutdown() before exit.
///
/// Log::error(fmt, ...) writes to **both** stderr and the log file.
/// Log::info(fmt, ...)  writes to **both** stdout and the log file.
/// This keeps the existing console behaviour unchanged while mirroring
/// everything into a persistent log file for post-mortem debugging.
namespace Log {

/// Create <exe_dir>/log/ directory (clearing any previous log files),
/// then open a fresh log file.  Returns false on failure (non-fatal —
/// the program will continue without file logging).
bool init();

/// Flush and close the log file.
void shutdown();

/// printf-style helpers that write to both a console stream and the log file.
void error(const char* fmt, ...);
void info(const char* fmt, ...);

/// Low-level: write a pre-formatted va_list to both \p stream and the log
/// file.  Useful when the caller already has a va_list.
void vwrite(FILE* stream, const char* fmt, va_list args);

} // namespace Log
