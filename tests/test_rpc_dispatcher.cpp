// =============================================================================
// tests/test_rpc_dispatcher.cpp — RpcDispatcher unit tests (T31).
//
// Covers all 9 dlss/* methods plus the encoder-allowlist regression guard.
// =============================================================================
#include <catch2/catch_test_macros.hpp>

#include "../src/rpc/dispatcher.h"

#include <nlohmann/json.hpp>

#include <string>

TEST_CASE("RpcDispatcher - setTask returns task_id", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("setTask", {{"config", "test.json"}});
    REQUIRE(result["ok"] == true);
    REQUIRE(result.contains("task_id"));
    REQUIRE_FALSE(result["task_id"].get<std::string>().empty());
}

TEST_CASE("RpcDispatcher - checkReady false without task", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("checkReady", {});
    REQUIRE(result.contains("ready"));
    REQUIRE(result["ready"] == false);
}

TEST_CASE("RpcDispatcher - checkReady true after setTask", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    dispatcher.dispatch("setTask", {{"config", "test.json"}});
    auto result = dispatcher.dispatch("checkReady", {});
    REQUIRE(result["ready"] == true);
}

TEST_CASE("RpcDispatcher - getStatus returns correct video_encoders", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getStatus", {});

    REQUIRE(result.contains("video_encoders"));
    auto& encoders = result["video_encoders"];
    REQUIRE(encoders.size() == 1);
    REQUIRE(encoders[0].get<std::string>() == "prores_422hq");

    // Extension codecs must NOT be in the registered list.
    for (const auto& enc : encoders) {
        const std::string s = enc.get<std::string>();
        REQUIRE(s != "dnxhd");
        REQUIRE(s != "dnxhr");
        REQUIRE(s != "h264");
        REQUIRE(s != "h265");
        REQUIRE(s != "av1");
        REQUIRE(s != "avif");
        REQUIRE(s != "webp");
        REQUIRE(s != "jpeg");
    }
}

TEST_CASE("RpcDispatcher - getStatus LDR formats include png_16", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getStatus", {});
    REQUIRE(result.contains("export_formats_ldr"));
    auto& ldr = result["export_formats_ldr"];

    bool hasPng = false;
    for (const auto& f : ldr) {
        if (f.get<std::string>() == "png_16") hasPng = true;
    }
    REQUIRE(hasPng);
}

TEST_CASE("RpcDispatcher - getStatus HDR formats no png", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getStatus", {});
    REQUIRE(result.contains("export_formats_hdr"));
    auto& hdr = result["export_formats_hdr"];
    for (const auto& f : hdr) {
        REQUIRE(f.get<std::string>() != "png_16");
    }
}

TEST_CASE("RpcDispatcher - getStatus exposes LDR ProRes only", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getStatus", {});
    bool hasProRes = false;
    for (const auto& f : result["export_formats_ldr"]) {
        if (f.get<std::string>() == "prores_422hq") hasProRes = true;
    }
    REQUIRE(hasProRes);
    for (const auto& f : result["export_formats_hdr"]) {
        REQUIRE(f.get<std::string>() != "prores_422hq");
    }
}

TEST_CASE("RpcDispatcher - getProgress lock-free + idle by default", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getProgress", {});
    REQUIRE(result.contains("current_frame"));
    REQUIRE(result.contains("total_frames"));
    REQUIRE(result.contains("percent"));
    REQUIRE(result.contains("state"));
    REQUIRE(result["state"].get<int>() == 0);  // Idle
}

TEST_CASE("RpcDispatcher - getTaskState reflects state transitions", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto idle = dispatcher.dispatch("getTaskState", {});
    REQUIRE(idle["state"] == "idle");

    dispatcher.dispatch("setTask", {{"config", "x.json"}});
    dispatcher.dispatch("startTask", {});

    auto running = dispatcher.dispatch("getTaskState", {});
    REQUIRE(running["state"] == "running");
    REQUIRE(running["total_frames"].get<int>() == 100);
}

TEST_CASE("RpcDispatcher - startTask without setTask fails", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("startTask", {});
    REQUIRE(result["ok"] == false);
    REQUIRE(result.contains("error"));
}

TEST_CASE("RpcDispatcher - checkOutputCollisions returns zero initially", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("checkOutputCollisions",
        {{"output_dir", "/tmp"}, {"template", "frame_####.exr"}, {"frame_count", 10}});
    REQUIRE(result.contains("collisions"));
    REQUIRE(result["collisions"].get<int>() == 0);
    REQUIRE(result.contains("files"));
    REQUIRE(result["files"].is_array());
}

TEST_CASE("RpcDispatcher - getPreviewFrame returns metadata only", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("getPreviewFrame", {});
    REQUIRE(result.contains("ptr"));
    REQUIRE(result.contains("size"));
    REQUIRE(result.contains("width"));
    REQUIRE(result.contains("height"));
}

TEST_CASE("RpcDispatcher - setDebugMode returns not implemented", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("setDebugMode", {{"mode", "overlay"}});
    REQUIRE(result["ok"] == false);
    REQUIRE(result.contains("reason"));
}

TEST_CASE("RpcDispatcher - unknown method returns error", "[unit][rpc]") {
    dlss::RpcDispatcher dispatcher;
    auto result = dispatcher.dispatch("unknownMethod", {});
    REQUIRE(result.contains("error"));
}
