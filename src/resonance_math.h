#ifndef RESONANCE_MATH_H
#define RESONANCE_MATH_H

#include <cmath>
#include <cstdint>

namespace resonance {

// Small audio-safe helpers: sanitize floats, IR sizing, wet factors, linear ramps (no Godot/phonon includes).

/// Replace NaN/Inf with 0 for IPL float parameters.
inline float sanitize_audio_float(float v) {
    return std::isfinite(v) ? v : 0.0f;
}

/// Sanitize delay in samples (IPL int field; NaN/Inf → 0).
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

/// Minimum band reverb time used by parametric/hybrid paths (IPL expects > 0).
inline float clamp_reverb_time(float v) {
    float s = sanitize_audio_float(v);
    return (s > 0.1f) ? s : 0.1f;
}

/// Extra wet attenuation for baked REVERB (IR has no directional occlusion): blend toward `1 - direct_path_factor`
/// using `reverb_transmission_amount`. Use 0 amount / skip for realtime or static-source bakes where IR encodes geometry.
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

/// Inverse-distance wet falloff for baked REVERB (IR has no source-listener distance encoded).
/// `min_distance_m` is the unity plateau (≤ min: 1.0). Beyond min: `min/dist` (Steam-Audio INVERSE shape).
/// When `max_distance_m > min_distance_m`: smooth fade to 0 in the last 10% of [min, max], 0 beyond max.
/// When `max_distance_m <= 0`: pure 1/d (no end-of-range cutoff).
/// Non-finite `dist_m` (NaN/Inf) returns 0 — safer default than the plateau when something upstream went wrong.
/// Used for all attenuation modes so wet reflections decay with distance independently of the direct curve.
inline float reverb_wet_distance_attenuation(float dist_m, float min_distance_m, float max_distance_m) {
    if (!std::isfinite(dist_m))
        return 0.0f;
    const float d = (dist_m > 0.0f) ? dist_m : 0.0f;
    const float mn = std::fmax(sanitize_audio_float(min_distance_m), 0.0f);
    const float mx = std::fmax(sanitize_audio_float(max_distance_m), 0.0f);
    if (d <= mn)
        return 1.0f;
    const float effective_d = (d > 1e-6f) ? d : 1e-6f;
    float att = (mn > 0.0f) ? (mn / effective_d) : (1.0f / effective_d);
    if (mx > mn) {
        if (d >= mx)
            return 0.0f;
        const float fade_start = mn + 0.9f * (mx - mn);
        if (d > fade_start) {
            const float t = (d - fade_start) / (mx - fade_start);
            const float fade = 1.0f - std::fmin(std::fmax(t, 0.0f), 1.0f);
            att *= fade;
        }
    }
    if (att <= 0.0f)
        return 0.0f;
    if (att >= 1.0f)
        return 1.0f;
    return sanitize_audio_float(att);
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

/// Linear gain ramp across `num_samples` (parameter moves without zipper noise).
inline void apply_volume_ramp(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    // Fast path: uniform gain
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
