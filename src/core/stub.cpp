// =============================================================================
// dlss_engine_lib stub.
//
// This file exists solely to give CMake a non-empty translation unit so the
// STATIC library target builds before later tasks (T4..T30) populate the real
// engine sources (src/core, src/gpu, src/dlss, src/io, src/rpc, src/pipeline).
//
// Once any real .cpp is added to dlss_engine_lib this file may be removed.
// =============================================================================

namespace dlss::detail {

// Anchor symbol — keeps the object non-empty without exporting anything.
int engine_lib_stub_anchor() noexcept { return 0; }

} // namespace dlss::detail
