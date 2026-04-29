#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_math.h"
#include <cmath>
#include <limits>

using namespace resonance;

// --- baked_reverb_wet_occlusion_factor ---
// The helper exists specifically to fix outdoor sources leaking reverb through walls in baked REVERB mode.
// IPL_BAKEDDATAVARIATION_REVERB IRs assume source = listener at each probe, so the reflection-effect input
// needs separate wall damping. These tests lock down the formula so realtime and STATICSOURCE paths never
// accidentally pick up a factor < 1 (which would double-attenuate).

TEST_CASE("wet occlusion: line of sight returns unity", "[reflection_occlusion]") {
    REQUIRE(baked_reverb_wet_occlusion_factor(1.0f, 1.0f, 1.0f, 1.0f, 1.0f) == Approx(1.0f));
    REQUIRE(baked_reverb_wet_occlusion_factor(1.0f, 1.0f, 1.0f, 1.0f, 0.0f) == Approx(1.0f));
}

TEST_CASE("wet occlusion: fully blocked and full amount mutes wet", "[reflection_occlusion]") {
    // occlusion=0 with transmission=0 across all bands and amount=1 (full damping) -> wet muted.
    REQUIRE(baked_reverb_wet_occlusion_factor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f) == Approx(0.0f));
}

TEST_CASE("wet occlusion: reverb_transmission_amount=0 keeps wet at unity", "[reflection_occlusion]") {
    // When the user dials damping to zero, the wet path must not be attenuated regardless of occlusion.
    REQUIRE(baked_reverb_wet_occlusion_factor(0.0f, 0.0f, 0.0f, 0.0f, 0.0f) == Approx(1.0f));
    REQUIRE(baked_reverb_wet_occlusion_factor(0.3f, 0.1f, 0.1f, 0.1f, 0.0f) == Approx(1.0f));
}

TEST_CASE("wet occlusion: partial transmission through walls", "[reflection_occlusion]") {
    // Fully occluded (occ=0), walls let 0.25 mid-transmission, full amount -> wet ≈ 0.25.
    const float got = baked_reverb_wet_occlusion_factor(0.0f, 0.25f, 0.25f, 0.25f, 1.0f);
    REQUIRE(got == Approx(0.25f));
}

TEST_CASE("wet occlusion: matches direct-path occ_factor when amount=1", "[reflection_occlusion]") {
    // Lock down the formula: factor = 1 - amount * (1 - (occ + (1-occ)*tx_avg))
    const float occ = 0.4f;
    const float tx_low = 0.3f;
    const float tx_mid = 0.5f;
    const float tx_high = 0.7f;
    const float tx_avg = (tx_low + tx_mid + tx_high) / 3.0f;
    const float expected_direct = occ + (1.0f - occ) * tx_avg;
    const float got = baked_reverb_wet_occlusion_factor(occ, tx_low, tx_mid, tx_high, 1.0f);
    REQUIRE(got == Approx(expected_direct).margin(1e-5f));
}

TEST_CASE("wet occlusion: mid amount is linear blend", "[reflection_occlusion]") {
    const float occ = 0.2f;
    const float full = baked_reverb_wet_occlusion_factor(occ, 0.1f, 0.1f, 0.1f, 1.0f);
    const float none = baked_reverb_wet_occlusion_factor(occ, 0.1f, 0.1f, 0.1f, 0.0f);
    const float half = baked_reverb_wet_occlusion_factor(occ, 0.1f, 0.1f, 0.1f, 0.5f);
    REQUIRE(none == Approx(1.0f));
    REQUIRE(half == Approx(0.5f * full + 0.5f * none).margin(1e-5f));
}

TEST_CASE("wet occlusion: NaN inputs sanitized to zero then clamped", "[reflection_occlusion]") {
    const float nan_v = std::numeric_limits<float>::quiet_NaN();
    // NaN occlusion -> sanitized to 0 (fully occluded), NaN transmissions -> 0 transmissive, amount=1 -> wet=0.
    REQUIRE(baked_reverb_wet_occlusion_factor(nan_v, nan_v, nan_v, nan_v, 1.0f) == Approx(0.0f));
    // NaN amount -> sanitized to 0 -> no damping.
    REQUIRE(baked_reverb_wet_occlusion_factor(0.0f, 0.0f, 0.0f, 0.0f, nan_v) == Approx(1.0f));
}

TEST_CASE("wet occlusion: clamped in [0,1]", "[reflection_occlusion]") {
    // Pathological out-of-range inputs must not produce factors outside [0,1].
    const float got_over = baked_reverb_wet_occlusion_factor(2.0f, 2.0f, 2.0f, 2.0f, 2.0f);
    const float got_under = baked_reverb_wet_occlusion_factor(-1.0f, -1.0f, -1.0f, -1.0f, 1.0f);
    REQUIRE(got_over >= 0.0f);
    REQUIRE(got_over <= 1.0f);
    REQUIRE(got_under >= 0.0f);
    REQUIRE(got_under <= 1.0f);
}

// --- Regression: realtime / STATICSOURCE / STATICLISTENER paths keep wet at unity ---
// The player skips this helper for non-REVERB baked variations and passes wet_occlusion_factor = 1.0.
// This guard test encodes the invariant: callers should never invoke the helper for those paths.
// If someone does invoke it, the result with amount=0 must still be unity, preventing double-attenuation.
TEST_CASE("wet occlusion: amount=0 emulates realtime / STATICSOURCE behaviour", "[reflection_occlusion][regression]") {
    // Realtime mode is modeled by apply_occlusion_to_baked_reflections=false (toggle off) OR by the player
    // bypassing the helper. Either way, no wet attenuation should happen. Setting amount=0 here stands in
    // for that guarantee at the math level.
    for (float occ = 0.0f; occ <= 1.0f; occ += 0.1f) {
        for (float tx = 0.0f; tx <= 1.0f; tx += 0.1f) {
            const float got = baked_reverb_wet_occlusion_factor(occ, tx, tx, tx, 0.0f);
            REQUIRE(got == Approx(1.0f));
        }
    }
}

// --- Ramp regression: per-sample volume ramp for parametric_mix_level * wet_occ ---
// Audio thread multiplies reflections_mix_level by wet_occ before ramping prev -> current. A sudden change
// in wet_occ (e.g. wall transition) must produce a smooth ramp without clicks. We re-run apply_volume_ramp
// with post-factor values and check monotonicity.
TEST_CASE("wet occlusion: ramp prev*occ to new*occ has no zero-crossing clicks", "[reflection_occlusion][volume_ramp]") {
    const int n = 16;
    float buffer[n];
    for (int i = 0; i < n; ++i)
        buffer[i] = 1.0f;
    const float prev_level = 1.0f;
    const float new_level = 1.0f;
    const float prev_occ = 1.0f;
    const float new_occ = baked_reverb_wet_occlusion_factor(0.0f, 0.0f, 0.0f, 0.0f, 1.0f); // 0
    apply_volume_ramp(prev_level * prev_occ, new_level * new_occ, n, buffer);
    // Monotonic decreasing from 1.0 to (1-1/n).
    REQUIRE(buffer[0] == Approx(1.0f));
    for (int i = 1; i < n; ++i) {
        REQUIRE(buffer[i] <= buffer[i - 1] + 1e-5f);
        REQUIRE(buffer[i] >= 0.0f);
    }
}
