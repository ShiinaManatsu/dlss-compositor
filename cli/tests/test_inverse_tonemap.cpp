// =============================================================================
// test_inverse_tonemap.cpp — CPU verification of inverse_tonemap_eotf.comp math.
//
// These tests mirror the GLSL functions in inverse_tonemap_eotf.comp and verify
// round-trip accuracy with the forward functions in tonemap_eotf.comp:
//   - LDR sRGB round-trip: srgbEncode → srgbDecode (inverse sRGB EOTF)
//   - HDR ACES round-trip: acesFilmic → acesInverse
//   - HDR PQ round-trip:   pqEncode → pqDecode (inverse PQ)
//   - HDR full round-trip: ACES+PQ forward → inverse PQ+ACES
//   - NaN/Inf safety: HDR inverse never produces NaN or Inf
//
// No GPU required — pure CPU math.
// =============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <cmath>
#include <algorithm>

// ---------------------------------------------------------------------------
// CPU mirrors of shader functions (forward — must match tonemap_eotf.comp)
// ---------------------------------------------------------------------------

static float srgbEncode(float x) {
    x = std::max(0.0f, std::min(1.0f, x));
    return (x < 0.0031308f) ? x * 12.92f : 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static float acesFilmic(float x) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return std::max(0.0f, std::min(1.0f, (x * (a * x + b)) / (x * (c * x + d) + e)));
}

static float pqEncode(float scene_linear) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float L    = std::max(0.0f, scene_linear * 0.01f);  // 1.0 scene = 100 nits = 0.01 * 10000
    float Lm1  = std::pow(L, m1);
    return std::pow((c1 + c2 * Lm1) / (1.0f + c3 * Lm1), m2);
}

// ---------------------------------------------------------------------------
// CPU mirrors of inverse functions (must match inverse_tonemap_eotf.comp)
// ---------------------------------------------------------------------------

static float srgbDecode(float x) {
    return (x < 0.04045f) ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

static float acesInverse(float y) {
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    float A    = c * y - a;
    float B    = d * y - b;
    float C    = e * y;
    float disc = std::max(0.0f, B * B - 4.0f * A * C);  // NaN guard: clamp to 0
    return std::max(0.0f, (-B - std::sqrt(disc)) / (2.0f * A));
}

static float pqDecode(float pq) {
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    float E   = std::pow(std::max(0.0f, pq), 1.0f / m2);
    float num = std::max(0.0f, E - c1);
    float den = std::max(1e-10f, c2 - c3 * E);
    return std::pow(num / den, 1.0f / m1) * 100.0f;  // Back to scene-linear (1.0 = 100 nits)
}

// ---------------------------------------------------------------------------
// LDR sRGB round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("LDR sRGB round-trip error < 0.01", "[unit][ldr][roundtrip]") {
    // Forward: linear → sRGB; Inverse: sRGB → linear
    // Error must be < 0.01 (absolute)
    for (float v : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        float encoded = srgbEncode(v);
        float decoded = srgbDecode(encoded);
        REQUIRE(std::abs(decoded - v) < 0.01f);
    }
}

TEST_CASE("LDR sRGB round-trip - identity for black and white", "[unit][ldr]") {
    REQUIRE(srgbDecode(srgbEncode(0.0f)) == Catch::Approx(0.0f).epsilon(0.0001f));
    REQUIRE(srgbDecode(srgbEncode(1.0f)) == Catch::Approx(1.0f).epsilon(0.001f));
}

TEST_CASE("LDR sRGB round-trip - linear segment continuity", "[unit][ldr]") {
    // At the breakpoint 0.0031308, both branches should agree
    const float breakpoint = 0.0031308f;
    float encoded = srgbEncode(breakpoint);
    float decoded = srgbDecode(encoded);
    REQUIRE(std::abs(decoded - breakpoint) < 0.001f);
}

