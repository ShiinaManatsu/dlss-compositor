#include <catch2/catch_test_macros.hpp>

#include "core/ndjson_emitter.h"

#include <thread>
#include <vector>

TEST_CASE("NdjsonEmitter is disabled by default", "[ndjson]") {
    NdjsonEmitter emitter;
    REQUIRE_FALSE(emitter.isEnabled());
}

TEST_CASE("NdjsonEmitter enable/disable round-trip", "[ndjson]") {
    NdjsonEmitter emitter;
    emitter.setEnabled(true);
    REQUIRE(emitter.isEnabled());
    emitter.setEnabled(false);
    REQUIRE_FALSE(emitter.isEnabled());
}

TEST_CASE("NdjsonEmitter::emitRaw is a no-op when disabled", "[ndjson]") {
    NdjsonEmitter emitter;
    // Should neither crash nor produce output. We can't easily intercept stdout
    // here, but exercising the disabled path catches mutex/state regressions.
    emitter.emitRaw("{\"event\":\"should_not_appear\"}");
    REQUIRE_FALSE(emitter.isEnabled());
}

TEST_CASE("NdjsonEmitter is thread-safe across concurrent toggles", "[ndjson]") {
    NdjsonEmitter emitter;
    constexpr int kThreads = 8;
    constexpr int kIterations = 200;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&emitter, t, kIterations] {
            for (int i = 0; i < kIterations; ++i) {
                emitter.setEnabled((t + i) % 2 == 0);
                (void)emitter.isEnabled();
                // Disabled-path emitRaw should be safe under contention.
                emitter.emitRaw("{\"event\":\"thread_test\"}");
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    // Final state is non-deterministic; just assert we didn't deadlock or crash.
    SUCCEED("Concurrent NdjsonEmitter access completed without races/crashes");
}

TEST_CASE("globalNdjsonEmitter returns the same singleton instance", "[ndjson]") {
    NdjsonEmitter& a = globalNdjsonEmitter();
    NdjsonEmitter& b = globalNdjsonEmitter();
    REQUIRE(&a == &b);

    // Don't leave the singleton enabled — other tests rely on stdout being clean.
    a.setEnabled(false);
    REQUIRE_FALSE(b.isEnabled());
}
