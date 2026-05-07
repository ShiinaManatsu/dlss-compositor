#pragma once

#include <mutex>
#include <string>

/// Thread-safe emitter for NDJSON (newline-delimited JSON) progress events.
///
/// When enabled, `emitRaw()` writes the supplied JSON line followed by a single
/// '\n' to stdout, then flushes. When disabled (the default), all calls to
/// `emitRaw()` are no-ops, so it is cheap to sprinkle calls throughout the
/// pipeline without conditional callsites (although callers may still want to
/// guard expensive JSON construction with `isEnabled()`).
///
/// Used in conjunction with the `--progress-format json` CLI flag, which
/// enables the global instance via `globalNdjsonEmitter()`.
class NdjsonEmitter {
public:
    NdjsonEmitter() = default;

    void setEnabled(bool enabled);
    bool isEnabled() const;

    /// Emit a single JSON object (caller must supply a complete, valid JSON
    /// object string with no embedded newlines). Appends '\n' and flushes
    /// stdout. Thread-safe.
    void emitRaw(const std::string& jsonLine);

private:
    mutable std::mutex m_mutex;
    bool m_enabled = false;
};

/// Process-wide singleton used by the pipeline. The emitter is disabled
/// until `setEnabled(true)` is called (typically once, in main(), when
/// `--progress-format json` is parsed).
NdjsonEmitter& globalNdjsonEmitter();
