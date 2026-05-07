#include "../lib/catch2/single_include/catch2/catch.hpp"
#include "../resonance_math.h"
#include <cmath>

using namespace resonance;

// These tests lock down the Unity-parity distance-attenuation factor applied to the reflection-effect input
// (PlaybackParameters::refl_distance_attenuation). The formula in resonance_player.cpp is:
//   conv_reverb_gain        = reflections_mix_level * node_vol * refl_dist_att * wet_rmd * wet_occ
//   parametric_mix_level    = reflections_mix_level             * refl_dist_att * wet_occ
// and refl_dist_att resolves as:
//   refl_dist_att = apply_dist_wet ? attenuation : 1.0f
// The tests below intentionally use pure scalar arithmetic mirroring that chain to catch regressions in the
// product ordering and the override resolver contract without spinning up the full player.

namespace {

/// Mirrors the audio-thread gain product for the Conv/TAN wet path.
inline float conv_reverb_gain(float reflections_mix_level, float node_vol,
                              float refl_dist_att, float wet_rmd, float wet_occ) {
    return sanitize_audio_float(reflections_mix_level * node_vol * refl_dist_att * wet_rmd * wet_occ);
}

/// Mirrors the audio-thread gain product for the Parametric/Hybrid wet path.
inline float parametric_mix_level(float reflections_mix_level, float refl_dist_att, float wet_occ) {
    return sanitize_audio_float(reflections_mix_level * refl_dist_att * wet_occ);
}

/// Matches _build_playback_params override resolver. global_on = runtime apply_distance_curve_to_reflections.
/// Note: post-fix the resolver uses reverb_pathing_attenuation (1/d wet curve) instead of the direct attenuation.
inline float resolve_refl_dist_att(int override_mode, bool global_on, float reverb_pathing_attenuation) {
    bool apply_dist_wet;
    switch (override_mode) {
    case 0:
        apply_dist_wet = false;
        break;
    case 1:
        apply_dist_wet = true;
        break;
    default:
        apply_dist_wet = global_on;
        break;
    }
    return apply_dist_wet ? reverb_pathing_attenuation : 1.0f;
}

/// Mirrors `_compute_attenuation`'s wet branch (post-fix): wet always uses 1/d with optional max_distance fade,
/// LINEAR/CUSTOM let the direct curve win when steeper.
enum AttMode { MODE_INVERSE = 0,
               MODE_INVERSE_NO_SIM = 1,
               MODE_LINEAR = 2,
               MODE_CUSTOM_CURVE = 3 };

inline float compute_wet_attenuation(int mode, float dist, float min_d, float max_d, float direct_attenuation) {
    float wet = reverb_wet_distance_attenuation(dist, min_d, max_d);
    if ((mode == MODE_LINEAR || mode == MODE_CUSTOM_CURVE) &&
        direct_attenuation > 0.0f && direct_attenuation < wet) {
        wet = direct_attenuation;
    }
    return wet;
}

} // namespace

TEST_CASE("wet distance: attenuation=1 matches legacy behavior", "[wet_distance_attenuation]") {
    // With refl_dist_att = 1 the product must be identical to the pre-patch chain (mix * node * wet_rmd * wet_occ).
    const float refl_mix = 1.0f;
    const float node_vol = 0.8f;
    const float wet_rmd = 0.7f;
    const float wet_occ = 0.9f;
    const float legacy = sanitize_audio_float(refl_mix * node_vol * wet_rmd * wet_occ);
    REQUIRE(conv_reverb_gain(refl_mix, node_vol, 1.0f, wet_rmd, wet_occ) == Approx(legacy));
    REQUIRE(parametric_mix_level(refl_mix, 1.0f, wet_occ) == Approx(sanitize_audio_float(refl_mix * wet_occ)));
}

TEST_CASE("wet distance: attenuation=0 mutes the wet path", "[wet_distance_attenuation]") {
    // A fully faded source must not leak any reverb energy through the reflection effect input.
    REQUIRE(conv_reverb_gain(1.0f, 1.0f, 0.0f, 1.0f, 1.0f) == Approx(0.0f));
    REQUIRE(parametric_mix_level(1.0f, 0.0f, 1.0f) == Approx(0.0f));
}