// ---------------------------------------------------------------------------
// HDR ACES round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("HDR ACES round-trip error < 0.05 for mid-range", "[unit][hdr][roundtrip]") {
    // ACES inverse has limited precision for extreme values (quadratic approximation)
    // Mid-range values [0.1, 2.0] should round-trip within 5% relative error
    for (float v : {0.1f, 0.18f, 0.5f, 1.0f, 2.0f}) {
        float tonemapped = acesFilmic(v);
        float recovered  = acesInverse(tonemapped);
        float relError   = std::abs(recovered - v) / std::max(v, 0.001f);
        REQUIRE(relError < 0.05f);
    }
}

TEST_CASE("HDR ACES inverse - black maps to black", "[unit][hdr]") {
    // ACES(0) = 0, acesInverse(0) should also be 0 (or very close)
    float result = acesInverse(0.0f);
    REQUIRE(std::abs(result) < 0.001f);
}

TEST_CASE("HDR ACES inverse - output always non-negative", "[unit][hdr]") {
    // For any valid tonemapped value in [0,1], inverse must be >= 0
    for (int i = 0; i <= 100; ++i) {
        float y = static_cast<float>(i) / 100.0f;
        REQUIRE(acesInverse(y) >= 0.0f);
    }
}

// ---------------------------------------------------------------------------
// HDR PQ round-trip tests
// ---------------------------------------------------------------------------

TEST_CASE("HDR PQ round-trip error < 0.02 for [0.01, 4.0]", "[unit][hdr][roundtrip]") {
    // PQ is a precise standard; round-trip should be very accurate
    for (float v : {0.01f, 0.18f, 1.0f, 2.0f, 4.0f}) {
        float pq      = pqEncode(v);
        float decoded = pqDecode(pq);
        float relError = std::abs(decoded - v) / std::max(v, 0.001f);
        REQUIRE(relError < 0.02f);
    }
}

TEST_CASE("HDR PQ round-trip - black maps to black", "[unit][hdr]") {
    float pq      = pqEncode(0.0f);
    float decoded = pqDecode(pq);
    REQUIRE(std::abs(decoded) < 0.001f);
}

// ---------------------------------------------------------------------------
// HDR full round-trip: forward (ACES+PQ) then inverse (PQ+ACES)
// ---------------------------------------------------------------------------

TEST_CASE("HDR full round-trip (ACES+PQ forward then inverse PQ+ACES)", "[unit][hdr][roundtrip]") {
    // Full pipeline: scene_linear → ACES → PQ → (inverse PQ) → (inverse ACES) → scene_linear
    // Round-trip relative error < 5%
    for (float v : {0.18f, 1.0f, 2.0f}) {
        // Forward
        float tonemapped = acesFilmic(v);
        float pq         = pqEncode(tonemapped);

        // Inverse
        float tm_recovered = pqDecode(pq);
        tm_recovered = std::max(0.0f, std::min(1.0f, tm_recovered));  // Clamp to [0,1] for ACES inverse
        float scene_linear = acesInverse(tm_recovered);

        // Round-trip error < 5% relative
        float relError = std::abs(scene_linear - v) / std::max(v, 0.001f);
        REQUIRE(relError < 0.05f);
    }
}

// ---------------------------------------------------------------------------
// NaN / Inf safety for HDR inverse
// ---------------------------------------------------------------------------

TEST_CASE("HDR inverse does not produce NaN/Inf", "[unit][hdr][safety]") {
    // Test a range of PQ-encoded values, including edge cases
    // All results must be finite and non-negative
    for (float pq : {0.0f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f}) {
        float tm    = pqDecode(pq);
        tm          = std::max(0.0f, std::min(1.0f, tm));  // Clamp for ACES inverse
        float scene = acesInverse(tm);
        REQUIRE(std::isfinite(scene));
        REQUIRE(scene >= 0.0f);
    }
}

TEST_CASE("HDR ACES inverse discriminant clamped (no NaN from sqrt of negative)", "[unit][hdr][safety]") {
    // acesInverse must handle near-zero or negative discriminant from float rounding
    // y = 1.0 exactly is a degenerate case for the Narkowicz formula
    for (float y : {0.0f, 0.001f, 0.5f, 0.999f, 1.0f}) {
        float result = acesInverse(y);
        REQUIRE(std::isfinite(result));
        REQUIRE(result >= 0.0f);
    }
}
