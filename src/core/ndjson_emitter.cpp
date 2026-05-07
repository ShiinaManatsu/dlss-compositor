#include "core/ndjson_emitter.h"

#include <cstdio>

NdjsonEmitter& globalNdjsonEmitter() {
    static NdjsonEmitter instance;
    return instance;
}

void NdjsonEmitter::setEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_enabled = enabled;
}

bool NdjsonEmitter::isEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled;
}

void NdjsonEmitter::emitRaw(const std::string& jsonLine) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_enabled) {
        return;
    }
    // Single fwrite + newline keeps the line atomic across threads (we already
    // hold m_mutex). Using stdout (FILE*) so consumers can pipe.
    std::fwrite(jsonLine.data(), 1, jsonLine.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}
