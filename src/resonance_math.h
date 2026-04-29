#ifndef RESONANCE_MATH_H
#define RESONANCE_MATH_H

#include <cmath>
#include <cstdint>

namespace resonance {

/// Replace NaN/Inf with 0 to prevent Steam Audio "invalid IPLfloat32" warnings.
inline float sanitize_audio_float(float v) {
    return std::isfinite(v) ? v : 0.0f;
}

/// Sanitize Steam Audio delay (IPLint32): finite float round-trip, NaN/Inf -> 0 samples.
inline int32_t sanitize_delay_samples(int32_t v) {
    float f = static_cast<float>(v);
    f = sanitize_audio_float(f);
    return static_cast<int32_t>(std::lroundf(f));
}

/// Impulse-response length in samples from sample rate and duration (seconds).
inline int32_t reverb_ir_size_samples(int sample_rate, float duration_sec) {
    float d = sanitize_audio_float(duration_sec);
    return static_cast<int32_t>(std::lroundf(static_cast<float>(sample_rate) * d));
}

/// Clamp reverb time to valid range for Steam Audio (PARAMETRIC/HYBRID require > 0).
inline float clamp_reverb_time(float v) {
    float s = sanitize_audio_float(v);
    return (s > 0.1f) ? s : 0.1f;
}

/// Wet-path occlusion damping factor for baked REVERB reflections. Mirrors the direct-path occlusion+transmission
/// factor used by iplDirectEffectApply so that walls damp the reflection-effect input (baked REVERB IRs assume
/// source co-located with listener and cannot encode source→listener walls).
///
/// - occlusion: 1 = line-of-sight, 0 = fully occluded
/// - transmission_[low|mid|high]: 1 = fully transmissive (no damping), 0 = blocked
/// - reverb_transmission_amount: 0 = no wet damping (returns 1.0), 1 = full wet damping by direct-path factor
///
/// Returns 1.0 when there is no damping (no walls in the path) and 0.0 when the source is fully blocked with
/// reverb_transmission_amount = 1. Realtime reflections and STATICSOURCE/STATICLISTENER paths should pass
/// reverb_transmission_amount = 0 (or skip this helper) because their IRs already encode the actual geometry.
inline float baked_reverb_wet_occlusion_factor(float occlusion, float transmission_low, float transmission_mid, float transmission_high,
                                               float reverb_transmission_amount) {
    const float occ = std::fmin(std::fmax(sanitize_audio_float(occlusion), 0.0f), 1.0f);
    const float tx_avg = (sanitize_audio_float(transmission_low) + sanitize_audio_float(transmission_mid) + sanitize_audio_float(transmission_high)) / 3.0f;
    const float tx_avg_clamped = std::fmin(std::fmax(tx_avg, 0.0f), 1.0f);
    const float direct_factor = occ + (1.0f - occ) * tx_avg_clamped;
    const float amt = std::fmin(std::fmax(sanitize_audio_float(reverb_transmission_amount), 0.0f), 1.0f);
    const float damping = amt * (1.0f - direct_factor);
    const float factor = 1.0f - damping;
    if (factor <= 0.0f)
        return 0.0f;
    if (factor >= 1.0f)
        return 1.0f;
    return factor;
}

/// Extra wet falloff from runtime reverb_max_distance (meters). When max_dist_m <= 0: no effect (returns 1).
/// When dist <= max: 1; linear fade 1..0 from max_dist_m to 2*max_dist_m (used for reflections + pathing wet).
inline float reverb_wet_falloff_max_distance(float dist_m, float max_dist_m) {
    if (max_dist_m <= 0.0f || !std::isfinite(dist_m))
        return 1.0f;
    if (dist_m <= max_dist_m)
        return 1.0f;
    const float span = (max_dist_m > 1.0e-6f) ? max_dist_m : 1.0e-6f;
    const float t = (dist_m - max_dist_m) / span;
    const float x = 1.0f - t;
    if (x <= 0.0f)
        return sanitize_audio_float(0.0f);
    if (x >= 1.0f)
        return sanitize_audio_float(1.0f);
    return sanitize_audio_float(x);
}

/// Pure C++ volume ramping (no Godot/Phonon dependency).
/// Smoothly interpolates volume to prevent clicks/pops when parameters change.
inline void apply_volume_ramp(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    // Optimization: Constant volume
    if (std::abs(start_vol - end_vol) < 1e-5f) {
        if (std::abs(start_vol - 1.0f) > 1e-5f) {
            for (int i = 0; i < num_samples; ++i)
                buffer[i] *= start_vol;
        }
        return;
    }

    float step = (end_vol - start_vol) / (float)num_samples;
    float current = start_vol;
    for (int i = 0; i < num_samples; ++i) {
        buffer[i] *= current;
        current += step;
    }
}

/// Like apply_volume_ramp, then sanitize_audio_float per sample (single pass over the buffer).
inline void apply_volume_ramp_and_sanitize(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    if (std::abs(start_vol - end_vol) < 1e-5f) {
        if (std::abs(start_vol - 1.0f) > 1e-5f) {
            for (int i = 0; i < num_samples; ++i) {
                buffer[i] = sanitize_audio_float(buffer[i] * start_vol);
            }
        } else {
            for (int i = 0; i < num_samples; ++i) {
                buffer[i] = sanitize_audio_float(buffer[i]);
            }
        }
        return;
    }

    float step = (end_vol - start_vol) / (float)num_samples;
    float current = start_vol;
    for (int i = 0; i < num_samples; ++i) {
        buffer[i] = sanitize_audio_float(buffer[i] * current);
        current += step;
    }
}

} // namespace resonance

#endif // RESONANCE_MATH_H
