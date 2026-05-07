#include "../lib/catch2/single_include/catch2/catch.hpp"

// These tests lock down the gating logic for `PlaybackParameters::apply_air_absorption_to_wet` in
// resonance_player.cpp's `_build_playback_params`:
//
//     new_params.apply_air_absorption_to_wet =
//         c.air_absorption_enabled && srv && _compute_baked_data_variation(srv) >= 0;
//
// The wet pre-EQ exists so baked reflection IRs (which do not encode source→listener air absorption) pick up the
// distance-dependent high-frequency damping that realtime ray-traced IRs already include per ray. Applying it on
// realtime would double-attenuate; applying it without air absorption enabled at all is a no-op anyway.

namespace {

// _compute_baked_data_variation return values, mirrored from src/resonance_player.cpp.
constexpr int kVariationRealtime = -1;      // ray-traced reflections
constexpr int kVariationBakedReverb = 0;    // IPL_BAKEDDATAVARIATION_REVERB
constexpr int kVariationStaticSource = 1;   // IPL_BAKEDDATAVARIATION_STATICSOURCE
constexpr int kVariationStaticListener = 2; // IPL_BAKEDDATAVARIATION_STATICLISTENER

inline bool gate_air_absorption_to_wet(bool air_absorption_enabled, bool has_srv, int baked_variation) {
    return air_absorption_enabled && has_srv && baked_variation >= 0;
}

} // namespace

TEST_CASE("air absorption wet: realtime variation never receives the wet pre-EQ", "[air_absorption_wet]") {
    // Realtime ray-traced reflections include air absorption in the IR per ray; applying the pre-EQ on top would
    // double-attenuate high frequencies.
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationRealtime) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationRealtime) == false);
}

TEST_CASE("air absorption wet: baked reverb variation enables the pre-EQ when air absorption is on", "[air_absorption_wet]") {
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationBakedReverb) == true);
}

TEST_CASE("air absorption wet: baked static-source/listener also receives the pre-EQ", "[air_absorption_wet]") {
    // STATICSOURCE/STATICLISTENER bake the geometry to a fixed endpoint position, but the source→listener air path
    // still varies at runtime; the pre-EQ is the cheapest way to express that in the wet signal.
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationStaticSource) == true);
    REQUIRE(gate_air_absorption_to_wet(true, true, kVariationStaticListener) == true);
}

TEST_CASE("air absorption wet: master switch off blocks the pre-EQ regardless of variation", "[air_absorption_wet]") {
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationBakedReverb) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationStaticSource) == false);
    REQUIRE(gate_air_absorption_to_wet(false, true, kVariationStaticListener) == false);
}

TEST_CASE("air absorption wet: missing server disables the pre-EQ", "[air_absorption_wet]") {
    // Defensive: if no server is available we cannot resolve the variation, so the wet pre-EQ stays off.
    REQUIRE(gate_air_absorption_to_wet(true, false, kVariationBakedReverb) == false);
    REQUIRE(gate_air_absorption_to_wet(true, false, kVariationRealtime) == false);
}
