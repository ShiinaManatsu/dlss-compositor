// =============================================================================
// test_tonemap_pass.cpp — CPU verification of tonemap_eotf.comp shader math.
//
// These tests mirror the GLSL functions in tonemap_eotf.comp and verify:
//   - LDR sRGB EOTF (IEC 61966-2-1): known reference values
//   - HDR ACES Filmic (Narkowicz approximation): monotonicity, mid-grey, clamp
//   - HDR PQ ST.2084 (SMPTE ST.2084 / ITU-R BT.2100): normalization, range
//   - LDR vs HDR path distinction (LDR must NOT apply ACES)
//
// No GPU required — pure CPU math.
// =============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// CPU mirrors of shader functions (must exactly match tonemap_eotf.comp)
// ---------------------------------------------------------------------------

static float srgbEncodeScalar(float linear) {
    linear = std::max(0.0f, std::min(1.0f, linear));  // clamp [0,1]
    return (linear < 0.0031308f)
        ? (linear * 12.92f)
        : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
}

static float acesFilmicScalar(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float result = (x * (a * x + b)) / (x * (c * x + d) + e);
    return std::max(0.0f, std::min(1.0f, result));
}

static float pqEotfScalar(float linear_scene) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float L = std::max(0.0f, linear_scene * 0.01f);  // 1.0 scene = 100 nits = 0.01 * 10000
    float Lm1 = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm1) / (1.0f + c3 * Lm1), m2);
}

// ---------------------------------------------------------------------------
// LDR sRGB EOTF tests
// ---------------------------------------------------------------------------

TEST_CASE("LDR sRGB EOTF - known values", "[unit][ldr]") {
    // sRGB(0.5) ≈ 0.7354 (reference: IEC 61966-2-1 table)
    REQUIRE(srgbEncodeScalar(0.5f) == Catch::Approx(0.7354f).epsilon(0.002));

    // sRGB(0.0) = 0.0 (black point)
    REQUIRE(srgbEncodeScalar(0.0f) == Catch::Approx(0.0f));

    // sRGB(1.0) = 1.0 (white point)
    REQUIRE(srgbEncodeScalar(1.0f) == Catch::Approx(1.0f));

    // sRGB(2.0) = sRGB(1.0) = 1.0 (HDR values clamped in LDR mode)
    REQUIRE(srgbEncodeScalar(2.0f) == Catch::Approx(1.0f));

    // sRGB(-0.5) = 0.0 (negative clamped)
    REQUIRE(srgbEncodeScalar(-0.5f) == Catch::Approx(0.0f));
}

TEST_CASE("LDR sRGB EOTF - linear segment", "[unit][ldr]") {
    // For linear < 0.0031308: output = linear * 12.92
    const float v = 0.001f;
    REQUIRE(srgbEncodeScalar(v) == Catch::Approx(v * 12.92f).epsilon(0.0001f));
}

TEST_CASE("LDR sRGB EOTF - power segment", "[unit][ldr]") {
    // For linear >= 0.0031308: output = 1.055 * linear^(1/2.4) - 0.055
    const float v = 0.5f;
    const float expected = 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    REQUIRE(srgbEncodeScalar(v) == Catch::Approx(expected).epsilon(0.0001f));
}

TEST_CASE("LDR sRGB EOTF - monotonically increasing", "[unit][ldr]") {
    // Verify monotonicity over [0, 1]
    float prev = srgbEncodeScalar(0.0f);
    for (int i = 1; i <= 100; ++i) {
        float cur = srgbEncodeScalar(static_cast<float>(i) / 100.0f);
        REQUIRE(cur >= prev);
        prev = cur;
    }
}

// ---------------------------------------------------------------------------
// HDR ACES Filmic tests
// ---------------------------------------------------------------------------

TEST_CASE("HDR ACES Filmic - mid-grey preserved", "[unit][hdr]") {
    // 18% mid-grey should survive ACES approximately
    // Narkowicz ACES is designed to approximately preserve mid-grey
    float result = acesFilmicScalar(0.18f);
    REQUIRE(result > 0.1f);
    REQUIRE(result < 1.0f);
    // ACES ~preserves 18% grey (within 5% tolerance per Narkowicz design goals)
    REQUIRE(result == Catch::Approx(0.18f).epsilon(0.05));
}

TEST_CASE("HDR ACES Filmic - high luminance compressed", "[unit][hdr]") {
    // Very bright values should be compressed to < 1.0
    float result = acesFilmicScalar(100.0f);
    REQUIRE(result < 1.0f);
    REQUIRE(result > 0.9f);  // Should be near white

    // Even extreme values stay in [0, 1]
    REQUIRE(acesFilmicScalar(1000.0f) <= 1.0f);
    REQUIRE(acesFilmicScalar(0.0f)    >= 0.0f);
}

