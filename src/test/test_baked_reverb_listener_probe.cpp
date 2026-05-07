#include "../lib/catch2/single_include/catch2/catch.hpp"

// These tests lock down the gating logic for the listener-centric probe lookup in
// resonance_server_sources.cpp's `_update_source_internal`:
//
//     bool use_listener_probe = baked_reverb_use_listener_probe;
//     if (handle >= 0 && handle < kMaxCacheHandles) {
//         const int8_t ov = _source_baked_reverb_listener_probe_override_[handle].load(...);
//         if (ov >= 0)
//             use_listener_probe = (ov != 0);
//     }
//     if (use_listener_probe && baked_data_variation == 0 && enable_reflections &&
//         (sim_flags & IPL_SIMULATIONFLAGS_REFLECTIONS) != 0 &&
//         pending_listener_valid) { /* second iplSourceSetInputs with listener pos */ }
//
// Background: IPL_BAKEDDATAVARIATION_REVERB picks the probe nearest inputs.source.origin. The reverb bake
// assumes source==listener at the probe (the IR is the listener's room), so feeding the source position picks
// the wrong probe whenever the source is in a different room than the listener. The fix overrides only the
// reflection-side source position with the listener's position; STATICSOURCE/STATICLISTENER variations bake to
// fixed endpoints and must not be touched, and realtime ray-traced reflections do not use probes at all.

namespace {

constexpr int kVariationRealtime = -1;      // ray-traced reflections (no probes)
constexpr int kVariationBakedReverb = 0;    // IPL_BAKEDDATAVARIATION_REVERB (the only variation we redirect)
constexpr int kVariationStaticSource = 1;   // IPL_BAKEDDATAVARIATION_STATICSOURCE (fixed endpoint)
constexpr int kVariationStaticListener = 2; // IPL_BAKEDDATAVARIATION_STATICLISTENER (fixed endpoint)

constexpr int kOverrideUseGlobal = -1;
constexpr int kOverrideOff = 0;
constexpr int kOverrideOn = 1;

// Mirrors the resolution in _update_source_internal.
inline bool resolve_use_listener_probe(bool global_flag, int per_source_override) {
    if (per_source_override >= 0)
        return per_source_override != 0;
    return global_flag;
}

// Full gate that controls whether the second iplSourceSetInputs for IPL_SIMULATIONFLAGS_REFLECTIONS runs.
inline bool gate_listener_probe_setinputs(bool use_listener_probe, int baked_variation, bool enable_reflections,
                                          bool reflections_in_sim_flags, bool listener_valid) {
    return use_listener_probe && baked_variation == kVariationBakedReverb && enable_reflections &&
           reflections_in_sim_flags && listener_valid;
}

} // namespace

TEST_CASE("baked reverb listener probe: per-source override beats the global flag in both directions",
          "[baked_reverb_listener_probe]") {
    REQUIRE(resolve_use_listener_probe(/*global*/ false, /*override*/ kOverrideOn) == true);
    REQUIRE(resolve_use_listener_probe(/*global*/ true, /*override*/ kOverrideOff) == false);
}

TEST_CASE("baked reverb listener probe: override -1 falls back to the global flag",
          "[baked_reverb_listener_probe]") {
    REQUIRE(resolve_use_listener_probe(/*global*/ true, /*override*/ kOverrideUseGlobal) == true);
    REQUIRE(resolve_use_listener_probe(/*global*/ false, /*override*/ kOverrideUseGlobal) == false);
}

TEST_CASE("baked reverb listener probe: only IPL_BAKEDDATAVARIATION_REVERB triggers the second SetInputs",
          "[baked_reverb_listener_probe]") {
    // Realtime / STATICSOURCE / STATICLISTENER must not be redirected — realtime never uses probes, and
    // STATIC* variations bake to a fixed endpoint that the user explicitly chose; overriding the source
    // position would invalidate the bake's geometry assumptions.
    REQUIRE(gate_listener_probe_setinputs(true, kVariationRealtime, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(true, kVariationStaticSource, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(true, kVariationStaticListener, true, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(true, kVariationBakedReverb, true, true, true) == true);
}

TEST_CASE("baked reverb listener probe: skipped when reflections are disabled",
          "[baked_reverb_listener_probe]") {
    // When reflections are off (per-source mute / runtime override / mix == 0), the simulator never reads the
    // reflection inputs anyway, so issuing the redirected SetInputs is wasted work — and worse, it would publish
    // a stale source position the first time reflections turn back on.
    REQUIRE(gate_listener_probe_setinputs(true, kVariationBakedReverb, /*enable_reflections*/ false, true, true) == false);
    REQUIRE(gate_listener_probe_setinputs(true, kVariationBakedReverb, true, /*reflections_in_sim_flags*/ false, true) == false);
}

TEST_CASE("baked reverb listener probe: skipped when listener is invalid",
          "[baked_reverb_listener_probe]") {
    // When the listener has not been registered yet (or was explicitly invalidated), reading
    // _read_listener_coords_seqlock() would return zero/garbage and pick whichever probe is nearest world origin.
    // Better to leave the source-position lookup in place until the listener becomes valid again.
    REQUIRE(gate_listener_probe_setinputs(true, kVariationBakedReverb, true, true, /*listener_valid*/ false) == false);
}

TEST_CASE("baked reverb listener probe: master switch off blocks the redirect",
          "[baked_reverb_listener_probe]") {
    REQUIRE(gate_listener_probe_setinputs(/*use_listener_probe*/ false, kVariationBakedReverb, true, true, true) == false);
}

TEST_CASE("baked reverb listener probe: end-to-end resolution composes override + gate",
          "[baked_reverb_listener_probe]") {
    // Player turns the redirect on for one source even though the project disables it globally.
    const bool resolved = resolve_use_listener_probe(/*global*/ false, /*override*/ kOverrideOn);
    REQUIRE(gate_listener_probe_setinputs(resolved, kVariationBakedReverb, true, true, true) == true);

    // Inverse: project enables the redirect, but a stylised always-on reverb bed opts out per source.
    const bool resolved_off = resolve_use_listener_probe(/*global*/ true, /*override*/ kOverrideOff);
    REQUIRE(gate_listener_probe_setinputs(resolved_off, kVariationBakedReverb, true, true, true) == false);
}