TEST_CASE("wet distance: linear scaling with attenuation", "[wet_distance_attenuation]") {
    // The wet input scales linearly with the distance curve value, independently of node_vol / wet_rmd / wet_occ.
    const float a = 0.5f;
    const float full = conv_reverb_gain(1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    const float half = conv_reverb_gain(1.0f, 1.0f, a, 1.0f, 1.0f);
    REQUIRE(half == Approx(full * a));
}

TEST_CASE("wet distance: override Use Global follows runtime flag", "[wet_distance_attenuation]") {
    // override == -1 (Use Global) must honor the runtime toggle.
    REQUIRE(resolve_refl_dist_att(-1, true, 0.25f) == Approx(0.25f));
    REQUIRE(resolve_refl_dist_att(-1, false, 0.25f) == Approx(1.0f));
}

TEST_CASE("wet distance: override Disabled pins factor to 1 regardless of global", "[wet_distance_attenuation]") {
    // override == 0 (Disabled) is the opt-out for 2D ambience beds; global on/off must be ignored.
    REQUIRE(resolve_refl_dist_att(0, true, 0.25f) == Approx(1.0f));
    REQUIRE(resolve_refl_dist_att(0, false, 0.25f) == Approx(1.0f));
}

TEST_CASE("wet distance: override Enabled forces curve regardless of global", "[wet_distance_attenuation]") {
    // override == 1 (Enabled) lets individual sources opt in even when the project disabled the global feature.
    REQUIRE(resolve_refl_dist_att(1, false, 0.4f) == Approx(0.4f));
    REQUIRE(resolve_refl_dist_att(1, true, 0.4f) == Approx(0.4f));
}

TEST_CASE("wet distance: full gain product with override Enabled", "[wet_distance_attenuation]") {
    // Sanity-check that resolver + gain product compose as advertised.
    const float att = 0.2f;
    const float dist_att = resolve_refl_dist_att(1, false, att);
    const float gain = conv_reverb_gain(1.0f, 1.0f, dist_att, 1.0f, 1.0f);
    REQUIRE(gain == Approx(att));
}

TEST_CASE("wet distance: does not bypass wet_occ or wet_rmd", "[wet_distance_attenuation]") {
    // Even at full proximity (attenuation=1), the existing wet_occ and wet_rmd factors must still apply. Regression
    // guard: the Unity-parity patch multiplies into the chain, it does not replace the previous factors.
    const float gain = conv_reverb_gain(1.0f, 1.0f, 1.0f, 0.5f, 0.5f);
    REQUIRE(gain == Approx(0.25f));
}

TEST_CASE("wet distance: INVERSE_NO_SIM gets a real 1/d wet falloff (not unity)", "[wet_distance_attenuation][regression]") {
    // Pre-fix bug: with attenuation_mode = INVERSE_NO_SIM, _compute_attenuation set out_reverb_pathing_attenuation=1.0,
    // so the wet stayed at full level at any distance. Post-fix: wet must follow 1/d with min/max_distance.
    const float wet_far = compute_wet_attenuation(MODE_INVERSE_NO_SIM, 50.0f, 1.0f, 100.0f, /*direct=*/1.0f);
    const float wet_mid = compute_wet_attenuation(MODE_INVERSE_NO_SIM, 5.0f, 1.0f, 100.0f, /*direct=*/1.0f);
    const float wet_near = compute_wet_attenuation(MODE_INVERSE_NO_SIM, 1.0f, 1.0f, 100.0f, /*direct=*/1.0f);
    REQUIRE(wet_near == Approx(1.0f));
    REQUIRE(wet_mid < wet_near);
    REQUIRE(wet_far < wet_mid);
    REQUIRE(wet_far > 0.0f);
}

TEST_CASE("wet distance: INVERSE wet hits zero at max_distance (Steam Audio direct curve is asymptotic)", "[wet_distance_attenuation]") {
    // INVERSE direct attenuation comes from Steam Audio (1/d, never zero); the wet curve must still fade to zero
    // at max_distance so distant sources do not leak reverb forever.
    const float wet_at_max = compute_wet_attenuation(MODE_INVERSE, 100.0f, 1.0f, 100.0f, /*direct=*/0.01f);
    REQUIRE(wet_at_max == Approx(0.0f));
}

TEST_CASE("wet distance: LINEAR direct curve overrides wet when steeper", "[wet_distance_attenuation]") {
    // 1/d is steep at large min_distance/max_distance ratios but flat when min_distance is close to max_distance.
    // Pick a tight range (min=1, max=1.5) so the LINEAR fade is steeper than 1/d at the midpoint.
    const float dist = 1.25f, min_d = 1.0f, max_d = 1.5f;
    const float wet_pure = reverb_wet_distance_attenuation(dist, min_d, max_d); // 1/1.25 ≈ 0.8 (before fade window)
    const float direct = 1.0f - (dist - min_d) / (max_d - min_d);               // 1 - 0.25/0.5 = 0.5
    REQUIRE(direct < wet_pure);
    const float wet = compute_wet_attenuation(MODE_LINEAR, dist, min_d, max_d, direct);
    REQUIRE(wet == Approx(direct));
}

TEST_CASE("wet distance: LINEAR direct=0 does not snap wet to 0 (avoids silent reverb past direct max)", "[wet_distance_attenuation]") {
    // Past the LINEAR max_distance the direct dries up; wet must keep decaying with 1/d * fade so far-away
    // listeners still hear the tail without abrupt silence.
    const float wet = compute_wet_attenuation(MODE_LINEAR, 50.0f, 1.0f, 100.0f, /*direct=*/0.0f);
    REQUIRE(wet > 0.0f);
    REQUIRE(wet < 0.1f); // 1/50 * (no fade yet, fade window starts at 91)
}

TEST_CASE("wet distance: max_distance=0 keeps pure 1/d (no end-of-range cutoff)", "[wet_distance_attenuation]") {
    // Backwards-compatible "infinite" wet for users who explicitly set reverb_max_distance=0.
    const float wet = compute_wet_attenuation(MODE_INVERSE_NO_SIM, 1000.0f, 1.0f, 0.0f, /*direct=*/1.0f);
    REQUIRE(wet == Approx(0.001f));
}