TEST_CASE("HDR ACES Filmic - monotonically increasing", "[unit][hdr]") {
    // ACES should be monotonic over a practical input range [0, 100]
    float prev = acesFilmicScalar(0.0f);
    for (int i = 1; i <= 200; ++i) {
        float x = static_cast<float>(i) * 0.5f;  // 0.5 to 100.0
        float cur = acesFilmicScalar(x);
        REQUIRE(cur >= prev - 1e-5f);  // allow tiny float noise
        prev = cur;
    }
}

TEST_CASE("HDR ACES Filmic - black stays black", "[unit][hdr]") {
    REQUIRE(acesFilmicScalar(0.0f) == Catch::Approx(0.0f).epsilon(0.001f));
}

// ---------------------------------------------------------------------------
// HDR PQ ST.2084 EOTF tests
// ---------------------------------------------------------------------------

TEST_CASE("HDR PQ EOTF - mid-grey chain", "[unit][hdr]") {
    // 18% mid-grey scene-linear → ACES → PQ
    // Validates the full HDR pipeline chain
    float tonemapped = acesFilmicScalar(0.18f);
    float pq = pqEotfScalar(tonemapped);

    // PQ output must be in valid [0, 1] range
    REQUIRE(pq > 0.0f);
    REQUIRE(pq <= 1.0f);
}

TEST_CASE("HDR PQ EOTF - black maps to black", "[unit][hdr]") {
    // 0.0 scene-linear → L = 0 nits → PQ = 0
    float result = pqEotfScalar(0.0f);
    REQUIRE(result == Catch::Approx(0.0f).epsilon(0.001f));
}

TEST_CASE("HDR PQ EOTF - 100 nit reference (1.0 scene-linear)", "[unit][hdr]") {
    // Per Blender convention: 1.0 scene-linear = 100 nits diffuse white
    // L = 1.0 * 0.01 = 0.01 = 100/10000 nits
    // PQ(100 nits) is a well-known reference: approximately 0.5083
    float result = pqEotfScalar(1.0f);
    // Per ST.2084 standard: 100 nits → PQ ≈ 0.5083
    REQUIRE(result == Catch::Approx(0.5083f).epsilon(0.005f));
}

TEST_CASE("HDR PQ EOTF - monotonically increasing", "[unit][hdr]") {
    // PQ should be monotonic: brighter scene → higher PQ code
    float prev = pqEotfScalar(0.0f);
    for (int i = 1; i <= 100; ++i) {
        float cur = pqEotfScalar(static_cast<float>(i) * 0.1f);
        REQUIRE(cur >= prev - 1e-5f);
        prev = cur;
    }
}

// ---------------------------------------------------------------------------
// LDR vs HDR path distinction
// ---------------------------------------------------------------------------

TEST_CASE("LDR does NOT apply ACES tonemap", "[unit][pipeline]") {
    // LDR path: 0.5 scene-linear → sRGB(0.5) ≈ 0.7354
    // HDR path: 0.5 scene-linear → ACES(0.5) → different value
    float ldr_out  = srgbEncodeScalar(0.5f);
    float aces_out = acesFilmicScalar(0.5f);

    // The values must differ — LDR bypasses ACES
    REQUIRE(ldr_out != Catch::Approx(aces_out).epsilon(0.01f));

    // LDR output is well-defined
    REQUIRE(ldr_out == Catch::Approx(0.7354f).epsilon(0.002f));
}

TEST_CASE("HDR path applies both ACES and PQ (not sRGB)", "[unit][pipeline]") {
    // HDR output (ACES + PQ) should differ from sRGB output for same input
    float linear_in = 0.5f;
    float ldr_out   = srgbEncodeScalar(linear_in);

    float aces_out  = acesFilmicScalar(linear_in);
    float hdr_out   = pqEotfScalar(aces_out);

    // HDR and LDR outputs must differ
    REQUIRE(hdr_out != Catch::Approx(ldr_out).epsilon(0.01f));
}

TEST_CASE("HDR LDR boundary: LDR input in [0,1] gives valid sRGB out", "[unit][pipeline]") {
    // All inputs in [0, 1] must produce [0, 1] sRGB output
    for (int i = 0; i <= 20; ++i) {
        float v = static_cast<float>(i) / 20.0f;
        float out = srgbEncodeScalar(v);
        REQUIRE(out >= 0.0f);
        REQUIRE(out <= 1.0f);
    }
}
