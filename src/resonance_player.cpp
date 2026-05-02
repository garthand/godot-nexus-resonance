#include "resonance_player.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_probe_volume.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <limits>
#include <sstream>

using namespace godot;

namespace {
/// Godot does not apply AudioStreamPlayer3D volume to GDExtension AudioStreamPlayback::_mix buffers; match Volume + max_db here.
float owner_effective_volume_linear(const ResonancePlayer* owner) {
    if (!owner)
        return 1.0f;
    return resonance::sanitize_audio_float(owner->get_effective_volume_linear_cached());
}

static void collect_collision_object_rids_recursive(Node* node, std::vector<RID>& out) {
    if (!node)
        return;
    auto* co = Object::cast_to<CollisionObject3D>(node);
    if (co)
        out.push_back(co->get_rid());
    const int nch = node->get_child_count();
    for (int i = 0; i < nch; ++i)
        collect_collision_object_rids_recursive(node->get_child(i), out);
}

bool ipl_all_channel_ptrs_ok(const IPLAudioBuffer& b, int nch) {
    if (!b.data || b.numChannels < nch)
        return false;
    for (int c = 0; c < nch; ++c) {
        if (!b.data[c])
            return false;
    }
    return true;
}

/// Fold one multichannel sample to stereo (Steam Audio layout: quad / 5.1 / 7.1 channel order).
void downmix_multichannel_sample(const float* const* d, int nch, int sample_idx, float& out_l, float& out_r) {
    constexpr float k = 0.70710678f;
    switch (nch) {
    case 1:
        out_l = out_r = d[0][sample_idx];
        break;
    case 2:
        out_l = d[0][sample_idx];
        out_r = d[1][sample_idx];
        break;
    case 4:
        out_l = d[0][sample_idx] + k * d[2][sample_idx];
        out_r = d[1][sample_idx] + k * d[3][sample_idx];
        break;
    case 6: // FL, FR, C, LFE, RL, RR
        out_l = d[0][sample_idx] + k * d[2][sample_idx] + k * d[4][sample_idx];
        out_r = d[1][sample_idx] + k * d[2][sample_idx] + k * d[5][sample_idx];
        break;
    case 8: // FL, FR, C, LFE, BL, BR, SL, SR
        out_l = d[0][sample_idx] + k * d[2][sample_idx] + k * d[4][sample_idx] + k * d[6][sample_idx];
        out_r = d[1][sample_idx] + k * d[2][sample_idx] + k * d[5][sample_idx] + k * d[7][sample_idx];
        break;
    default:
        out_l = out_r = 0.0f;
    }
}
} // namespace

// ============================================================================
// RESONANCE INTERNAL PLAYBACK
// ============================================================================

ResonanceStreamPlayback::ResonanceStreamPlayback() {
    params_next.apply_air_absorption = false;
    params_next.air_absorption[0] = 1.0f;
    params_next.air_absorption[1] = 1.0f;
    params_next.air_absorption[2] = 1.0f;
    params_next.apply_directivity = false;
    params_next.directivity_value = 1.0f;
    params_next.occlusion = 0.0f;
    params_next.transmission[0] = 1.0f;
    params_next.transmission[1] = 1.0f;
    params_next.transmission[2] = 1.0f;
    params_next.attenuation = 1.0f;
    params_next.listener_orientation.ahead = {0, 0, -1};
    params_next.listener_orientation.up = {0, 1, 0};
    params_next.listener_orientation.right = {1, 0, 0};
    params_next.listener_orientation.origin = {0, 0, 0};
    params_current = params_next;

    input_ring_l.resize(resonance::kRingBufferCapacity);
    input_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_l.resize(resonance::kRingBufferCapacity);
    output_ring_r.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_l.resize(resonance::kRingBufferCapacity);
    output_ring_reverb_r.resize(resonance::kRingBufferCapacity);

    // Temp buffers resized in _lazy_init to match frame_size_ from ResonanceServer
    temp_process_buffer_l.resize(resonance::kGodotDefaultFrameSize);
    temp_process_buffer_r.resize(resonance::kGodotDefaultFrameSize);

    temp_reverb_buffer_l.resize(resonance::kMaxAudioFrameSize);
    temp_reverb_buffer_r.resize(resonance::kMaxAudioFrameSize);

    // Clean struct init
    memset(&sa_in_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_direct_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_final_mix_buffer, 0, sizeof(IPLAudioBuffer));

    parametric_path_sh_coeffs[0] = resonance::kAmbisonicWChannelScale;
    parametric_path_sh_coeffs[1] = parametric_path_sh_coeffs[2] = parametric_path_sh_coeffs[3] = 0.0f;
}

ResonanceStreamPlayback::~ResonanceStreamPlayback() {
    if (owner_player_)
        owner_player_->internal_unregister_playback(this);
    _cleanup_steam_audio();
}

void ResonanceStreamPlayback::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceStreamPlayback*>(userdata)->_cleanup_steam_audio();
}

void ResonanceStreamPlayback::set_base_playback(const Ref<AudioStreamPlayback>& p_playback) {
    base_playback = p_playback;
}

void ResonanceStreamPlayback::update_parameters(const PlaybackParameters& p_params) {
    params_next = p_params;
    params_dirty.store(true, std::memory_order_release);
    // Opens the _mix gate. Before this point the audio thread must not emit samples because
    // params_current still holds defaults (no 3D position, no attenuation, no occlusion).
    params_ever_synced_.store(true, std::memory_order_release);
}

void ResonanceStreamPlayback::_sync_params() {
    if (params_dirty.load(std::memory_order_acquire)) {
        instrumentation_param_sync_count.fetch_add(1, std::memory_order_relaxed);
        params_current = params_next;
        params_dirty.store(false, std::memory_order_release);

        if (params_current.source_handle != current_source_handle) {
            if (local_source) {
                iplSourceRelease(&local_source);
                local_source = nullptr;
            }
            current_source_handle = params_current.source_handle;
            ResonanceServer* srv = ResonanceServer::get_singleton();
            if (srv && current_source_handle >= 0) {
                local_source = srv->get_source_from_handle(current_source_handle);
            }
        }
    }
}

void ResonanceStreamPlayback::_cleanup_steam_audio() {
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->unregister_ipl_context_client(this);

    if (local_source) {
        iplSourceRelease(&local_source);
        local_source = nullptr;
    }

    direct_processor.cleanup();
    reflection_processor.cleanup();
    path_processor.cleanup();
    mixer_processor.cleanup();

    if (context) {
        if (sa_in_buffer.data)
            iplAudioBufferFree(context, &sa_in_buffer);
        if (sa_direct_out_buffer.data)
            iplAudioBufferFree(context, &sa_direct_out_buffer);
        if (sa_path_out_buffer.data)
            iplAudioBufferFree(context, &sa_path_out_buffer);
        if (sa_final_mix_buffer.data)
            iplAudioBufferFree(context, &sa_final_mix_buffer);
    }

    // IMPORTANT: Reset structs to 0 to prevent double-free or invalid access
    memset(&sa_in_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_direct_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_path_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_final_mix_buffer, 0, sizeof(IPLAudioBuffer));

    is_initialized = false;
    direct_out_channels_ = 2;

    input_ring_l.clear();
    input_ring_r.clear();
    output_ring_l.clear();
    output_ring_r.clear();
    output_ring_reverb_l.clear();
    output_ring_reverb_r.clear();

    prev_direct_weight = 1.0f;
    prev_conv_reflections_mix_level_ = -1.0f;
    prev_parametric_reflections_mix_level_ = 1.0f;
    prev_pathing_mix_level_ = 1.0f;
    input_started = false;
    params_ever_synced_.store(false, std::memory_order_release);
}

void ResonanceStreamPlayback::_lazy_init_steam_audio(int ignored_rate) {
    (void)ignored_rate; // Sample rate comes from ResonanceServer::get_sample_rate(); param kept for API consistency.
    if (is_initialized)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;

    current_sample_rate = srv->get_sample_rate();
    frame_size_ = srv->get_audio_frame_size();
    context = srv->get_context_handle();
    int order = srv->get_ambisonic_order();
    int refl_type = srv->get_reflection_type();
    direct_out_channels_ = srv->get_direct_speaker_channels();
    if (direct_out_channels_ < 1)
        direct_out_channels_ = 2;

    temp_process_buffer_l.resize(frame_size_);
    temp_process_buffer_r.resize(frame_size_);

    // 1. Initialize Direct (always create Ambisonics Encode path for runtime switching)
    direct_processor.initialize(context, current_sample_rate, frame_size_, order, true, direct_out_channels_);

    // 2. Initialize Reflection (convolution or parametric)
    reflection_processor.initialize(context, current_sample_rate, frame_size_, order, refl_type, srv->get_max_reverb_duration(),
                                    srv->get_convolution_ir_max_samples());

    // 3. Initialize Path Processor (for pathing simulation)
    path_processor.initialize(context, current_sample_rate, frame_size_, order);
    // 4. Initialize Mixer Processor (for convolution ambisonic decode)
    mixer_processor.initialize(context, current_sample_rate, frame_size_, order);

    // 5. Allocate Buffers (direct path matches server speaker layout; path processor stays stereo)
    if (iplAudioBufferAllocate(context, 2, frame_size_, &sa_in_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_direct_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, 2, frame_size_, &sa_path_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_final_mix_buffer) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonancePlayer: Playback Init Failed: Buffer allocation failed (IPLerror).");
        _cleanup_steam_audio();
        return;
    }
    if (!ipl_all_channel_ptrs_ok(sa_in_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_) ||
        !ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_)) {
        ResonanceLog::error("ResonancePlayer: Playback Init Failed: Buffer allocation returned null.");
        _cleanup_steam_audio();
        return;
    }

    is_initialized = true;
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->register_ipl_context_client(this, &ResonanceStreamPlayback::ipl_context_reinit_cleanup);
    ResonanceLog::info("Playback Initialized.");
}

bool ResonanceStreamPlayback::prewarm_steam_audio() {
    // Phase 4: main-thread pre-allocation. _lazy_init_steam_audio is idempotent after the initial is_initialized guard.
    // Skipping the audio-thread lazy allocation cost removes a multi-millisecond stall on the first _mix block for each
    // polyphonic voice, which previously caused audible artefacts on play() when the worker was mid-heavy-tick.
    if (is_initialized)
        return true;
    _lazy_init_steam_audio(0);
    return is_initialized;
}

void ResonanceStreamPlayback::_add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix, bool split_output,
                                                    const IPLCoordinateSpace3& listener_coords) {
    if (reflection_processor.is_parametric()) {
        for (int i = 0; i < frame_size_; i++) {
            float mono = reverb_buf->data[0][i] * refl_mix;
            if (split_output) {
                output_ring_reverb_l.write(&mono, 1);
                output_ring_reverb_r.write(&mono, 1);
            } else {
                if (sa_final_mix_buffer.data[0])
                    sa_final_mix_buffer.data[0][i] += mono;
                if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                    sa_final_mix_buffer.data[1][i] += mono;
            }
        }
    } else {
        AudioFrame reverb_frames[resonance::kMaxAudioFrameSize];
        memset(reverb_frames, 0, sizeof(reverb_frames));
        bool decode_ok = mixer_processor.decode_ambisonic_to_stereo(reverb_buf, listener_coords, reverb_frames, frame_size_);
        if (decode_ok) {
            for (int i = 0; i < frame_size_; i++) {
                float l = reverb_frames[i].left * refl_mix;
                float r = reverb_frames[i].right * refl_mix;
                if (split_output) {
                    output_ring_reverb_l.write(&l, 1);
                    output_ring_reverb_r.write(&r, 1);
                } else {
                    if (sa_final_mix_buffer.data[0])
                        sa_final_mix_buffer.data[0][i] += l;
                    if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                        sa_final_mix_buffer.data[1][i] += r;
                }
            }
        }
    }
}

void ResonanceStreamPlayback::_zero_sa_final_mix() {
    if (!sa_final_mix_buffer.data)
        return;
    for (int c = 0; c < direct_out_channels_ && c < sa_final_mix_buffer.numChannels; ++c) {
        if (sa_final_mix_buffer.data[c])
            memset(sa_final_mix_buffer.data[c], 0, frame_size_ * sizeof(float));
    }
}

void ResonanceStreamPlayback::_write_output_rings_folded() {
    if (!sa_final_mix_buffer.data || direct_out_channels_ < 1)
        return;
    if (direct_out_channels_ == 1 && sa_final_mix_buffer.data[0]) {
        output_ring_l.write(sa_final_mix_buffer.data[0], frame_size_);
        output_ring_r.write(sa_final_mix_buffer.data[0], frame_size_);
        return;
    }
    if (direct_out_channels_ == 2 && sa_final_mix_buffer.data[0] && sa_final_mix_buffer.data[1]) {
        output_ring_l.write(sa_final_mix_buffer.data[0], frame_size_);
        output_ring_r.write(sa_final_mix_buffer.data[1], frame_size_);
        return;
    }
    for (int i = 0; i < frame_size_; ++i) {
        float ol = 0.0f;
        float orv = 0.0f;
        downmix_multichannel_sample(sa_final_mix_buffer.data, direct_out_channels_, i, ol, orv);
        temp_process_buffer_l[i] = ol;
        temp_process_buffer_r[i] = orv;
    }
    output_ring_l.write(temp_process_buffer_l.data(), frame_size_);
    output_ring_r.write(temp_process_buffer_r.data(), frame_size_);
}

void ResonanceStreamPlayback::_process_steam_audio_block() {
    auto t0 = std::chrono::steady_clock::now();

    // Process-entry guards: centralised null checks at audio hot path entry
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!context || !srv || !srv->is_initialized())
        return;

    const float node_vol = owner_effective_volume_linear(owner_player_);
    if (!ipl_all_channel_ptrs_ok(sa_in_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_))
        return;

    // 1. Read RingBuffer
    input_ring_l.read(temp_process_buffer_l.data(), frame_size_);
    input_ring_r.read(temp_process_buffer_r.data(), frame_size_);

    // 2. Copy to Steam Audio Buffer (CRITICAL POINT)
    memcpy(sa_in_buffer.data[0], temp_process_buffer_l.data(), frame_size_ * sizeof(float));
    memcpy(sa_in_buffer.data[1], temp_process_buffer_r.data(), frame_size_ * sizeof(float));

    // Input-start detection: delay processing until first non-zero sample to avoid ramp artifacts
    // when Godot sends incorrect params before playback actually starts.
    // Steam Audio Unity spatialize_effect.cpp: fabsf(in[i]) != 0.0f (per sample, not exact == 0).
    if (!input_started) {
        for (int i = 0; i < frame_size_; i++) {
            if (std::fabs(temp_process_buffer_l[i]) != 0.0f || std::fabs(temp_process_buffer_r[i]) != 0.0f) {
                input_started = true;
                break;
            }
        }
        if (!input_started) {
            _zero_sa_final_mix();
            _write_output_rings_folded();
            return;
        }
    }

    if (!srv->is_spatial_audio_output_ready()) {
        _zero_sa_final_mix();
        _write_output_rings_folded();
        return;
    }

    // Steam pathing/direct/reflection use server-side params by handle; the block body does not read `local_source`.
    // Gating on `local_source` alone skipped the whole pipeline when `get_source_from_handle` returned null (H12/H13).
    if (current_source_handle >= 0) {
        float dbg_direct = 0.0f;
        float dbg_reverb = 0.0f;
        float dbg_path = 0.0f;

        instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);

        const float wet_rmd = resonance::reverb_wet_falloff_max_distance(params_current.distance, srv->get_reverb_max_distance());
        // Additional wet-input occlusion for baked REVERB (see PlaybackParameters::wet_occlusion_factor).
        // 1.0 = no extra damping (default for realtime / STATICSOURCE / STATICLISTENER or when the feature is off).
        const float wet_occ = resonance::sanitize_audio_float(params_current.wet_occlusion_factor);
        // Unity-parity 3D distance curve on the wet input. 1.0 when opt-out (see PlaybackParameters::refl_distance_attenuation).
        const float refl_dist_att = resonance::sanitize_audio_float(params_current.refl_distance_attenuation);

        // One listener frame for all Phonon spatialization (Steam Audio Unity: single listenermatrix → calcListenerCoordinates).
        // ResonanceAudioEffect / iplAmbisonicsDecodeEffectApply use get_current_listener_coords(); params_current.listener_orientation
        // can lag by a sync tick — mismatched HOA rotation vs direct sum causes comb-filtering / crackle at master.
        const IPLCoordinateSpace3 listener_cs = srv->get_current_listener_coords();

        // 3. Run Direct Processor (srv already validated by process-entry guards)
        int trans_type = params_current.direct_effect_transmission_type;
        bool hrtf_bilinear = params_current.direct_effect_hrtf_bilinear;
        direct_processor.process(
            params_current.use_ambisonics_encode,
            sa_in_buffer, sa_direct_out_buffer,
            params_current.attenuation,
            params_current.occlusion,
            params_current.transmission,
            params_current.air_absorption,
            params_current.apply_air_absorption,
            params_current.directivity_value,
            params_current.apply_directivity,
            params_current.enable_direct,
            params_current.use_binaural,
            trans_type,
            hrtf_bilinear,
            params_current.spatial_blend,
            listener_cs,
            ResonanceUtils::to_ipl_vector3(params_current.source_position));
        // 4. Reflection / reverb (reference: Steam Audio Unity native spatialize_effect.cpp process()).
        // Raw sa_in_buffer feeds iplReflectionEffectApply / pathing, same as Unity's inBuffer (not directBuffer).
        // Pre-effect gain on the reflection mixer path: reflections_mix_level * linear source volume; Unity does not
        // multiply by reverb_pathing_attenuation here. When runtime reverb_max_distance > 0, apply extra wet falloff
        // (see ResonanceRuntimeConfig.reverb_max_distance).
        bool reverb_to_player_output = false; // Only for Parametric/Hybrid
        const float refl_wet_output_gain = wet_rmd;
        if (srv && params_current.enable_reverb) {
            IPLReflectionEffectParams reverb_params{};
            bool has_reverb = srv->fetch_reverb_params(current_source_handle, reverb_params);
            const int refl_type = srv->get_reflection_type();

            if (has_reverb) {
                if (refl_type == resonance::kReflectionHybrid) {
                    if (params_current.reflections_eq[0] != 1.0f || params_current.reflections_eq[1] != 1.0f || params_current.reflections_eq[2] != 1.0f) {
                        reverb_params.eq[0] *= params_current.reflections_eq[0];
                        reverb_params.eq[1] *= params_current.reflections_eq[1];
                        reverb_params.eq[2] *= params_current.reflections_eq[2];
                    }
                    if (params_current.reflections_delay >= 0) {
                        reverb_params.delay = params_current.reflections_delay;
                    }
                }

                // Convolution (0) / TAN (3): Feed mixer only; Reverb Bus reads it.
                // Parametric (1) / Hybrid (2): process_mix_direct and add to our output.
                if (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan) {
                    auto mixer_guard = srv->scoped_mixer_read();
                    IPLReflectionMixer mixer = mixer_guard.get();
                    if (mixer) {
                        const float curr_refl_mix = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                        const float wet_extra = resonance::sanitize_audio_float(node_vol * refl_dist_att * wet_rmd * wet_occ);
                        const float conv_reverb_gain = curr_refl_mix * wet_extra;
                        dbg_reverb = conv_reverb_gain;
                        // Mono RMS of input for reverb-bus instrumentation (editor / template_debug only; see DEBUG_ENABLED in godot-cpp).
                        float input_rms = 0.0f;
#ifdef DEBUG_ENABLED
                        float sum_sq = 0.0f;
                        int nch = sa_in_buffer.numChannels;
                        if (nch > 0 && sa_in_buffer.data) {
                            for (int i = 0; i < frame_size_; i++) {
                                float mono = 0.0f;
                                for (int c = 0; c < nch && sa_in_buffer.data[c]; c++)
                                    mono += sa_in_buffer.data[c][i];
                                mono /= static_cast<float>(nch);
                                sum_sq += mono * mono;
                            }
                        }
                        input_rms = (frame_size_ > 0) ? std::sqrt(sum_sq / static_cast<float>(frame_size_)) : 0.0f;
#endif
                        const auto conv_apply_t0 = std::chrono::steady_clock::now();
                        const bool reflection_applied =
                            reflection_processor.process_mix(sa_in_buffer, reverb_params, mixer, prev_conv_reflections_mix_level_, curr_refl_mix, wet_extra);
                        const auto conv_apply_t1 = std::chrono::steady_clock::now();
                        if (reflection_applied) {
                            srv->record_convolution_reflection_apply_usec(static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(conv_apply_t1 - conv_apply_t0).count()));
                            srv->record_convolution_feed(reverb_params.ir != nullptr, conv_reverb_gain, input_rms);
                            prev_conv_reflections_mix_level_ = curr_refl_mix;
                            srv->record_mixer_feed();
                            reflection_tail_params_ = reverb_params;
                            reflection_tail_have_params_ = true;
                            reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
                            reflection_tail_split_output_ = params_current.reverb_split_output;
                        }
                    }
                } else {
                    reverb_to_player_output = true;
                    const float parametric_mix_level = resonance::sanitize_audio_float(params_current.reflections_mix_level * refl_dist_att * wet_occ);
                    if (reflection_processor.process_mix_direct(sa_in_buffer, reverb_params, prev_parametric_reflections_mix_level_, parametric_mix_level)) {
                        prev_parametric_reflections_mix_level_ = parametric_mix_level;
                        reflection_tail_params_ = reverb_params;
                        reflection_tail_have_params_ = true;
                        reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
                        reflection_tail_split_output_ = params_current.reverb_split_output;
                    }
                }
            } else if (reflection_tail_have_params_ &&
                       (refl_type == resonance::kReflectionParametric || refl_type == resonance::kReflectionHybrid)) {
                // fetch_reverb_params can fail (simulation lock, pending source, etc.). Skipping Apply for a whole
                // Steam frame leaves dry advancing but wet frozen — heard as block-rate pumping. Keep advancing the
                // effect with last-known room params; wet output level matches Unity (no extra distance/air scale).
                IPLReflectionEffectParams rp = reflection_tail_params_;
                if (refl_type == resonance::kReflectionHybrid && params_current.reflections_delay >= 0)
                    rp.delay = params_current.reflections_delay;
                reverb_to_player_output = true;
                const float parametric_mix_level_stale = resonance::sanitize_audio_float(params_current.reflections_mix_level * refl_dist_att * wet_occ);
                if (reflection_processor.process_mix_direct(sa_in_buffer, rp, prev_parametric_reflections_mix_level_, parametric_mix_level_stale)) {
                    prev_parametric_reflections_mix_level_ = parametric_mix_level_stale;
                    reflection_tail_wet_gain_ = resonance::sanitize_audio_float(wet_rmd);
                    reflection_tail_split_output_ = params_current.reverb_split_output;
                }
            } else {
                instrumentation_reverb_miss_blocks.fetch_add(1, std::memory_order_relaxed);
                // Only the reflections wet path needs fetch_reverb_params; pathing-only (reflections_mix == 0) misses here are normal.
                const float refl_mix_gate = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                if (refl_mix_gate > 0.0f) {
                    // Throttle: skip first N misses, then log only after threshold (reset to 0 after log)
                    if (++no_reverb_warn_count <= resonance::kPlayerNoReverbWarnSkipInitial || no_reverb_warn_count == resonance::kPlayerNoReverbWarnThreshold) { /* skip log */
                    }
                    if (no_reverb_warn_count > resonance::kPlayerNoReverbWarnThreshold) {
                        String player_label = "(unknown)";
                        if (owner_player_) {
                            player_label = owner_player_->get_name();
                            if (player_label.is_empty())
                                player_label = "(unnamed ResonancePlayer)";
                        }
                        const int rt = srv->get_reflection_type();
                        String detail;
                        if (rt == resonance::kReflectionConvolution || rt == resonance::kReflectionTan) {
                            detail = "Convolution/TAN: worker reflection cache not ready yet, cache miss, source still attaching, or realtime reflections turned off for this source (mix/gating). Not related to baked probes.";
                        } else if (rt == resonance::kReflectionParametric || rt == resonance::kReflectionHybrid) {
                            detail = "Parametric/Hybrid: wait for RunReflections to populate params; for baked reverb also verify probes are baked and the source lies within probe influence / range.";
                        } else {
                            detail = "Check reflection mode and simulation state.";
                        }
                        String msg = String("Playback (`") + player_label + "`): No reflection effect params from simulation while reflections_mix > 0. " + detail;
                        ResonanceLog::warn(msg);
                        no_reverb_warn_count = 0;
                    }
                } else {
                    no_reverb_warn_count = 0;
                }
            }
        }

        // Apply Volume Ramping (Direct Only). Use mix level: enable_direct * direct_mix_level
        float target_direct = (params_current.enable_direct ? 1.0f : 0.0f) * params_current.direct_mix_level;

        // Effective direct gain (matches iplDirectEffectApply physics) for debug display
        float eff_gain = params_current.attenuation;
        float trans_avg = (params_current.transmission[0] + params_current.transmission[1] + params_current.transmission[2]) / 3.0f;
        float occ_factor = params_current.occlusion + (1.0f - params_current.occlusion) * trans_avg;
        eff_gain *= occ_factor;
        if (params_current.apply_directivity)
            eff_gain *= params_current.directivity_value;
        if (params_current.apply_air_absorption) {
            float air_avg = (params_current.air_absorption[0] + params_current.air_absorption[1] + params_current.air_absorption[2]) / 3.0f;
            eff_gain *= air_avg;
        }
        eff_gain *= target_direct;
        dbg_direct = resonance::sanitize_audio_float(std::clamp(eff_gain, 0.0f, 1.0f));

        // Apply to Direct (all speaker channels)
        for (int c = 0; c < direct_out_channels_; c++) {
            if (sa_direct_out_buffer.data[c])
                resonance::apply_volume_ramp(prev_direct_weight, target_direct, frame_size_, sa_direct_out_buffer.data[c]);
        }
        prev_direct_weight = target_direct;

        // Mix Direct into final buffer
        for (int c = 0; c < direct_out_channels_; c++) {
            if (sa_direct_out_buffer.data[c] && sa_final_mix_buffer.data[c])
                memcpy(sa_final_mix_buffer.data[c], sa_direct_out_buffer.data[c], frame_size_ * sizeof(float));
        }

        // Add Reverb to player output (Parametric/Hybrid only; Convolution feeds mixer, no fallback)
        // When reverb_split_output: write to reverb ring for separate bus; else mix into final.
        if (reverb_to_player_output) {
            IPLAudioBuffer* reverb_buf = reflection_processor.get_direct_output_buffer();
            if (reverb_buf && reverb_buf->data) {
                // reflections_mix_level was ramped on mono before iplReflectionEffectApply (Unity spatialize path).
                float refl_mix = resonance::sanitize_audio_float(refl_wet_output_gain);
                dbg_reverb = resonance::sanitize_audio_float(params_current.reflections_mix_level * node_vol * refl_dist_att * wet_rmd * wet_occ);
                _add_reverb_to_output(reverb_buf, refl_mix, params_current.reverb_split_output, listener_cs);
            }
        }

        // Add Pathing (multi-path sound propagation around obstacles).
        // Pathing only when enable_reverb is true – pathing is indirect sound (reflections); requires reverb to be active.
        // Mix level: ramp on mono input before iplPathEffectApply (Steam Audio Unity/FMOD). Do not multiply wet by
        // reverb_pathing_attenuation — path SH already include per-path distance attenuation in the simulation.
        if (srv && srv->is_pathing_enabled() && params_current.enable_reverb && params_current.pathing_mix_level > 0.0f) {
            srv->record_pathing_player_gate_enter();
            IPLPathEffectParams path_params{};
            bool use_pathing = srv->fetch_pathing_params(current_source_handle, path_params);
            if (use_pathing) {
                path_params.listener = listener_cs;
                if (!params_current.apply_hrtf_to_pathing) {
                    path_params.hrtf = nullptr;
                    path_params.binaural = IPL_FALSE;
                }
                // Cache params for EOS tail. Deep-copy SH coeffs so the pointer remains valid even if the server swaps caches.
                pathing_tail_params_ = path_params;
                pathing_tail_have_params_ = true;
                if (path_params.shCoeffs && path_params.order >= 0) {
                    const int n = (path_params.order + 1) * (path_params.order + 1);
                    const int to_copy = std::min(n, static_cast<int>(pathing_tail_sh_coeffs_.size()));
                    for (int i = 0; i < to_copy; i++)
                        pathing_tail_sh_coeffs_[static_cast<size_t>(i)] = path_params.shCoeffs[i];
                    for (size_t i = static_cast<size_t>(to_copy); i < pathing_tail_sh_coeffs_.size(); i++)
                        pathing_tail_sh_coeffs_[i] = 0.0f;
                    pathing_tail_params_.shCoeffs = pathing_tail_sh_coeffs_.data();
                } else {
                    for (size_t i = 0; i < pathing_tail_sh_coeffs_.size(); i++)
                        pathing_tail_sh_coeffs_[i] = 0.0f;
                    pathing_tail_params_.shCoeffs = pathing_tail_sh_coeffs_.data();
                }
                const int32_t path_order = path_params.order;
                instrumentation_last_pathing_order.store(path_order, std::memory_order_relaxed);
                if (path_params.shCoeffs && path_order >= 0) {
                    const int n = (path_order + 1) * (path_order + 1);
                    double sum_sq = 0.0;
                    for (int i = 0; i < n; i++) {
                        const double c = static_cast<double>(path_params.shCoeffs[i]);
                        sum_sq += c * c;
                    }
                    const float energy = static_cast<float>(sum_sq);
                    const float sh_rms = (n > 0) ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n))) : 0.0f;
                    instrumentation_last_pathing_sh_energy.store(energy, std::memory_order_relaxed);
                    instrumentation_last_pathing_sh_rms.store(sh_rms, std::memory_order_relaxed);
                }
                if (sa_path_out_buffer.data[0])
                    memset(sa_path_out_buffer.data[0], 0, frame_size_ * sizeof(float));
                if (sa_path_out_buffer.data[1])
                    memset(sa_path_out_buffer.data[1], 0, frame_size_ * sizeof(float));
                path_processor.process(sa_in_buffer, path_params, sa_path_out_buffer, prev_pathing_mix_level_,
                                       params_current.pathing_mix_level);
                float path_sum_sq = 0.0f;
                for (int i = 0; i < frame_size_; i++) {
                    const float pl = sa_path_out_buffer.data[0][i];
                    const float pr = sa_path_out_buffer.data[1][i];
                    path_sum_sq += pl * pl + pr * pr;
                }
                const float path_out_rms = (frame_size_ > 0) ? std::sqrt(path_sum_sq / (2.0f * static_cast<float>(frame_size_))) : 0.0f;
                instrumentation_last_pathing_out_rms.store(path_out_rms, std::memory_order_relaxed);
                dbg_path = resonance::sanitize_audio_float(std::clamp(path_out_rms, 0.0f, 1.0f));
                for (int i = 0; i < frame_size_; i++) {
                    if (sa_final_mix_buffer.data[0])
                        sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
                    if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                        sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
                }
                prev_pathing_mix_level_ = params_current.pathing_mix_level;
                srv->record_pathing_player_applied();
            } else {
                srv->record_pathing_player_fetch_miss();
            }
        } else {
            prev_pathing_mix_level_ = params_current.pathing_mix_level;
        }

        debug_signal_direct.store(dbg_direct, std::memory_order_relaxed);
        debug_signal_reverb.store(dbg_reverb, std::memory_order_relaxed);
        debug_signal_pathing.store(dbg_path, std::memory_order_relaxed);
    } else {
        // Passthrough: no active Steam source handle for this playback
        debug_signal_direct.store(1.0f, std::memory_order_relaxed);
        debug_signal_reverb.store(0.0f, std::memory_order_relaxed);
        debug_signal_pathing.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);
        instrumentation_passthrough_blocks.fetch_add(1, std::memory_order_relaxed);
        if (sa_final_mix_buffer.data[0] && sa_in_buffer.data[0])
            memcpy(sa_final_mix_buffer.data[0], sa_in_buffer.data[0], frame_size_ * sizeof(float));
        if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1] && sa_in_buffer.data[1])
            memcpy(sa_final_mix_buffer.data[1], sa_in_buffer.data[1], frame_size_ * sizeof(float));
        for (int c = 2; c < direct_out_channels_; c++) {
            if (sa_final_mix_buffer.data[c])
                memset(sa_final_mix_buffer.data[c], 0, frame_size_ * sizeof(float));
        }

        // Reset Ramps if we lost source, so next time it fades in cleanly
        prev_direct_weight = 1.0f;
        prev_parametric_reflections_mix_level_ = 1.0f;
        prev_pathing_mix_level_ = 1.0f;
        // Convolution/TAN mixer feed uses separate prev; re-arm "first block" when source reattaches
        // (Unity rebinds simulation source; we must not carry a stale reflections-mix ramp across null handle).
        prev_conv_reflections_mix_level_ = -1.0f;
    }

    // Safety: clamp output to prevent NaN/overflow from processing bugs
    for (int c = 0; c < direct_out_channels_; c++) {
        if (!sa_final_mix_buffer.data[c])
            continue;
        for (int i = 0; i < frame_size_; i++)
            sa_final_mix_buffer.data[c][i] = std::clamp(sa_final_mix_buffer.data[c][i], -1.0f, 1.0f);
    }

    // Instrumentation: output RMS and silent-block detection (stereo fold-down; matches ring samples)
    float sum_sq = 0.0f;
    for (int i = 0; i < frame_size_; i++) {
        float l = 0.0f;
        float r = 0.0f;
        downmix_multichannel_sample(sa_final_mix_buffer.data, direct_out_channels_, i, l, r);
        sum_sq += l * l + r * r;
    }
    float rms = (frame_size_ > 0) ? std::sqrt(sum_sq / (2.0f * static_cast<float>(frame_size_))) : 0.0f;
    instrumentation_last_output_rms_q8.store((uint32_t)(rms * 256.0f), std::memory_order_relaxed);
    if (current_source_handle >= 0 && rms < resonance::kInstrumentationSilentBlockThreshold)
        instrumentation_silent_output_blocks.fetch_add(1, std::memory_order_relaxed);

    _write_output_rings_folded();

    auto t1 = std::chrono::steady_clock::now();
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    uint64_t cur_max = instrumentation_max_block_time_us.load(std::memory_order_relaxed);
    while (us > cur_max && !instrumentation_max_block_time_us.compare_exchange_weak(cur_max, us, std::memory_order_relaxed)) {
    }
}

void ResonanceStreamPlayback::get_instrumentation_snapshot(uint64_t& out_input_dropped, uint64_t& out_output_underrun,
                                                           uint64_t& out_output_blocked, uint64_t& out_mix_calls, uint64_t& out_blocks_processed,
                                                           uint64_t& out_passthrough_blocks, uint64_t& out_reverb_miss_blocks, uint64_t& out_max_block_time_us,
                                                           uint64_t& out_late_mix, uint64_t& out_last_mix_gap_us, uint64_t& out_max_mix_gap_us,
                                                           uint64_t& out_expected_mix_gap_us, uint64_t& out_param_syncs, uint64_t& out_zero_input,
                                                           int32_t& out_mix_frames_min, int32_t& out_mix_frames_max,
                                                           uint64_t& out_silent_blocks, float& out_last_rms,
                                                           float& out_pathing_sh_rms, float& out_pathing_sh_energy, float& out_pathing_out_rms,
                                                           int32_t& out_pathing_order) const {
    out_input_dropped = instrumentation_input_dropped.load(std::memory_order_relaxed);
    out_output_underrun = instrumentation_output_underrun.load(std::memory_order_relaxed);
    out_output_blocked = instrumentation_output_blocked.load(std::memory_order_relaxed);
    out_mix_calls = instrumentation_mix_call_count.load(std::memory_order_relaxed);
    out_blocks_processed = instrumentation_blocks_processed.load(std::memory_order_relaxed);
    out_passthrough_blocks = instrumentation_passthrough_blocks.load(std::memory_order_relaxed);
    out_reverb_miss_blocks = instrumentation_reverb_miss_blocks.load(std::memory_order_relaxed);
    out_max_block_time_us = instrumentation_max_block_time_us.load(std::memory_order_relaxed);
    out_late_mix = instrumentation_late_mix_count.load(std::memory_order_relaxed);
    out_last_mix_gap_us = instrumentation_last_mix_gap_us_.load(std::memory_order_relaxed);
    out_max_mix_gap_us = instrumentation_max_mix_gap_us_.load(std::memory_order_relaxed);
    out_expected_mix_gap_us = instrumentation_expected_mix_gap_us_.load(std::memory_order_relaxed);
    out_param_syncs = instrumentation_param_sync_count.load(std::memory_order_relaxed);
    out_zero_input = instrumentation_zero_input_count.load(std::memory_order_relaxed);
    out_mix_frames_min = instrumentation_mix_frames_min.load(std::memory_order_relaxed);
    out_mix_frames_max = instrumentation_mix_frames_max.load(std::memory_order_relaxed);
    out_silent_blocks = instrumentation_silent_output_blocks.load(std::memory_order_relaxed);
    out_last_rms = instrumentation_last_output_rms_q8.load(std::memory_order_relaxed) / 256.0f;
    out_pathing_sh_rms = instrumentation_last_pathing_sh_rms.load(std::memory_order_relaxed);
    out_pathing_sh_energy = instrumentation_last_pathing_sh_energy.load(std::memory_order_relaxed);
    out_pathing_out_rms = instrumentation_last_pathing_out_rms.load(std::memory_order_relaxed);
    out_pathing_order = instrumentation_last_pathing_order.load(std::memory_order_relaxed);
}

// out_pathing: path wet stereo RMS after simulation, clamped to [0,1] (not pathing_mix_level).
void ResonanceStreamPlayback::get_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) const {
    out_direct = debug_signal_direct.load(std::memory_order_relaxed);
    out_reverb = debug_signal_reverb.load(std::memory_order_relaxed);
    out_pathing = debug_signal_pathing.load(std::memory_order_relaxed);
}

int32_t ResonanceStreamPlayback::read_reverb_frames(AudioFrame* buffer, int32_t frames) {
    if (!buffer || frames <= 0)
        return 0;
    size_t avail = output_ring_reverb_l.get_available_read();
    int32_t to_read = (int32_t)std::min((size_t)frames, avail);
    to_read = std::min(to_read, (int32_t)resonance::kMaxAudioFrameSize);
    if (to_read <= 0) {
        // Ring ran dry: if last callback ended non-zero, fade to zero to avoid a click.
        if (reverb_ring_prev_valid_ && (std::abs(reverb_ring_prev_l_) > 1.0e-6f || std::abs(reverb_ring_prev_r_) > 1.0e-6f)) {
            const float v_rev = owner_effective_volume_linear(owner_player_);
            const float start_l = reverb_ring_prev_l_ * v_rev;
            const float start_r = reverb_ring_prev_r_ * v_rev;
            for (int32_t i = 0; i < frames; i++) {
                const float fade = 1.0f - (float(i + 1) / float(frames));
                buffer[i].left = start_l * fade;
                buffer[i].right = start_r * fade;
            }
        } else {
            for (int32_t i = 0; i < frames; i++) {
                buffer[i].left = 0.0f;
                buffer[i].right = 0.0f;
            }
        }
        reverb_ring_prev_l_ = 0.0f;
        reverb_ring_prev_r_ = 0.0f;
        reverb_ring_prev_valid_ = true;
        return frames;
    }
    output_ring_reverb_l.read(temp_reverb_buffer_l.data(), to_read);
    output_ring_reverb_r.read(temp_reverb_buffer_r.data(), to_read);
    const float v_rev = owner_effective_volume_linear(owner_player_);
    for (int32_t i = 0; i < to_read; i++) {
        buffer[i].left = temp_reverb_buffer_l[i] * v_rev;
        buffer[i].right = temp_reverb_buffer_r[i] * v_rev;
    }
    // If caller asked for more than available, fade the remainder to 0 instead of hard zero-pad.
    if (to_read < frames) {
        const float start_l = buffer[to_read - 1].left;
        const float start_r = buffer[to_read - 1].right;
        const int32_t rem = frames - to_read;
        for (int32_t k = 0; k < rem; k++) {
            const float fade = 1.0f - (float(k + 1) / float(rem));
            buffer[to_read + k].left = start_l * fade;
            buffer[to_read + k].right = start_r * fade;
        }
        reverb_ring_prev_l_ = 0.0f;
        reverb_ring_prev_r_ = 0.0f;
        reverb_ring_prev_valid_ = true;
    } else {
        reverb_ring_prev_l_ = temp_reverb_buffer_l[to_read - 1];
        reverb_ring_prev_r_ = temp_reverb_buffer_r[to_read - 1];
        reverb_ring_prev_valid_ = true;
    }
    return frames;
}

void ResonanceStreamPlayback::reset_instrumentation() {
    instrumentation_input_dropped.store(0, std::memory_order_relaxed);
    instrumentation_output_underrun.store(0, std::memory_order_relaxed);
    instrumentation_output_blocked.store(0, std::memory_order_relaxed);
    instrumentation_mix_call_count.store(0, std::memory_order_relaxed);
    instrumentation_blocks_processed.store(0, std::memory_order_relaxed);
    instrumentation_passthrough_blocks.store(0, std::memory_order_relaxed);
    instrumentation_reverb_miss_blocks.store(0, std::memory_order_relaxed);
    instrumentation_max_block_time_us.store(0, std::memory_order_relaxed);
    instrumentation_late_mix_count.store(0, std::memory_order_relaxed);
    instrumentation_last_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_max_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_expected_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_param_sync_count.store(0, std::memory_order_relaxed);
    instrumentation_zero_input_count.store(0, std::memory_order_relaxed);
    instrumentation_mix_frames_min.store(std::numeric_limits<int32_t>::max(), std::memory_order_relaxed);
    instrumentation_mix_frames_max.store(0, std::memory_order_relaxed);
    instrumentation_silent_output_blocks.store(0, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);
}

int32_t ResonanceStreamPlayback::_mix(AudioFrame* buffer, float rate_scale, int32_t frames) {
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        return frames;
    }
    if (base_playback.is_null())
        return 0;
    // First-params gate: before ResonancePlayer has pushed the first spatial parameters,
    // params_current still holds defaults (listener-position, full gain), which would cause
    // an audible full-volume burst for one block. Silence output until parameters arrive.
    //
    // IMPORTANT: Still advance the base playback while gated; otherwise the stream never
    // reaches EOS and Godot never emits `finished`.
    if (!params_ever_synced_.load(std::memory_order_acquire)) {
        // Advance/discard one block from the base playback.
        const Ref<AudioStreamPlayback> base_gate_guard = base_playback;
        if (base_gate_guard.is_valid()) {
            (void)base_gate_guard->mix_audio(rate_scale, frames);
        }
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        return frames;
    }
    // (debug logging removed)
    auto now = std::chrono::steady_clock::now();
    instrumentation_mix_call_count.fetch_add(1, std::memory_order_relaxed);
    if (instrumentation_mix_call_count.load(std::memory_order_relaxed) > 1) {
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_mix_time_).count();
        const uint64_t gap_us = (elapsed_us > 0) ? static_cast<uint64_t>(elapsed_us) : 0;
        instrumentation_last_mix_gap_us_.store(gap_us, std::memory_order_relaxed);
        uint64_t prev_max = instrumentation_max_mix_gap_us_.load(std::memory_order_relaxed);
        if (gap_us > prev_max)
            instrumentation_max_mix_gap_us_.store(gap_us, std::memory_order_relaxed);
        if (current_sample_rate > 0 && frames > 0) {
            const uint64_t expected_us = static_cast<uint64_t>(
                (static_cast<double>(frames) * 1000000.0) / static_cast<double>(current_sample_rate));
            instrumentation_expected_mix_gap_us_.store(expected_us, std::memory_order_relaxed);
        }
        if (elapsed_us > resonance::kLateMixThresholdUs)
            instrumentation_late_mix_count.fetch_add(1, std::memory_order_relaxed);
    }
    last_mix_time_ = now;
    _sync_params();

    ResonanceServer* srv_guard = ResonanceServer::get_singleton();
    if (is_initialized && srv_guard && srv_guard->is_initialized() && context != srv_guard->get_context_handle())
        _cleanup_steam_audio();

    // Keep a strong ref for the mix call so teardown on another thread cannot drop base_playback mid-call.
    const Ref<AudioStreamPlayback> base_guard = base_playback;
    if (base_guard.is_null())
        return 0;
    PackedVector2Array mixed_frames = base_guard->mix_audio(rate_scale, frames);
    int32_t samples_read = static_cast<int32_t>(mixed_frames.size());
    // Some stream backends can report "not playing" (EOS) but still return a full silent block.
    // Our tail-drain logic hinges on samples_read == 0, so normalize this case to EOS to avoid
    // a playback that never transitions into the tail branch (and thus never reaches finished()).
    if (samples_read == frames && !base_guard->is_playing()) {
        samples_read = 0;
    }
    // Godot normally returns exactly `frames` samples. If a backend returns more, writing past
    // `buffer` would corrupt memory (defensive guard).
    if (samples_read > frames) {
        static std::atomic<int> oversize_warn_count{0};
        const int n = oversize_warn_count.fetch_add(1, std::memory_order_relaxed);
        if (n < 4) {
            ResonanceLog::warn(
                "ResonanceStreamPlayback: mix_audio returned more samples than requested; clamping to avoid buffer overflow.");
        }
        samples_read = frames;
    }
    // (debug logging removed)
    if (samples_read == 0)
        instrumentation_zero_input_count.fetch_add(1, std::memory_order_relaxed);
    else {
        int32_t cur_min = instrumentation_mix_frames_min.load(std::memory_order_relaxed);
        if (samples_read < cur_min)
            instrumentation_mix_frames_min.store(samples_read, std::memory_order_relaxed);
        int32_t cur_max = instrumentation_mix_frames_max.load(std::memory_order_relaxed);
        if (samples_read > cur_max)
            instrumentation_mix_frames_max.store(samples_read, std::memory_order_relaxed);
    }

    // When no input: drain direct effect tail and any remaining output for clean fade-out
    if (samples_read == 0) {
        if (!is_initialized)
            return 0;
        if (!srv_guard || !srv_guard->is_initialized() || context != srv_guard->get_context_handle()) {
            _cleanup_steam_audio();
            return 0;
        }
        // Arm the tail grace cap on the first transition into this branch so a stuck IPL
        // effect handle (or any other pathological state) cannot keep the playback alive
        // forever. Budget = max_reverb_duration in audio blocks, plus a small safety margin.
        if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) < 0) {
            const int sr = current_sample_rate > 0 ? current_sample_rate : 48000;
            const int fs = frame_size_ > 0 ? frame_size_ : resonance::kGodotDefaultFrameSize;
            const float max_reverb_duration = srv_guard->get_max_reverb_duration();
            const int64_t blocks = (int64_t)((max_reverb_duration * (float)sr) / (float)fs) + 8;
            tail_grace_blocks_remaining_.store(blocks > 0 ? blocks : 8, std::memory_order_release);
        }
        // Dry ended with a partial block still in the input ring: the samples_read > 0 path only
        // calls _process_steam_audio_block when available_read >= frame_size_. Skipping the final
        // partial frame starves one convolution Apply (and can leave the reflection mixer empty for
        // one bus tick) before GetTail — audible as a short dropout.
        while (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
               output_ring_l.get_available_write() >= (size_t)frame_size_) {
            _process_steam_audio_block();
            instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
        }
        {
            const size_t rem = input_ring_l.get_available_read();
            if (rem > 0 && rem < (size_t)frame_size_) {
                // Pad the final partial block to a full process frame. Zero-fill caused a sharp
                // drop; DC-hold sounded stepped. Linear fade of the pad tail toward zero smooths the
                // last streaming frame before GetTail.
                if (input_ring_r.get_available_read() == rem &&
                    rem <= temp_process_buffer_l.size() && rem <= temp_process_buffer_r.size() &&
                    (size_t)frame_size_ <= temp_process_buffer_l.size() &&
                    (size_t)frame_size_ <= temp_process_buffer_r.size()) {
                    input_ring_l.read(temp_process_buffer_l.data(), rem);
                    input_ring_r.read(temp_process_buffer_r.data(), rem);
                    const float hold_l = temp_process_buffer_l[rem - 1];
                    const float hold_r = temp_process_buffer_r[rem - 1];
                    const size_t pad_count = (size_t)frame_size_ - rem;
                    // Linear fade of pad region from last sample toward zero. DC-hold fed a non-zero
                    // constant into the convolution tail of the same frame as real audio, which can
                    // sound stepped/choppy; tapering to zero smooths the streaming→tail handoff.
                    for (size_t k = 0; k < pad_count; k++) {
                        const float fade = 1.0f - float(k + 1) / float(pad_count);
                        temp_process_buffer_l[rem + k] = hold_l * fade;
                        temp_process_buffer_r[rem + k] = hold_r * fade;
                    }
                    if (input_ring_l.get_available_write() >= (size_t)frame_size_ &&
                        input_ring_r.get_available_write() >= (size_t)frame_size_) {
                        input_ring_l.write(temp_process_buffer_l.data(), (size_t)frame_size_);
                        input_ring_r.write(temp_process_buffer_r.data(), (size_t)frame_size_);
                    } else {
                        input_ring_l.write(temp_process_buffer_l.data(), rem);
                        input_ring_r.write(temp_process_buffer_r.data(), rem);
                        for (size_t k = 0; k < pad_count; k++) {
                            if (input_ring_l.get_available_write() == 0 || input_ring_r.get_available_write() == 0)
                                break;
                            const float fade = 1.0f - float(k + 1) / float(pad_count);
                            float pl = hold_l * fade;
                            float pr = hold_r * fade;
                            input_ring_l.write(&pl, 1);
                            input_ring_r.write(&pr, 1);
                        }
                    }
                }
                if (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
                    output_ring_l.get_available_write() >= (size_t)frame_size_) {
                    _process_steam_audio_block();
                    instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        bool produced_any = false;
        while (output_ring_l.get_available_read() < (size_t)frames) {
            if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) <= 0)
                break;
            if (!ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_) ||
                !ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_))
                break;

            bool produced = false;
            _zero_sa_final_mix();

            if (direct_processor.process_tail(sa_direct_out_buffer)) {
                for (int c = 0; c < direct_out_channels_; c++) {
                    if (sa_direct_out_buffer.data[c] && sa_final_mix_buffer.data[c])
                        memcpy(sa_final_mix_buffer.data[c], sa_direct_out_buffer.data[c], frame_size_ * sizeof(float));
                }
                produced = true;
            }

            if (current_source_handle >= 0 && reflection_tail_have_params_ && reflection_processor.get_tail_size_samples() > 0) {
                const int eos_refl_type = srv_guard->get_reflection_type();
                if (eos_refl_type == resonance::kReflectionConvolution || eos_refl_type == resonance::kReflectionTan) {
                    auto eos_mixer_guard = srv_guard->scoped_mixer_read();
                    IPLReflectionMixer eos_mixer = eos_mixer_guard.get();
                    if (eos_mixer) {
                        IPLReflectionEffectParams rp = reflection_tail_params_;
                        // Unity reverb: the spatialize effect continues to call iplReflectionEffectApply each DSP tick
                        // (with silent input once the source ends). There is no explicit GetTail path in Unity's mixer
                        // integration. To match that behavior and avoid EOS clicks from a Tail->Mixer step, keep the
                        // effect advancing via Apply(silence) until its internal tail is exhausted (bounded by grace).
                        if (!conv_reverb_eos_silence_apply_done_)
                            conv_reverb_eos_silence_apply_done_ = true;
                        if (sa_in_buffer.data[0])
                            memset(sa_in_buffer.data[0], 0, static_cast<size_t>(frame_size_) * sizeof(float));
                        if (sa_in_buffer.data[1])
                            memset(sa_in_buffer.data[1], 0, static_cast<size_t>(frame_size_) * sizeof(float));

                        const float node_vol_eos = owner_effective_volume_linear(owner_player_);
                        const float wet_rmd_eos = resonance::reverb_wet_falloff_max_distance(
                            params_current.distance, srv_guard->get_reverb_max_distance());
                        const float wet_occ_eos = resonance::sanitize_audio_float(params_current.wet_occlusion_factor);
                        const float refl_dist_att_eos = resonance::sanitize_audio_float(params_current.refl_distance_attenuation);
                        const float curr_refl_mix_eos = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                        const float wet_extra_eos = resonance::sanitize_audio_float(
                            node_vol_eos * refl_dist_att_eos * wet_rmd_eos * wet_occ_eos);

                        if (reflection_processor.process_mix(sa_in_buffer, rp, eos_mixer, prev_conv_reflections_mix_level_,
                                                             curr_refl_mix_eos, wet_extra_eos)) {
                            prev_conv_reflections_mix_level_ = curr_refl_mix_eos;
                            srv_guard->record_mixer_feed();
                            produced = true;
                        }
                    }
                } else if (eos_refl_type == resonance::kReflectionParametric || eos_refl_type == resonance::kReflectionHybrid) {
                    IPLReflectionEffectParams rp = reflection_tail_params_;
                    reflection_processor.tail_apply_direct(&rp);
                    IPLAudioBuffer* reverb_buf = reflection_processor.get_direct_output_buffer();
                    if (reverb_buf && reverb_buf->data) {
                        _add_reverb_to_output(reverb_buf, reflection_tail_wet_gain_, reflection_tail_split_output_,
                                              srv_guard->get_current_listener_coords());
                        produced = true;
                    }
                }
            }
            if (reflection_processor.get_tail_size_samples() <= 0)
                reflection_tail_have_params_ = false;

            if (current_source_handle >= 0 && srv_guard && srv_guard->is_pathing_enabled() && path_processor.get_tail_size_samples() > 0 &&
                sa_path_out_buffer.data && sa_path_out_buffer.data[0] && sa_path_out_buffer.data[1]) {
                // Unity pathing: continues calling iplPathEffectApply every DSP tick (silent input after EOS).
                // Avoid GetTail() step differences by advancing the effect via Apply(silence) with cached params.
                if (pathing_tail_have_params_ && pathing_tail_params_.shCoeffs) {
                    if (sa_in_buffer.data[0])
                        memset(sa_in_buffer.data[0], 0, static_cast<size_t>(frame_size_) * sizeof(float));
                    if (sa_in_buffer.data[1])
                        memset(sa_in_buffer.data[1], 0, static_cast<size_t>(frame_size_) * sizeof(float));

                    if (sa_path_out_buffer.data[0])
                        memset(sa_path_out_buffer.data[0], 0, frame_size_ * sizeof(float));
                    if (sa_path_out_buffer.data[1])
                        memset(sa_path_out_buffer.data[1], 0, frame_size_ * sizeof(float));

                    // Keep last mix level (no new ramp). Silent input means only tail decay remains.
                    path_processor.process(sa_in_buffer, pathing_tail_params_, sa_path_out_buffer,
                                           prev_pathing_mix_level_, prev_pathing_mix_level_);
                    for (int i = 0; i < frame_size_; i++) {
                        if (sa_final_mix_buffer.data[0])
                            sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
                        if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                            sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
                    }
                    produced = true;
                }
            }

            if (!produced)
                break;
            produced_any = true;

            for (int c = 0; c < direct_out_channels_; c++) {
                if (!sa_final_mix_buffer.data[c])
                    continue;
                for (int i = 0; i < frame_size_; i++)
                    sa_final_mix_buffer.data[c][i] = std::clamp(sa_final_mix_buffer.data[c][i], -1.0f, 1.0f);
            }
            _write_output_rings_folded();
            // Decrement grace budget for every produced tail block.
            int64_t remaining = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
            if (remaining > 0)
                tail_grace_blocks_remaining_.store(remaining - 1, std::memory_order_release);
        }
        int available = (int)output_ring_l.get_available_read();
        int to_copy = (frames < available) ? frames : available;
        const float v_tail = owner_effective_volume_linear(owner_player_);
        for (int i = 0; i < to_copy; i++) {
            float l, r;
            output_ring_l.read(&l, 1);
            output_ring_r.read(&r, 1);
            buffer[i].left = l * v_tail;
            buffer[i].right = r * v_tail;
        }
        for (int i = to_copy; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        // When fully drained, return 0 to signal EOS to AudioServer. Some Godot builds/backends
        // do not reliably poll _is_playing() for custom playbacks, so "always return frames"
        // can prevent `finished` from ever emitting.
        //
        // IMPORTANT: Steam Audio effects can report a non-zero tail size even when we can no
        // longer advance/produce tail audio (e.g. missing mixer/handles). In that case we must
        // still terminate, otherwise playback gets stuck forever with silent output and no
        // `finished`.
        const bool drained = (to_copy == 0) &&
                             (!produced_any ||
                              (!has_active_tail_residue() &&
                               (tail_grace_blocks_remaining_.load(std::memory_order_acquire) <= 0)));
        return drained ? 0 : frames;
    }

    if (!is_initialized) {
        _lazy_init_steam_audio(0);
        // If init failed (e.g. out of memory or no context), fallback to passthrough
        if (!is_initialized) {
            const float v = owner_effective_volume_linear(owner_player_);
            for (int i = 0; i < samples_read; i++) {
                buffer[i].left = mixed_frames[i].x * v;
                buffer[i].right = mixed_frames[i].y * v;
            }
            for (int i = samples_read; i < frames; i++) {
                buffer[i].left = 0.0f;
                buffer[i].right = 0.0f;
            }
            return frames;
        }
    }

    const Vector2* src_ptr = mixed_frames.ptr();
    for (int i = 0; i < samples_read; i++) {
        float l = src_ptr[i].x;
        float r = src_ptr[i].y;

        if (input_ring_l.get_available_write() > 0) {
            input_ring_l.write(&l, 1);
            input_ring_r.write(&r, 1);
        } else {
            instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // If the decoder produced fewer samples than requested (common for some stream backends and for
    // one-shots near start/stop), treat the remainder as silence so we can still process one full
    // Steam Audio block per callback (Unity parity: DSP always runs at fixed buffer size).
    for (int i = samples_read; i < frames; i++) {
        float l = 0.0f;
        float r = 0.0f;
        if (input_ring_l.get_available_write() > 0) {
            input_ring_l.write(&l, 1);
            input_ring_r.write(&r, 1);
        } else {
            instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }

    int blocks_processed_this_call = 0;
    while (blocks_processed_this_call < kMaxBlocksPerMixCall && input_ring_l.get_available_read() >= frame_size_) {
        if (output_ring_l.get_available_write() >= frame_size_) {
            _process_steam_audio_block();
            instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
            blocks_processed_this_call++;
        } else {
            instrumentation_output_blocked.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    // Always report `frames` produced, even if mix_audio() returned fewer (end-of-stream tail).
    // Returning samples_read < frames here would cause AudioServer to detach the playback before
    // we get a chance to drain the reverb / pathing tail in the samples_read==0 branch on the
    // next call. Zero-padding the unused tail of `buffer` is safe and the tail-drain branch will
    // produce real wet audio once the dry signal has fully ended.
    int samples_to_output = frames;
    int available = (int)output_ring_l.get_available_read();
    int valid_copy = (samples_to_output < available) ? samples_to_output : available;
    if (valid_copy < samples_to_output) {
        instrumentation_output_underrun.fetch_add((uint64_t)(samples_to_output - valid_copy), std::memory_order_relaxed);
    }

    const float v_out = owner_effective_volume_linear(owner_player_);
    for (int i = 0; i < valid_copy; i++) {
        float l = 0.0f;
        float r = 0.0f;
        output_ring_l.read(&l, 1);
        output_ring_r.read(&r, 1);
        buffer[i].left = l * v_out;
        buffer[i].right = r * v_out;
    }

    for (int i = valid_copy; i < samples_to_output; i++) {
        buffer[i].left = 0.0f;
        buffer[i].right = 0.0f;
    }

    return samples_to_output;
}

void ResonanceStreamPlayback::_start(double from_pos) {
    input_started = false;
    // Re-arm the first-params gate for the new playback run so restarted players do not leak
    // a full-volume burst with stale or default parameters before the main thread pushes fresh ones.
    params_ever_synced_.store(false, std::memory_order_release);
    // Cancel any in-flight tail-drain from a previous run so the fresh playback starts cleanly.
    stop_requested_.store(false, std::memory_order_release);
    tail_grace_blocks_remaining_.store(-1, std::memory_order_release);
    prev_conv_reflections_mix_level_ = -1.0f;
    prev_parametric_reflections_mix_level_ = 1.0f;
    prev_pathing_mix_level_ = 1.0f;
    reflection_processor.reset_effect();
    path_processor.reset_effect();
    reflection_tail_have_params_ = false;
    conv_reverb_eos_silence_apply_done_ = false;
    memset(&reflection_tail_params_, 0, sizeof(reflection_tail_params_));
    pathing_tail_have_params_ = false;
    memset(&pathing_tail_params_, 0, sizeof(pathing_tail_params_));
    reverb_ring_prev_l_ = 0.0f;
    reverb_ring_prev_r_ = 0.0f;
    reverb_ring_prev_valid_ = false;
    direct_processor.reset_for_new_playback();
    input_ring_l.clear();
    input_ring_r.clear();
    output_ring_l.clear();
    output_ring_r.clear();
    output_ring_reverb_l.clear();
    output_ring_reverb_r.clear();
    if (base_playback.is_valid()) {
        base_playback->start(from_pos);
        // (debug logging removed)
    }
    if (owner_player_)
        owner_player_->internal_register_playback(this);
}
void ResonanceStreamPlayback::_stop() {
    // Soft stop: halt the dry input so mix_audio() returns 0 from now on, but keep the
    // playback alive while the reverb / pathing tail decays. _is_playing() stays true via
    // has_active_tail_residue() until the effect tails are exhausted or the grace block
    // budget runs out. The destructor performs the final unregister + IPL cleanup once Godot
    // detaches the playback (after _is_playing returns false). We intentionally do NOT call
    // direct_processor.reset_for_new_playback() here - that would clear the in-flight tail.
    if (base_playback.is_valid())
        base_playback->stop();
    stop_requested_.store(true, std::memory_order_release);
}

void ResonanceStreamPlayback::request_soft_stop() {
    if (base_playback.is_valid())
        base_playback->stop();
    stop_requested_.store(true, std::memory_order_release);
}

bool ResonanceStreamPlayback::has_active_tail_residue() const {
    if (output_ring_l.get_available_read() > 0)
        return true;
    if (output_ring_reverb_l.get_available_read() > 0)
        return true;
    if (reflection_processor.get_tail_size_samples() > 0)
        return true;
    if (path_processor.get_tail_size_samples() > 0)
        return true;
    return false;
}

bool ResonanceStreamPlayback::_is_playing() const {
    if (base_playback.is_valid() && base_playback->is_playing())
        return true;
    // Dry signal ended (natural completion or explicit stop). Stay alive while output rings
    // or IPL tails hold residue, or while tail_grace_blocks_remaining_ still budgets tail
    // generation in the samples_read==0 path (grace counts production iterations, not ring depth).
    if (!is_initialized)
        return false;
    if (has_active_tail_residue())
        return true;
    const int64_t grace = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
    if (grace == 0)
        return false;
    // Grace is armed (>= 0) on the first samples_read==0 mix. Until then it stays -1; Godot can
    // query _is_playing() after base EOS but before that callback runs — keep the playback alive
    // so the tail branch executes once.
    if (grace < 0)
        return true;
    // grace > 0 but no measurable residue: original behaviour — stop rather than spin idle.
    return false;
}
int ResonanceStreamPlayback::_get_loop_count() const {
    return base_playback.is_valid() ? base_playback->get_loop_count() : 0;
}
void ResonanceStreamPlayback::_seek(double position) {
    if (base_playback.is_valid())
        base_playback->seek(position);
}

void ResonanceStream::set_base_stream(const Ref<AudioStream>& p_stream) { base_stream = p_stream; }
Ref<AudioStreamPlayback> ResonanceStream::_instantiate_playback() const {
    // Animation TYPE_AUDIO uses AudioStreamPolyphonic. Returning ResonanceStreamPlayback here crashes on Windows
    // before playback starts. Native playback is stable.
    // Steam for TYPE_AUDIO: enable [member ResonancePlayer.convert_animation_audio_tracks_at_runtime] or convert in editor.
    if (base_stream.is_valid() && base_stream->is_class("AudioStreamPolyphonic")) {
        return base_stream->instantiate_playback();
    }
    Ref<ResonanceStreamPlayback> playback;
    playback.instantiate();
    playback->set_owner_player(stream_owner_);
    if (base_stream.is_valid()) {
        playback->set_base_playback(base_stream->instantiate_playback());
    }
    // Phase 4: pre-allocate Steam Audio effects and buffers here on the calling thread (typically main during play()).
    // Each polyphonic voice instantiates a new playback, so this covers all max_polyphony voices. _mix will skip the
    // lazy path and no longer allocates IPL buffers on the audio thread.
    playback->prewarm_steam_audio();
    return playback;
}

void ResonanceReverbPlayback::set_parent_player(ResonancePlayer* p_player) {
    parent_player = p_player;
}

int32_t ResonanceReverbPlayback::_mix(AudioFrame* buffer, float _rate_scale, int32_t frames) {
    if (!parent_player || frames <= 0 || !buffer)
        return 0;
    std::vector<ResonanceStreamPlayback*> voices;
    parent_player->internal_copy_internal_playbacks(voices);
    size_t total_reverb_samples = 0;
    for (ResonanceStreamPlayback* pb : voices) {
        if (pb)
            total_reverb_samples += pb->get_reverb_ring_available_read();
    }
    ResonanceStreamPlayback* fallback_pb = nullptr;
    if (voices.empty())
        fallback_pb = Object::cast_to<ResonanceStreamPlayback>(parent_player->get_stream_playback().ptr());
    if (voices.empty() && !fallback_pb)
        return 0;
    if (voices.empty()) {
        if (!parent_player->is_playing() && fallback_pb->get_reverb_ring_available_read() == 0)
            return 0;
        return fallback_pb->read_reverb_frames(buffer, frames);
    }
    if (!parent_player->is_playing() && total_reverb_samples == 0)
        return 0;
    for (int32_t i = 0; i < frames; i++) {
        buffer[i].left = 0.0f;
        buffer[i].right = 0.0f;
    }
    thread_local static std::vector<AudioFrame> tmp_voice_reverb;
    tmp_voice_reverb.resize((size_t)frames);
    for (ResonanceStreamPlayback* pb : voices) {
        if (!pb)
            continue;
        pb->read_reverb_frames(tmp_voice_reverb.data(), frames);
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left += tmp_voice_reverb[i].left;
            buffer[i].right += tmp_voice_reverb[i].right;
        }
    }
    for (int32_t i = 0; i < frames; i++) {
        buffer[i].left = std::clamp(buffer[i].left, -1.0f, 1.0f);
        buffer[i].right = std::clamp(buffer[i].right, -1.0f, 1.0f);
    }
    return frames;
}

void ResonanceReverbPlayback::_start(double _from_pos) {}
void ResonanceReverbPlayback::_stop() {}
bool ResonanceReverbPlayback::_is_playing() const {
    if (!parent_player)
        return false;
    if (parent_player->is_playing())
        return true;
    // Parent stopped, but the split-reverb ring may still hold tail samples that have not
    // been drained yet; keep this child alive until the rings are empty so the wet tail
    // reaches the reverb bus instead of being cut off.
    return parent_player->any_internal_playback_has_reverb_ring_data();
}
int ResonanceReverbPlayback::_get_loop_count() const { return 0; }
void ResonanceReverbPlayback::_seek(double _position) {}

void ResonanceReverbPlayback::_bind_methods() {}

void ResonanceReverbStream::set_parent_player(ResonancePlayer* p_player) {
    parent_player = p_player;
}

Ref<AudioStreamPlayback> ResonanceReverbStream::_instantiate_playback() const {
    Ref<ResonanceReverbPlayback> pb;
    pb.instantiate();
    pb->set_parent_player(parent_player);
    return pb;
}

void ResonanceReverbStream::_bind_methods() {}

float ResonancePlayer::_config_float(const char* key, float default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (float)v : default_val;
}
int ResonancePlayer::_config_int(const char* key, int default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (int)v : default_val;
}
bool ResonancePlayer::_config_bool(const char* key, bool default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (bool)v : default_val;
}
ResonanceStreamPlayback* ResonancePlayer::_get_resonance_playback() {
    if (!is_playing())
        return nullptr;
    Ref<AudioStreamPlayback> pb = get_stream_playback();
    return pb.is_valid() ? Object::cast_to<ResonanceStreamPlayback>(pb.ptr()) : nullptr;
}

Ref<Curve> ResonancePlayer::_config_curve(const char* key, const Ref<Curve>& default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    if (v.get_type() == Variant::OBJECT) {
        Ref<Curve> r = v;
        if (r.is_valid())
            return r;
    }
    return default_val;
}

NodePath ResonancePlayer::_config_node_path(const char* key) const {
    if (!player_config.is_valid())
        return NodePath();
    Variant v = player_config->get(StringName(key));
    if (v.get_type() == Variant::NODE_PATH)
        return NodePath(v);
    return NodePath();
}

void ResonancePlayer::_refresh_config_cache() {
    if (!player_config.is_valid())
        return;
    config_cache_.min_distance = _config_float("min_distance", 1.0f);
    config_cache_.max_distance = _config_float("max_distance", 500.0f);
    config_cache_.source_radius = _config_float("source_radius", 1.0f);
    const bool legacy_dist_sim = _config_bool("distance_attenuation_simulation_enabled", true);
    int am = _config_int("attenuation_mode", 0);
    if (am == 0 && !legacy_dist_sim)
        am = ATTENUATION_INVERSE_NO_SIM;
    if (am < ATTENUATION_INVERSE || am > ATTENUATION_INVERSE_NO_SIM)
        am = ATTENUATION_INVERSE;
    config_cache_.attenuation_mode = am;
    config_cache_.linear_curve_use_sim_distance_attenuation = legacy_dist_sim;
    config_cache_.attenuation_curve = _config_curve("attenuation_curve", Ref<Curve>());
    config_cache_.air_absorption_enabled = _config_bool("air_absorption_enabled", true);
    config_cache_.air_absorption_input = _config_int("air_absorption_input", 0);
    config_cache_.air_absorption_low = _config_float("air_absorption_low", 1.0f);
    config_cache_.air_absorption_mid = _config_float("air_absorption_mid", 1.0f);
    config_cache_.air_absorption_high = _config_float("air_absorption_high", 1.0f);
    config_cache_.direct_binaural_override = _config_int("direct_binaural_override", -1);
    config_cache_.directivity_enabled = _config_bool("directivity_enabled", false);
    config_cache_.directivity_weight = _config_float("directivity_weight", 0.0f);
    config_cache_.directivity_power = _config_float("directivity_power", 1.0f);
    config_cache_.spatial_blend = _config_float("spatial_blend", 1.0f);
    config_cache_.use_ambisonics_encode = _config_bool("use_ambisonics_encode", false);
    auto read_tri_state = [this](const char* override_key, const char* legacy_bool_key) -> int {
        if (!player_config.is_valid())
            return -1;
        Variant v_ov = player_config->get(StringName(override_key));
        if (v_ov.get_type() != Variant::NIL) {
            int o = (int)v_ov;
            if (o < -1 || o > 1)
                o = -1;
            return o;
        }
        Variant v_leg = player_config->get(StringName(legacy_bool_key));
        if (v_leg.get_type() != Variant::NIL)
            return (bool)v_leg ? 1 : 0;
        return -1;
    };
    config_cache_.path_validation_override = read_tri_state("path_validation_override", "path_validation_enabled");
    config_cache_.find_alternate_paths_override = read_tri_state("find_alternate_paths_override", "find_alternate_paths");
    config_cache_.reflections_type = _config_int("reflections_type", -1);
    config_cache_.reflections_enabled = _config_int("reflections_enabled", -1);
    config_cache_.pathing_enabled_override = _config_int("pathing_enabled_override", -1);
    {
        auto read_tri_state_hrtf = [this](const char* new_key, const char* legacy_key) -> int {
            if (!player_config.is_valid())
                return -1;
            Variant v_new = player_config->get(StringName(new_key));
            if (v_new.get_type() != Variant::NIL) {
                int o = static_cast<int>(v_new);
                if (o < -1 || o > 1)
                    o = -1;
                return o;
            }
            return _config_int(legacy_key, -1);
        };
        config_cache_.reverb_binaural_override = read_tri_state_hrtf("reverb_binaural_override", "apply_hrtf_to_reflections");
        config_cache_.pathing_binaural_override = read_tri_state_hrtf("pathing_binaural_override", "apply_hrtf_to_pathing");
    }
    config_cache_.occlusion_input = _config_int("occlusion_input", 0);
    config_cache_.transmission_input = _config_int("transmission_input", 0);
    config_cache_.directivity_input = _config_int("directivity_input", 0);
    config_cache_.occlusion_value = _config_float("occlusion_value", 1.0f);
    config_cache_.transmission_low = _config_float("transmission_low", 1.0f);
    config_cache_.transmission_mid = _config_float("transmission_mid", 1.0f);
    config_cache_.transmission_high = _config_float("transmission_high", 1.0f);
    config_cache_.directivity_value = _config_float("directivity_value", 1.0f);
    config_cache_.occlusion_samples = _config_int("occlusion_samples", resonance::kDefaultOcclusionSamples);
    {
        ResonanceServer* srv = ResonanceServer::get_singleton();
        int tx_surfaces_def = srv ? srv->get_max_transmission_surfaces() : resonance::kDefaultPlayerConfigTransmissionRays;
        int mts_ov = _config_int("max_transmission_surfaces_override", 0);
        if (mts_ov == -1)
            mts_ov = 0; // legacy GDScript enum "Use Global:-1"
        if (mts_ov != 0 && mts_ov != 1)
            mts_ov = 0;
        config_cache_.max_transmission_surfaces_override = mts_ov;
        if (mts_ov == 0) {
            config_cache_.max_transmission_surfaces = tx_surfaces_def;
        } else {
            int mts = _config_int("max_transmission_surfaces", tx_surfaces_def);
            if (mts < 1)
                mts = 1;
            if (mts > resonance::kMaxTransmissionRays)
                mts = resonance::kMaxTransmissionRays;
            config_cache_.max_transmission_surfaces = mts;
        }
    }
    config_cache_.direct_mix_level = _config_float("direct_mix_level", 1.0f);
    config_cache_.reflections_mix_level = _config_float("reflections_mix_level", 1.0f);
    config_cache_.pathing_mix_level = _config_float("pathing_mix_level", 1.0f);
    config_cache_.reflections_eq_low = _config_float("reflections_eq_low", 1.0f);
    config_cache_.reflections_eq_mid = _config_float("reflections_eq_mid", 1.0f);
    config_cache_.reflections_eq_high = _config_float("reflections_eq_high", 1.0f);
    config_cache_.reflections_delay = _config_int("reflections_delay", -1);
    config_cache_.perspective_override = _config_int("perspective_correction_override", -1);
    config_cache_.perspective_factor = _config_float("perspective_factor", 1.0f);
    config_cache_.playback_parameter_min_interval = _config_float("playback_parameter_min_interval", 0.0f);
    config_cache_.playback_parameter_min_move = _config_float("playback_parameter_min_move", 0.0f);
    config_cache_.playback_coeff_smoothing_time = _config_float("playback_coeff_smoothing_time", 0.0f);
    config_cache_.simulation_occlusion_enabled = _config_bool("simulation_occlusion_enabled", true);
    config_cache_.simulation_transmission_enabled = _config_bool("simulation_transmission_enabled", true);
    {
        int occ_ov = _config_int("occlusion_type_override", 2);
        // 2 = Use Global (GDScript enum; avoids negative values in .tres). -1 = legacy Use Global.
        if (occ_ov == 2 || occ_ov == -1)
            config_cache_.occlusion_type_override = -1;
        else if (occ_ov == 0 || occ_ov == 1)
            config_cache_.occlusion_type_override = occ_ov;
        else
            config_cache_.occlusion_type_override = -1;
    }
    config_cache_.transmission_type_override = _config_int("transmission_type_override", -1);
    if (config_cache_.transmission_type_override < -1 || config_cache_.transmission_type_override > 1)
        config_cache_.transmission_type_override = -1;
    config_cache_.hrtf_interpolation_override = _config_int("hrtf_interpolation_override", -1);
    if (config_cache_.hrtf_interpolation_override < -1 || config_cache_.hrtf_interpolation_override > 1)
        config_cache_.hrtf_interpolation_override = -1;
    {
        int occ_wet_ov = _config_int("apply_occlusion_to_baked_reflections_override", -1);
        if (occ_wet_ov < -1 || occ_wet_ov > 1)
            occ_wet_ov = -1;
        config_cache_.apply_occlusion_to_baked_reflections_override = occ_wet_ov;
    }
    {
        int dist_wet_ov = _config_int("apply_distance_curve_to_reflections_override", -1);
        if (dist_wet_ov < -1 || dist_wet_ov > 1)
            dist_wet_ov = -1;
        config_cache_.apply_distance_curve_to_reflections_override = dist_wet_ov;
    }
    {
        int tx_input = _config_int("reverb_transmission_amount_input", 0);
        if (tx_input != 0 && tx_input != 1)
            tx_input = 0;
        config_cache_.reverb_transmission_amount_input = tx_input;
        float tx_amt = _config_float("reverb_transmission_amount", 1.0f);
        if (tx_amt < 0.0f)
            tx_amt = 0.0f;
        if (tx_amt > 1.0f)
            tx_amt = 1.0f;
        config_cache_.reverb_transmission_amount = tx_amt;
    }
    config_cache_valid_ = true;
}

bool ResonancePlayer::_steam_sim_distance_attenuation_enabled(const ConfigCache& c) {
    if (c.attenuation_mode == ATTENUATION_INVERSE_NO_SIM)
        return false;
    if (c.attenuation_mode == ATTENUATION_INVERSE)
        return true;
    return c.linear_curve_use_sim_distance_attenuation;
}

void ResonancePlayer::_ready() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid()) {
        // No config: behave as plain AudioStreamPlayer3D
        if (is_autoplay_enabled())
            play();
        return;
    }
    add_to_group("resonance_player");
    debug_drawer.initialize(this);
    directivity_drawer_.initialize(this);
    directivity_drawer_.mark_dirty();
    set_attenuation_model(ATTENUATION_DISABLED);
    _ensure_source_exists();
    _update_stream_setup();
    if (is_autoplay_enabled())
        play_stream();
}

void ResonancePlayer::_exit_tree() {
    _clear_physics_ray_auto_exclude_rids();
    // Hard cutoff on tree exit: tail decay is meaningless once the node leaves the tree
    // (and would race with imminent destruction). Bypass the soft-stop path in stop() and
    // fall through to plain AudioStreamPlayer3D::stop() so AudioServer detaches the playbacks
    // synchronously before the parent is freed.
    warned_source_handle_create_failed_ = false;
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;
    Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
        if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
            rp->stop();
    }
    AudioStreamPlayer3D::stop();
    debug_drawer.cleanup();
    directivity_drawer_.cleanup();

    if (source_handle >= 0) {
        ResonanceServer* srv = ResonanceServer::get_singleton();
        if (srv && !ResonanceServer::is_shutting_down())
            srv->destroy_source_handle(source_handle);
        source_handle = -1;
    }
}

ResonancePlayer::~ResonancePlayer() {
    std::vector<ResonanceStreamPlayback*> copy;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        copy.swap(internal_playbacks_);
    }
    for (ResonanceStreamPlayback* p : copy) {
        if (p)
            p->internal_orphan_owner_player();
    }
}

void ResonancePlayer::internal_register_playback(ResonanceStreamPlayback* p) {
    if (!p)
        return;
    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    for (ResonanceStreamPlayback* q : internal_playbacks_) {
        if (q == p)
            return;
    }
    internal_playbacks_.push_back(p);
}

void ResonancePlayer::internal_unregister_playback(ResonanceStreamPlayback* p) {
    if (!p)
        return;
    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    internal_playbacks_.erase(std::remove(internal_playbacks_.begin(), internal_playbacks_.end(), p), internal_playbacks_.end());
}

void ResonancePlayer::internal_copy_internal_playbacks(std::vector<ResonanceStreamPlayback*>& out) const {
    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    out = internal_playbacks_;
}

bool ResonancePlayer::any_internal_playback_has_reverb_ring_data() const {
    std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
    for (ResonanceStreamPlayback* pb : internal_playbacks_) {
        if (pb && pb->get_reverb_ring_available_read() > 0)
            return true;
    }
    return false;
}

void ResonancePlayer::_broadcast_update_parameters(const PlaybackParameters& p) {
    std::vector<ResonanceStreamPlayback*> copy;
    {
        std::lock_guard<std::mutex> lock(internal_playbacks_mutex_);
        copy = internal_playbacks_;
    }
    for (ResonanceStreamPlayback* pb : copy) {
        if (pb)
            pb->update_parameters(p);
    }
}

void ResonancePlayer::_aggregate_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) {
    out_direct = 0.0f;
    out_reverb = 0.0f;
    out_pathing = 0.0f;
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    for (ResonanceStreamPlayback* pb : copy) {
        if (!pb)
            continue;
        float d = 0.0f, r = 0.0f, p = 0.0f;
        pb->get_debug_signal_levels(d, r, p);
        out_direct = std::max(out_direct, d);
        out_reverb = std::max(out_reverb, r);
        out_pathing = std::max(out_pathing, p);
    }
    if (copy.empty()) {
        if (ResonanceStreamPlayback* pb = _get_resonance_playback())
            pb->get_debug_signal_levels(out_direct, out_reverb, out_pathing);
    }
}

void ResonancePlayer::_ensure_config_valid() {
    if (config_cache_frame_countdown <= 0 || !config_cache_valid_) {
        _refresh_config_cache();
        config_cache_frame_countdown = kConfigCacheRefreshInterval;
    }
}

void ResonancePlayer::_ensure_config_and_apply_source(int32_t pathing_batch) {
    _ensure_config_valid();
    _apply_update_source(pathing_batch, false);
}

int ResonancePlayer::_compute_baked_data_variation(const ResonanceServer* srv) const {
    const ConfigCache& c = config_cache_;
    // Player config reflections_type:
    // -1 = Use Global (runtime default_reflections_mode)
    //  0 = Realtime
    //  1 = Baked Reverb
    //  2 = Baked Static Source
    //  3 = Baked Static Listener
    //
    // Server baked_data_variation:
    // -1 = Realtime reflections (ray traced)
    //  0 = Baked Reverb (probe data)
    //  1 = Baked Static Source
    //  2 = Baked Static Listener
    if (c.reflections_type == -1) {
        return (srv && srv->get_default_reflections_mode() == resonance::kDefaultReflectionsRealtime) ? -1 : 0;
    }
    if (c.reflections_type == 0)
        return -1;
    if (c.reflections_type == 2)
        return 1;
    if (c.reflections_type == 3)
        return 2;
    // c.reflections_type == 1 (Baked Reverb) or unknown -> baked reverb.
    return 0;
}

void ResonancePlayer::_apply_update_source(int32_t pathing_batch, bool defer_if_sim_mutex_busy) {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || source_handle < 0)
        return;

    const ConfigCache& c = config_cache_;
    Transform3D gt = get_global_transform();
    Vector3 forward = -gt.basis.get_column(2);
    Vector3 up = gt.basis.get_column(1);
    const bool use_sim_attenuation = _steam_sim_distance_attenuation_enabled(c);
    const int occ_type_ov = c.occlusion_type_override;
    const bool sim_occ = c.simulation_occlusion_enabled;
    const bool sim_tx = c.simulation_transmission_enabled;

    int baked_var = _compute_baked_data_variation(srv);

    Vector3 baked_center = get_global_position();
    Vector3 listener_pos_for_bake = get_global_position();
    Viewport* vp = get_viewport();
    if (vp && vp->get_camera_3d()) {
        listener_pos_for_bake = vp->get_camera_3d()->get_global_position();
    }
    if (baked_var == 1) {
        NodePath np = _config_node_path("current_baked_source");
        Node* n = np.is_empty() ? nullptr : get_node_or_null(np);
        Node3D* n3d = n ? Object::cast_to<Node3D>(n) : nullptr;
        baked_center = n3d ? n3d->get_global_position() : get_global_position();
    } else if (baked_var == 2) {
        NodePath np = _config_node_path("current_baked_listener");
        Node* n = np.is_empty() ? nullptr : get_node_or_null(np);
        Node3D* n3d = n ? Object::cast_to<Node3D>(n) : nullptr;
        baked_center = n3d ? n3d->get_global_position() : listener_pos_for_bake;
    }

    const bool sim_air_absorption = c.air_absorption_enabled && (c.air_absorption_input == 0);
    const bool eff_path_validation = (c.path_validation_override == -1) ? srv->get_default_path_validation_enabled() : (c.path_validation_override != 0);
    const bool eff_find_alternate = (c.find_alternate_paths_override == -1) ? srv->get_default_find_alternate_paths() : (c.find_alternate_paths_override != 0);
    // Mix levels gate IPL_SIMULATIONFLAGS in ResonanceServer::_update_source_internal (per-axis + skip all sim when all mixes 0).

    if (defer_if_sim_mutex_busy) {
        if (srv->uses_batch_source_updates()) {
            srv->enqueue_source_update(source_handle, get_global_position(), c.source_radius,
                                       forward, up, c.directivity_weight, c.directivity_power, sim_air_absorption,
                                       use_sim_attenuation, c.min_distance,
                                       eff_path_validation, eff_find_alternate,
                                       c.occlusion_samples, c.max_transmission_surfaces,
                                       baked_var, baked_center, resonance::kBakedEndpointRadius,
                                       pathing_batch, c.reflections_enabled, c.pathing_enabled_override,
                                       occ_type_ov, sim_occ, sim_tx,
                                       c.direct_mix_level, c.reflections_mix_level, c.pathing_mix_level);
        } else {
            srv->try_update_source(source_handle, get_global_position(), c.source_radius,
                                   forward, up, c.directivity_weight, c.directivity_power, sim_air_absorption,
                                   use_sim_attenuation, c.min_distance,
                                   eff_path_validation, eff_find_alternate,
                                   c.occlusion_samples, c.max_transmission_surfaces,
                                   baked_var, baked_center, resonance::kBakedEndpointRadius,
                                   pathing_batch, c.reflections_enabled, c.pathing_enabled_override,
                                   occ_type_ov, sim_occ, sim_tx,
                                   c.direct_mix_level, c.reflections_mix_level, c.pathing_mix_level);
        }
    } else {
        srv->update_source(source_handle, get_global_position(), c.source_radius,
                           forward, up, c.directivity_weight, c.directivity_power, sim_air_absorption,
                           use_sim_attenuation, c.min_distance,
                           eff_path_validation, eff_find_alternate,
                           c.occlusion_samples, c.max_transmission_surfaces,
                           baked_var, baked_center, resonance::kBakedEndpointRadius,
                           pathing_batch, c.reflections_enabled, c.pathing_enabled_override,
                           occ_type_ov, sim_occ, sim_tx,
                           c.direct_mix_level, c.reflections_mix_level, c.pathing_mix_level);
    }
}

void ResonancePlayer::_setup_attenuation(ResonanceServer* srv) {
    const ConfigCache& c = config_cache_;
    if (c.attenuation_mode == ATTENUATION_LINEAR || c.attenuation_mode == ATTENUATION_CUSTOM_CURVE) {
        PackedFloat32Array curve_samples;
        const int n = resonance::kAttenuationCurveSamples;
        if (c.attenuation_mode == ATTENUATION_LINEAR) {
            curve_samples.resize(n);
            for (int i = 0; i < n; i++)
                curve_samples[i] = 1.0f - (float)i / (n - 1); // Linear falloff
        } else if (c.attenuation_curve.is_valid()) {
            curve_samples.resize(n);
            for (int i = 0; i < n; i++) {
                float t = (float)i / (n - 1);
                curve_samples[i] = c.attenuation_curve->sample(t);
            }
        } else {
            curve_samples.resize(n);
            for (int i = 0; i < n; i++)
                curve_samples[i] = (i == 0) ? 1.0f : 0.0f;
        }
        srv->set_source_attenuation_callback_data(source_handle, c.attenuation_mode, c.min_distance, c.max_distance, curve_samples);
    } else if (c.attenuation_mode == ATTENUATION_INVERSE || c.attenuation_mode == ATTENUATION_INVERSE_NO_SIM) {
        PackedFloat32Array empty_curve;
        srv->set_source_attenuation_callback_data(source_handle, 0, c.min_distance, c.max_distance, empty_curve);
    }
}

void ResonancePlayer::_compute_listener_data(Viewport* vp, Vector3& out_listener_pos, IPLCoordinateSpace3& out_listener_orient) {
    out_listener_pos = Vector3(0, 0, 0);
    out_listener_orient = IPLCoordinateSpace3{};
    if (vp && vp->get_camera_3d()) {
        Camera3D* cam = vp->get_camera_3d();
        out_listener_pos = cam->get_global_position();
        Vector3 forward = -cam->get_global_transform().basis.get_column(2);
        Vector3 up = cam->get_global_transform().basis.get_column(1);
        Vector3 right = cam->get_global_transform().basis.get_column(0);
        out_listener_orient.origin = {out_listener_pos.x, out_listener_pos.y, out_listener_pos.z};
        out_listener_orient.ahead = {forward.x, forward.y, forward.z};
        out_listener_orient.up = {up.x, up.y, up.z};
        out_listener_orient.right = {right.x, right.y, right.z};
    }
}

void ResonancePlayer::_compute_attenuation(float dist, const OcclusionData& occ_data, float& out_attenuation, float& out_reverb_pathing_attenuation) {
    const ConfigCache& c = config_cache_;
    out_attenuation = 1.0f;
    if (c.attenuation_mode == ATTENUATION_INVERSE) {
        out_attenuation = occ_data.distance_attenuation;
    } else if (c.attenuation_mode == ATTENUATION_INVERSE_NO_SIM) {
        out_attenuation = 1.0f;
    } else if (c.attenuation_mode == ATTENUATION_LINEAR) {
        if (dist <= c.min_distance)
            out_attenuation = 1.0f;
        else if (c.max_distance <= c.min_distance || dist >= c.max_distance)
            out_attenuation = 0.0f;
        else
            out_attenuation = 1.0f - ((dist - c.min_distance) / (c.max_distance - c.min_distance));
    } else if (c.attenuation_mode == ATTENUATION_CUSTOM_CURVE) {
        if (c.attenuation_curve.is_valid() && c.max_distance > c.min_distance) {
            float t = (dist - c.min_distance) / (c.max_distance - c.min_distance);
            t = CLAMP(t, 0.0f, 1.0f);
            out_attenuation = c.attenuation_curve->sample(t);
        } else if (c.attenuation_curve.is_valid()) {
            out_attenuation = (dist <= c.min_distance) ? 1.0f : 0.0f;
        } else {
            out_attenuation = (dist >= c.max_distance) ? 0.0f : 1.0f;
        }
    }
    out_reverb_pathing_attenuation = out_attenuation;
    if (c.attenuation_mode == ATTENUATION_LINEAR || c.attenuation_mode == ATTENUATION_CUSTOM_CURVE) {
        float inverse_ref = (dist > 0.0f && c.min_distance > 0.0f)
                                ? (1.0f / (dist >= c.min_distance ? dist : c.min_distance))
                                : 1.0f;
        if (inverse_ref > 1.0f)
            inverse_ref = 1.0f;
        if (out_attenuation > inverse_ref)
            out_reverb_pathing_attenuation = inverse_ref;
        // Linear/curve direct gain hits 0 beyond max_distance; do not zero parametric/pathing wet the same way —
        // they still represent energy that should decay with distance (inverse_ref), not snap off (fixes silent mix
        // with pathing_applied and fetch hits while listener is past direct max_distance).
        if (out_attenuation <= 0.0f && inverse_ref > 0.0f)
            out_reverb_pathing_attenuation = inverse_ref;
    }
}

Vector3 ResonancePlayer::_apply_perspective_correction(Vector3 listener_pos, Viewport* vp, bool apply_perspective, float perspective_factor_val) {
    if (!apply_perspective || !vp || !vp->get_camera_3d())
        return get_global_position();
    Camera3D* cam = vp->get_camera_3d();
    Transform3D view_xform = cam->get_global_transform().affine_inverse();
    Vector3 view_pos = view_xform.xform(get_global_position());
    Projection proj = cam->get_camera_projection();
    Vector4 clip = proj.xform(Vector4(view_pos.x, view_pos.y, view_pos.z, 1.0f));
    if (clip.w <= 0.01f)
        return get_global_position();
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    ndc_x = CLAMP(ndc_x, -1.0f, 1.0f);
    ndc_y = CLAMP(ndc_y, -1.0f, 1.0f);
    float factor = perspective_factor_val;
    float sx = ndc_x * factor;
    float sy = ndc_y * factor;
    Vector3 dir_view(sx, sy, -1.0f);
    float len_sq = dir_view.length_squared();
    if (len_sq <= resonance::kDegenerateVectorEpsilon)
        return get_global_position();
    dir_view = dir_view / std::sqrt(len_sq);
    Vector3 dir_world = cam->get_global_transform().basis.xform(dir_view);
    return listener_pos + dir_world;
}

PlaybackParameters ResonancePlayer::_build_playback_params(const Vector3& listener_pos, const IPLCoordinateSpace3& listener_orient,
                                                           float attenuation, float reverb_pathing_attenuation, float dist, const Vector3& effective_source_pos,
                                                           float occ_val, float tx_low, float tx_mid, float tx_high, float directivity_val, const Vector3& air_abs,
                                                           bool has_reverb, bool direct_enabled, bool reverb_enabled) {
    const ConfigCache& c = config_cache_;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    PlaybackParameters new_params;
    new_params.source_handle = source_handle;
    new_params.occlusion = occ_val;
    new_params.transmission[0] = tx_low;
    new_params.transmission[1] = tx_mid;
    new_params.transmission[2] = tx_high;
    new_params.attenuation = attenuation;
    new_params.reverb_pathing_attenuation = reverb_pathing_attenuation;
    new_params.distance = dist;
    new_params.source_position = effective_source_pos;
    bool eff_use_binaural = (c.direct_binaural_override == -1) ? srv->use_direct_binaural() : (c.direct_binaural_override == 1);
    new_params.use_binaural = eff_use_binaural;
    new_params.apply_air_absorption = c.air_absorption_enabled;
    new_params.air_absorption[0] = air_abs.x;
    new_params.air_absorption[1] = air_abs.y;
    new_params.air_absorption[2] = air_abs.z;
    new_params.apply_directivity = c.directivity_enabled;
    new_params.directivity_value = directivity_val;
    new_params.apply_hrtf_to_reflections = (c.reverb_binaural_override == -1) ? srv->use_reverb_binaural() : (c.reverb_binaural_override == 1);
    new_params.apply_hrtf_to_pathing = (c.pathing_binaural_override == -1) ? srv->use_pathing_binaural() : (c.pathing_binaural_override == 1);
    new_params.listener_orientation = listener_orient;
    new_params.enable_direct = direct_enabled;
    new_params.enable_reverb = reverb_enabled;
    new_params.has_valid_reverb = has_reverb;
    new_params.spatial_blend = c.spatial_blend;
    new_params.use_ambisonics_encode = c.use_ambisonics_encode;
    new_params.direct_mix_level = c.direct_mix_level;
    new_params.reflections_mix_level = c.reflections_mix_level;
    new_params.pathing_mix_level = c.pathing_mix_level;
    new_params.reflections_eq[0] = c.reflections_eq_low;
    new_params.reflections_eq[1] = c.reflections_eq_mid;
    new_params.reflections_eq[2] = c.reflections_eq_high;
    new_params.reflections_delay = c.reflections_delay;
    new_params.reverb_split_output = reverb_split_output_;
    int eff_tx_type = resonance::kTransmissionFreqIndependent;
    bool eff_hrtf_bi = false;
    if (srv) {
        eff_tx_type = srv->get_transmission_type();
        eff_hrtf_bi = srv->get_hrtf_interpolation_bilinear();
    }
    if (c.transmission_type_override == resonance::kTransmissionFreqIndependent || c.transmission_type_override == resonance::kTransmissionFreqDependent)
        eff_tx_type = c.transmission_type_override;
    new_params.direct_effect_transmission_type = eff_tx_type;
    if (c.hrtf_interpolation_override == 0)
        eff_hrtf_bi = false;
    else if (c.hrtf_interpolation_override == 1)
        eff_hrtf_bi = true;
    new_params.direct_effect_hrtf_bilinear = eff_hrtf_bi;

    // Baked REVERB has no notion of source position (IR assumes source co-located with listener at each probe,
    // see Steam Audio SDK phonon.h around IPLBakedDataVariation). Damp the wet input by the same occlusion/
    // transmission factor that attenuates the direct path so walls also damp the reverb feed.
    // Realtime reflections and STATICSOURCE/STATICLISTENER already encode source→listener geometry in the IR
    // and keep factor 1.0 to avoid double-attenuation.
    // Per-player overrides beat the global runtime toggles so outdoor sources can opt in while indoor sources
    // keep plausible around-the-corner reverb.
    bool apply_occ_wet = false;
    if (srv) {
        switch (c.apply_occlusion_to_baked_reflections_override) {
        case 0:
            apply_occ_wet = false;
            break;
        case 1:
            apply_occ_wet = true;
            break;
        default:
            apply_occ_wet = srv->get_apply_occlusion_to_baked_reflections();
            break;
        }
    }
    float trans_amount = 1.0f;
    if (c.reverb_transmission_amount_input == 1)
        trans_amount = c.reverb_transmission_amount;
    else if (srv)
        trans_amount = srv->get_reverb_transmission_amount();
    new_params.wet_occlusion_factor = 1.0f;
    if (srv && apply_occ_wet && _compute_baked_data_variation(srv) == 0) {
        new_params.wet_occlusion_factor = resonance::baked_reverb_wet_occlusion_factor(
            occ_val, tx_low, tx_mid, tx_high, trans_amount);
    }

    // Unity parity: multiply the reflection-effect input by the player's 3D distance curve so the baked/realtime
    // wet path actually fades with distance. Steam Audio Unity's Spatialize pipeline bakes this into the inBuffer
    // (via AudioSource::rolloff); Nexus feeds the raw AudioStream ring, so we apply the curve explicitly here.
    bool apply_dist_wet = true;
    if (srv) {
        switch (c.apply_distance_curve_to_reflections_override) {
        case 0:
            apply_dist_wet = false;
            break;
        case 1:
            apply_dist_wet = true;
            break;
        default:
            apply_dist_wet = srv->get_apply_distance_curve_to_reflections();
            break;
        }
    }
    new_params.refl_distance_attenuation = apply_dist_wet ? attenuation : 1.0f;
    return new_params;
}

void ResonancePlayer::_prepare_source_for_simulation(ResonanceServer* srv) {
    _ensure_config_valid();
    config_cache_frame_countdown--;

    const ConfigCache& c = config_cache_;
    _setup_attenuation(srv);

    int32_t pathing_batch = -1;
    if (!pathing_probe_volume.is_empty()) {
        Node* node = get_node_or_null(pathing_probe_volume);
        ResonanceProbeVolume* pv = Object::cast_to<ResonanceProbeVolume>(node);
        if (pv) {
            pathing_batch = pv->get_probe_batch_handle();
        } else {
            // Target node gone (deleted/reparented). Auto-clear to avoid Godot NodePath error
            // when engine validates paths (e.g. scene save, inspector). EXIT_TREE clear on ProbeVolume
            // handles normal deletion; this catches edge cases (undo, cross-scene refs).
            set_pathing_probe_volume(NodePath());
        }
    }
    _apply_update_source(pathing_batch, true);
}

bool ResonancePlayer::_playback_lod_should_apply_playback_params(double delta, bool debug_hud_active, const Vector3& source_pos) {
    if (debug_hud_active)
        return true;
    const ConfigCache& c = config_cache_;
    const float iv = c.playback_parameter_min_interval;
    const float mv = c.playback_parameter_min_move;
    if (iv <= 0.0f && mv <= 0.0f)
        return true;
    playback_lod_time_since_full_ += delta;
    const bool hit_time = (iv > 0.0f) && (playback_lod_time_since_full_ >= static_cast<double>(iv));
    const float mv_sq = mv * mv;
    const bool hit_move = (mv > 0.0f) &&
                          (!playback_lod_have_anchor_ || (source_pos - playback_lod_anchor_pos_).length_squared() >= mv_sq);
    if (!hit_time && !hit_move)
        return false;
    playback_lod_time_since_full_ = 0.0;
    playback_lod_have_anchor_ = true;
    playback_lod_anchor_pos_ = source_pos;
    return true;
}

void ResonancePlayer::_apply_playback_params_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds) {
    const ConfigCache& c = config_cache_;
    Viewport* vp = get_viewport();
    Vector3 listener_pos;
    IPLCoordinateSpace3 listener_orient;
    _compute_listener_data(vp, listener_pos, listener_orient);

    OcclusionData occ_data = srv->get_source_occlusion_data(source_handle);
    float dist = get_global_position().distance_to(listener_pos);
    float attenuation;
    float reverb_pathing_attenuation;
    _compute_attenuation(dist, occ_data, attenuation, reverb_pathing_attenuation);

    Vector3 air_abs;
    if (c.air_absorption_enabled && c.air_absorption_input == 1) {
        air_abs.x = CLAMP(c.air_absorption_low, 0.0f, 1.0f);
        air_abs.y = CLAMP(c.air_absorption_mid, 0.0f, 1.0f);
        air_abs.z = CLAMP(c.air_absorption_high, 0.0f, 1.0f);
    } else {
        air_abs = Vector3(occ_data.air_absorption[0], occ_data.air_absorption[1], occ_data.air_absorption[2]);
    }
    float occ_val = (c.occlusion_input == 1) ? CLAMP(c.occlusion_value, 0.0f, 1.0f) : occ_data.occlusion;
    float tx_low = (c.transmission_input == 1) ? CLAMP(c.transmission_low, 0.0f, 1.0f) : occ_data.transmission[0];
    float tx_mid = (c.transmission_input == 1) ? CLAMP(c.transmission_mid, 0.0f, 1.0f) : occ_data.transmission[1];
    float tx_high = (c.transmission_input == 1) ? CLAMP(c.transmission_high, 0.0f, 1.0f) : occ_data.transmission[2];
    if (c.occlusion_input == 0 && !c.simulation_occlusion_enabled)
        occ_val = 1.0f;
    if (c.transmission_input == 0 && !c.simulation_transmission_enabled) {
        tx_low = 1.0f;
        tx_mid = 1.0f;
        tx_high = 1.0f;
    }
    float directivity_val = (c.directivity_input == 1) ? CLAMP(c.directivity_value, 0.0f, 1.0f) : occ_data.directivity;

    const float tau = c.playback_coeff_smoothing_time;
    const bool smooth_occ = (tau > 0.0f) && (c.occlusion_input == 0);
    const bool smooth_tx = (tau > 0.0f) && (c.transmission_input == 0);
    if (smooth_occ || smooth_tx) {
        const bool reinit = !coeff_smooth_initialized_ || (coeff_smooth_source_handle_ != source_handle);
        if (reinit) {
            if (smooth_occ)
                coeff_smooth_occ_ = occ_val;
            if (smooth_tx) {
                coeff_smooth_tx_[0] = tx_low;
                coeff_smooth_tx_[1] = tx_mid;
                coeff_smooth_tx_[2] = tx_high;
            }
            coeff_smooth_initialized_ = true;
            coeff_smooth_source_handle_ = source_handle;
        } else {
            float alpha = 1.0f;
            if (delta_seconds > 0.0 && std::isfinite(static_cast<double>(tau)) && tau > 0.0f) {
                const double t = std::max(static_cast<double>(tau), 1.0e-6);
                alpha = 1.0f - static_cast<float>(std::exp(-delta_seconds / t));
            }
            if (smooth_occ)
                coeff_smooth_occ_ += alpha * (occ_val - coeff_smooth_occ_);
            if (smooth_tx) {
                coeff_smooth_tx_[0] += alpha * (tx_low - coeff_smooth_tx_[0]);
                coeff_smooth_tx_[1] += alpha * (tx_mid - coeff_smooth_tx_[1]);
                coeff_smooth_tx_[2] += alpha * (tx_high - coeff_smooth_tx_[2]);
            }
        }
        if (smooth_occ)
            occ_val = std::clamp(coeff_smooth_occ_, 0.0f, 1.0f);
        if (smooth_tx) {
            tx_low = std::clamp(coeff_smooth_tx_[0], 0.0f, 1.0f);
            tx_mid = std::clamp(coeff_smooth_tx_[1], 0.0f, 1.0f);
            tx_high = std::clamp(coeff_smooth_tx_[2], 0.0f, 1.0f);
        }
    } else {
        coeff_smooth_initialized_ = false;
    }

    IPLReflectionEffectParams ignored_params{};
    bool has_reverb = srv->peek_reverb_params_likely_available(source_handle);
    if (!has_reverb)
        has_reverb = srv->fetch_reverb_params(source_handle, ignored_params);

    bool direct_enabled = (c.direct_mix_level > 0.0f) && (!srv || srv->is_output_direct_enabled());
    bool reverb_enabled = ((c.reflections_mix_level > 0.0f) || (c.pathing_mix_level > 0.0f)) && (!srv || srv->is_output_reverb_enabled());

    bool apply_perspective = (c.perspective_override == 1) || (c.perspective_override == -1 && srv->is_perspective_correction_enabled());
    float perspective_factor_val = (c.perspective_override == 1) ? CLAMP(c.perspective_factor, resonance::kPlayerPerspectiveFactorMin, resonance::kPlayerPerspectiveFactorMax) : srv->get_perspective_correction_factor();
    Vector3 effective_source_pos = _apply_perspective_correction(listener_pos, vp, apply_perspective, perspective_factor_val);

    PlaybackParameters new_params = _build_playback_params(listener_pos, listener_orient,
                                                           attenuation, reverb_pathing_attenuation, dist, effective_source_pos,
                                                           occ_val, tx_low, tx_mid, tx_high, directivity_val, air_abs,
                                                           has_reverb, direct_enabled, reverb_enabled);

    _broadcast_update_parameters(new_params);

    if (opt_debug_out) {
        opt_debug_out->source_pos = get_global_position();
        opt_debug_out->listener_pos = listener_pos;
        opt_debug_out->occlusion = occ_val;
        opt_debug_out->transmission[0] = tx_low;
        opt_debug_out->transmission[1] = tx_mid;
        opt_debug_out->transmission[2] = tx_high;
        opt_debug_out->attenuation = attenuation;
        opt_debug_out->distance = dist;
        opt_debug_out->air_absorption = air_abs;
        opt_debug_out->directivity_val = directivity_val;
        opt_debug_out->air_abs_enabled = c.air_absorption_enabled;
        opt_debug_out->directivity_enabled = c.directivity_enabled;
    }
}

void ResonancePlayer::_sync_player_debug_drawer(double delta, ResonanceServer* srv, const ResonanceDebugData& dbg_data, bool hud_active) {
    if (exclude_from_debug_)
        return;
    const bool occ = srv && srv->is_debug_occlusion_enabled();
    const bool ref = srv && srv->is_debug_reflections_enabled();
    debug_drawer.process(delta, dbg_data, occ, ref, get_name(), hud_active);
}

void ResonancePlayer::_push_playback_parameters_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds) {
    _prepare_source_for_simulation(srv);
    _apply_playback_params_from_simulation(srv, opt_debug_out, delta_seconds);
}

void ResonancePlayer::_deferred_push_playback_parameters() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid() || !is_playing())
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_simulating() || source_handle < 0)
        return;
    _push_playback_parameters_from_simulation(srv, nullptr, 0.0);
}

void ResonancePlayer::_notification(int p_what) {
    if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_CHILD_ORDER_CHANGED) {
        Engine* eng = Engine::get_singleton();
        if (!eng || !eng->is_editor_hint())
            _sync_physics_ray_auto_exclude_rids();
    }
    if (p_what == NOTIFICATION_ENTER_TREE) {
        Engine* eng = Engine::get_singleton();
        if (eng && !eng->is_editor_hint() && convert_animation_audio_tracks_at_runtime_ && player_config.is_valid())
            call_deferred("_nexus_deferred_spawn_anim_audio_helper");
    }
}

void ResonancePlayer::_clear_physics_ray_auto_exclude_rids() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && !registered_physics_auto_exclude_rids_.empty()) {
        for (const RID& r : registered_physics_auto_exclude_rids_)
            srv->unregister_physics_ray_auto_exclude_rid(r);
    }
    registered_physics_auto_exclude_rids_.clear();
}

void ResonancePlayer::_sync_physics_ray_auto_exclude_rids() {
    if (!physics_ray_auto_exclude_collision_bodies_)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->uses_custom_ray_tracer())
        return;
    _clear_physics_ray_auto_exclude_rids();
    std::vector<RID> found;
    collect_collision_object_rids_recursive(this, found);
    for (const RID& r : found) {
        srv->register_physics_ray_auto_exclude_rid(r);
        registered_physics_auto_exclude_rids_.push_back(r);
    }
}

void ResonancePlayer::_process(double delta) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;

    {
        ResonanceDirectivityDrawer::Params dp;
        dp.enabled = _config_bool("directivity_enabled", false);
        dp.input_mode = _config_int("directivity_input", 0);
        dp.weight = _config_float("directivity_weight", 0.0f);
        dp.power = _config_float("directivity_power", 1.0f);
        dp.user_value = _config_float("directivity_value", 1.0f);
        dp.size = 1.0f;
        directivity_drawer_.process(dp, show_directivity_gizmo_);
    }

    // Update audio-thread readable snapshot values.
    {
        const float vdb = get_volume_db();
        const float cap_db = get_max_db();
        const float eff_db = std::min(vdb, cap_db);
        const float lin = std::pow(10.0f, eff_db / 20.0f);
        cached_effective_volume_linear_.store(resonance::sanitize_audio_float(lin), std::memory_order_relaxed);
    }

    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (physics_ray_auto_exclude_collision_bodies_ && srv && srv->uses_custom_ray_tracer()) {
        if (++physics_auto_exclude_resync_counter_ >= 60) {
            physics_auto_exclude_resync_counter_ = 0;
            _sync_physics_ray_auto_exclude_rids();
        }
    } else {
        physics_auto_exclude_resync_counter_ = 0;
    }
    const bool dbg_occ = srv && srv->is_debug_occlusion_enabled();
    const bool dbg_ref = srv && srv->is_debug_reflections_enabled();
    const bool want_player_debug_ui = !exclude_from_debug_ && (dbg_occ || dbg_ref);
    const bool pipeline_ok = is_playing() && srv && srv->is_simulating() && source_handle >= 0;

    if (want_player_debug_ui) {
        if (pipeline_ok)
            debug_overlay_grace_timer_ = resonance::kDebugOverlayGraceSeconds;
        else
            debug_overlay_grace_timer_ -= delta;
    } else {
        debug_overlay_grace_timer_ = 0.0;
    }

    const bool show_debug_hud = want_player_debug_ui && (pipeline_ok || debug_overlay_grace_timer_ > 0.0);

    if (!is_playing()) {
        if (show_debug_hud && debug_overlay_has_last_data_)
            _sync_player_debug_drawer(delta, srv, debug_overlay_last_data_, true);
        else
            _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }

    if (srv && source_handle < 0 && srv->is_initialized())
        _try_ensure_source_and_sync(srv, true);

    if (!srv || !srv->is_simulating() || source_handle < 0) {
        if (show_debug_hud && debug_overlay_has_last_data_)
            _sync_player_debug_drawer(delta, srv, debug_overlay_last_data_, true);
        else
            _sync_player_debug_drawer(delta, srv, ResonanceDebugData{}, false);
        return;
    }

    _prepare_source_for_simulation(srv);
    _ensure_config_valid();
    const bool coeff_smooth_active = (config_cache_.playback_coeff_smoothing_time > 0.0f) &&
                                     ((config_cache_.occlusion_input == 0) || (config_cache_.transmission_input == 0));
    const bool apply_playback = _playback_lod_should_apply_playback_params(delta, show_debug_hud, get_global_position()) || coeff_smooth_active;

    ResonanceDebugData dbg_data;
    if (apply_playback)
        _apply_playback_params_from_simulation(srv, &dbg_data, delta);
    else
        dbg_data = debug_overlay_last_data_;

    // --- DEBUG DRAWING ---
    _aggregate_debug_signal_levels(dbg_data.signal_direct, dbg_data.signal_reverb, dbg_data.signal_pathing);

    if (apply_playback) {
        debug_overlay_last_data_ = dbg_data;
        debug_overlay_has_last_data_ = true;
    }
    _sync_player_debug_drawer(delta, srv, dbg_data, show_debug_hud);
}

bool ResonancePlayer::_try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing) {
    if (!player_config.is_valid() || source_handle >= 0)
        return false;
    if (!srv || !srv->is_initialized())
        return false;

    const float eff_radius = _config_float("source_radius", 1.0f);
    const int32_t h = srv->create_source_handle(get_global_position(), eff_radius);
    if (h < 0) {
        if (!warned_source_handle_create_failed_) {
            warned_source_handle_create_failed_ = true;
            ResonanceLog::warn(
                "ResonancePlayer: create_source_handle failed (is the simulator ready?). Reverb/occlusion may stay dry until it succeeds.");
        }
        return false;
    }
    warned_source_handle_create_failed_ = false;
    source_handle = h;
    _prepare_source_for_simulation(srv);
    if (deferred_playback_push_if_playing && is_playing())
        call_deferred("_deferred_push_playback_parameters");
    return true;
}

void ResonancePlayer::_deferred_try_ensure_source_after_config() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid())
        return;
    _update_stream_setup();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _try_ensure_source_and_sync(srv, is_playing());
}

void ResonancePlayer::_ensure_source_exists() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    (void)_try_ensure_source_and_sync(srv, false);
}

void ResonancePlayer::_start_reverb_split_child_if_needed() {
    Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
    if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
        if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child)) {
            if (!rp->is_playing())
                rp->play();
        }
    }
}

void ResonancePlayer::set_stream(const Ref<AudioStream>& p_stream) {
    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();
    if (!player_config.is_valid()) {
        logical_stream_.unref();
        internal_stream.unref();
        AudioStreamPlayer3D::set_stream(p_stream);
        return;
    }
    logical_stream_ = p_stream;
    if (editor) {
        internal_stream.unref();
        AudioStreamPlayer3D::set_stream(p_stream);
        return;
    }
    if (!internal_stream.is_valid())
        internal_stream.instantiate();
    internal_stream->set_base_stream(logical_stream_);
    internal_stream->set_stream_owner(this);
    AudioStreamPlayer3D::set_stream(internal_stream);
}

Ref<AudioStream> ResonancePlayer::get_stream() const {
    if (!player_config.is_valid())
        return AudioStreamPlayer3D::get_stream();
    if (logical_stream_.is_valid())
        return logical_stream_;
    return AudioStreamPlayer3D::get_stream();
}

void ResonancePlayer::play(float from_position) {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        AudioStreamPlayer3D::play(from_position);
        return;
    }
    if (player_config.is_valid()) {
        playback_lod_have_anchor_ = false;
        playback_lod_time_since_full_ = 0.0;
        _update_stream_setup();
        ResonanceServer* srv = ResonanceServer::get_singleton();
        _try_ensure_source_and_sync(srv, false);
    }
    AudioStreamPlayer3D::play(from_position);
    if (player_config.is_valid()) {
        _start_reverb_split_child_if_needed();
        // Synchronously seed the freshly instantiated ResonanceStreamPlayback(s) with valid
        // spatial parameters so the audio thread's first _mix block opens the params gate
        // immediately and mixes from the source's real world position / attenuation instead
        // of waiting a frame for the deferred broadcast. _try_ensure_source_and_sync above
        // has already ensured source_handle >= 0 when possible; _push_playback_parameters_
        // _from_simulation tolerates empty simulation caches (defaults to occlusion=1 / no
        // reverb) which still produces correct 3D positioning.
        ResonanceServer* srv = ResonanceServer::get_singleton();
        if (srv && srv->is_simulating() && source_handle >= 0)
            _push_playback_parameters_from_simulation(srv, nullptr, 0.0);
        // Keep the deferred push as safety net in case source_handle was not yet available
        // this tick (worker still spinning up, late attach, etc.).
        call_deferred("_deferred_push_playback_parameters");
    }
}

void ResonancePlayer::_update_stream_setup() {
    if (!player_config.is_valid())
        return;
    const Ref<AudioStream> engine_s = AudioStreamPlayer3D::get_stream();
    if (internal_stream.is_valid() && engine_s == internal_stream) {
        if (ResonanceStream* ris = Object::cast_to<ResonanceStream>(internal_stream.ptr())) {
            ris->set_stream_owner(this);
            ris->set_base_stream(logical_stream_);
        }
        return;
    }
    logical_stream_ = engine_s;
    if (!internal_stream.is_valid())
        internal_stream.instantiate();
    internal_stream->set_base_stream(logical_stream_);
    internal_stream->set_stream_owner(this);
    AudioStreamPlayer3D::set_stream(internal_stream);
}

void ResonancePlayer::play_stream(double from_pos) {
    // GDExtension may narrow to float internally; keep double at call site.
    // NOLINTNEXTLINE(bugprone-narrowing-conversions)
    play(static_cast<float>(from_pos));
}

void ResonancePlayer::play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position) {
    set_stream(p_stream);
    play(from_position);
}

void ResonancePlayer::stop() {
    warned_source_handle_create_failed_ = false;
    playback_lod_have_anchor_ = false;
    playback_lod_time_since_full_ = 0.0;
    coeff_smooth_initialized_ = false;
    coeff_smooth_source_handle_ = -1;

    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();

    // Editor or fallback (no player_config / non-Resonance playback): use the original
    // hard-cutoff behaviour to match plain AudioStreamPlayer3D semantics.
    if (editor || !player_config.is_valid()) {
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
        return;
    }

    // Soft-stop: halt the dry input on each active ResonanceStreamPlayback voice but keep
    // the playbacks themselves alive while the reverb / pathing tail decays. We deliberately
    // do NOT call AudioStreamPlayer3D::stop() so Godot's audio engine keeps invoking _mix
    // on each voice while ResonanceStreamPlayback::_is_playing() returns true (which it does
    // while effect tails or output rings still hold residue, capped by max_reverb_duration).
    // Once _is_playing() finally returns false the AudioServer detaches the playback.
    // The reverb-split child is left running too; ResonanceReverbPlayback::_is_playing()
    // returns false on its own once both the parent and the split-reverb rings are empty.
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    bool any_soft_stopped = false;
    for (ResonanceStreamPlayback* pb : copy) {
        if (pb) {
            pb->request_soft_stop();
            any_soft_stopped = true;
        }
    }
    if (!any_soft_stopped) {
        // No active ResonanceStreamPlayback voices (e.g. AudioStreamPolyphonic via runtime
        // animation conversion, or the player was never played). Fall back to the plain
        // hard-cutoff path so callers see the expected stop() semantics immediately.
        Node* reverb_child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (reverb_child && reverb_child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(reverb_child))
                rp->stop();
        }
        AudioStreamPlayer3D::stop();
    }
}

void ResonancePlayer::set_pathing_probe_volume(const NodePath& p_path) { pathing_probe_volume = p_path; }
NodePath ResonancePlayer::get_pathing_probe_volume() const { return pathing_probe_volume; }

void ResonancePlayer::clear_pathing_probe_immediate() {
    pathing_probe_volume = NodePath();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized() || source_handle < 0)
        return;
    _ensure_config_and_apply_source(-1);
}
void ResonancePlayer::set_physics_ray_auto_exclude_collision_bodies(bool p_enable) {
    if (physics_ray_auto_exclude_collision_bodies_ == p_enable)
        return;
    physics_ray_auto_exclude_collision_bodies_ = p_enable;
    if (!p_enable)
        _clear_physics_ray_auto_exclude_rids();
    else
        _sync_physics_ray_auto_exclude_rids();
}

void ResonancePlayer::set_exclude_from_debug(bool p_exclude) { exclude_from_debug_ = p_exclude; }

void ResonancePlayer::set_convert_animation_audio_tracks_at_runtime(bool p_enable) {
    convert_animation_audio_tracks_at_runtime_ = p_enable;
    Engine* eng = Engine::get_singleton();
    if (p_enable && player_config.is_valid() && is_inside_tree() && eng && !eng->is_editor_hint())
        call_deferred("_nexus_deferred_spawn_anim_audio_helper");
}

void ResonancePlayer::set_player_config(const Ref<Resource>& p_config) {
    const bool had_config = player_config.is_valid();
    const Callable changed_cb = callable_mp(this, &ResonancePlayer::_on_player_config_changed_refresh_gizmo);
    if (had_config && player_config->is_connected("changed", changed_cb))
        player_config->disconnect("changed", changed_cb);
    player_config = p_config;
    config_cache_valid_ = false;
    if (had_config && !player_config.is_valid()) {
        const Ref<AudioStream> logical = logical_stream_;
        internal_stream.unref();
        logical_stream_.unref();
        AudioStreamPlayer3D::set_stream(logical);
    } else if (player_config.is_valid()) {
        call_deferred("_deferred_try_ensure_source_after_config");
        Engine* eng = Engine::get_singleton();
        if (eng && !eng->is_editor_hint() && convert_animation_audio_tracks_at_runtime_ && is_inside_tree())
            call_deferred("_nexus_deferred_spawn_anim_audio_helper");
    }
    if (player_config.is_valid() && !player_config->is_connected("changed", changed_cb))
        player_config->connect("changed", changed_cb);
    _on_player_config_changed_refresh_gizmo();
}
Ref<Resource> ResonancePlayer::get_player_config() const { return player_config; }

void ResonancePlayer::set_show_directivity_gizmo(bool p_enable) {
    if (show_directivity_gizmo_ == p_enable)
        return;
    show_directivity_gizmo_ = p_enable;
    directivity_drawer_.mark_dirty();
    update_gizmos();
}

void ResonancePlayer::_on_player_config_changed_refresh_gizmo() {
    directivity_drawer_.mark_dirty();
    update_gizmos();
}

PackedVector3Array ResonancePlayer::build_directivity_gizmo_lines(
    bool enabled, int input_mode, float weight, float power, float user_value, float size) {
    PackedVector3Array lines;
    if (size <= 0.0f)
        size = 1.0f;

    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    constexpr int kMeridianSegments = 64;
    constexpr int kRingSegments = 32;

    const bool is_dipole = enabled && input_mode == 0;
    const float w = std::clamp(weight, 0.0f, 1.0f);
    const float p = std::max(0.0f, power);
    const float uv = std::clamp(user_value, 0.0f, 1.0f);

    auto sample = [&](float theta) -> float {
        if (!is_dipole)
            return 1.0f;
        const float c = std::cos(theta);
        const float v = std::fabs((1.0f - w) + w * c);
        if (p == 0.0f)
            return 1.0f;
        return std::pow(v, p);
    };

    auto meridian_xz = [&](float theta) -> Vector3 {
        const float r = sample(theta) * size;
        return Vector3(std::sin(theta) * r, 0.0f, -std::cos(theta) * r);
    };
    auto meridian_yz = [&](float theta) -> Vector3 {
        const float r = sample(theta) * size;
        return Vector3(0.0f, std::sin(theta) * r, -std::cos(theta) * r);
    };

    for (int i = 0; i < kMeridianSegments; ++i) {
        const float t0 = (float)i / kMeridianSegments * kTwoPi;
        const float t1 = (float)(i + 1) / kMeridianSegments * kTwoPi;
        lines.push_back(meridian_xz(t0));
        lines.push_back(meridian_xz(t1));
        lines.push_back(meridian_yz(t0));
        lines.push_back(meridian_yz(t1));
    }

    // Azimuth rings (circles in the XY plane at constant theta; axis-symmetry around -Z).
    float ring_thetas[8];
    int ring_count = 0;
    if (is_dipole) {
        ring_thetas[ring_count++] = kPi / 6.0f;
        ring_thetas[ring_count++] = kPi / 3.0f;
        ring_thetas[ring_count++] = 2.0f * kPi / 3.0f;
        ring_thetas[ring_count++] = 5.0f * kPi / 6.0f;
    } else {
        ring_thetas[ring_count++] = kPi * 0.5f; // equator only
    }
    for (int idx = 0; idx < ring_count; ++idx) {
        const float theta = ring_thetas[idx];
        const float r = sample(theta) * size;
        if (r < 1e-4f)
            continue;
        const float ring_radius = std::sin(theta) * r;
        const float z = -std::cos(theta) * r;
        if (ring_radius < 1e-4f)
            continue;
        for (int i = 0; i < kRingSegments; ++i) {
            const float a0 = (float)i / kRingSegments * kTwoPi;
            const float a1 = (float)(i + 1) / kRingSegments * kTwoPi;
            const Vector3 p0(std::cos(a0) * ring_radius, std::sin(a0) * ring_radius, z);
            const Vector3 p1(std::cos(a1) * ring_radius, std::sin(a1) * ring_radius, z);
            lines.push_back(p0);
            lines.push_back(p1);
        }
    }

    // Forward arrow on -Z: for dipole the length matches the forward lobe, for user-defined we scale
    // by the directivity_value so a near-muted scalar shows visually.
    float arrow_len = size;
    if (is_dipole)
        arrow_len = sample(0.0f) * size;
    else if (enabled && input_mode == 1)
        arrow_len = size * std::max(0.05f, uv);
    const Vector3 tip(0.0f, 0.0f, -arrow_len);
    lines.push_back(Vector3(0, 0, 0));
    lines.push_back(tip);
    const float head = size * 0.08f;
    lines.push_back(tip);
    lines.push_back(tip + Vector3(head, 0.0f, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(-head, 0.0f, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(0.0f, head, head));
    lines.push_back(tip);
    lines.push_back(tip + Vector3(0.0f, -head, head));

    return lines;
}

void ResonancePlayer::set_reverb_split_output(bool p_enable, const StringName& p_reverb_bus) {
    if (reverb_split_output_ != p_enable) {
        reverb_split_output_ = p_enable;
        _update_reverb_split_child(p_reverb_bus);
    } else if (reverb_split_output_ && !p_reverb_bus.is_empty()) {
        Node* child = get_node_or_null(NodePath("ResonanceReverbOutput"));
        if (child && child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(child))
                rp->set_bus(p_reverb_bus);
        }
    }
}

void ResonancePlayer::_update_reverb_split_child(const StringName& p_reverb_bus) {
    const NodePath child_path = NodePath("ResonanceReverbOutput");
    Node* child = get_node_or_null(child_path);
    if (reverb_split_output_) {
        if (!child) {
            AudioStreamPlayer* reverb_player = memnew(AudioStreamPlayer);
            reverb_player->set_name("ResonanceReverbOutput");
            Ref<ResonanceReverbStream> reverb_stream;
            reverb_stream.instantiate();
            reverb_stream->set_parent_player(this);
            reverb_player->set_stream(reverb_stream);
            if (!p_reverb_bus.is_empty())
                reverb_player->set_bus(p_reverb_bus);
            add_child(reverb_player);
            if (is_playing())
                reverb_player->play();
        } else if (!p_reverb_bus.is_empty() && child->is_class("AudioStreamPlayer")) {
            if (AudioStreamPlayer* rp = Object::cast_to<AudioStreamPlayer>(child))
                rp->set_bus(p_reverb_bus);
        }
    } else if (child) {
        child->queue_free();
    }
}

void ResonancePlayer::_nexus_deferred_spawn_anim_audio_helper() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!convert_animation_audio_tracks_at_runtime_ || !player_config.is_valid())
        return;
    if (get_node_or_null(NodePath("NexusAnimationAudioRuntimeHelper")))
        return;
    Ref<Resource> res;
    if (ResourceLoader* rl = ResourceLoader::get_singleton())
        res = rl->load("res://addons/nexus_resonance/nexus_animation_audio_runtime_helper.gd");
    Ref<Script> script = res;
    if (script.is_null())
        return;
    Node* helper = memnew(Node);
    helper->set_name(String("NexusAnimationAudioRuntimeHelper"));
    helper->set_script(script);
    add_child(helper);
}

Dictionary ResonancePlayer::get_audio_instrumentation() {
    Dictionary d;
    if (!player_config.is_valid())
        return d;
    d["godot_attenuation_model"] = (int)get_attenuation_model();
    d["godot_pitch_scale"] = get_pitch_scale();
    d["godot_max_distance"] = get_max_distance();
    d["godot_max_db"] = get_max_db();
    d["godot_unit_size"] = get_unit_size();

    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    if (copy.empty()) {
        if (ResonanceStreamPlayback* res_pb = _get_resonance_playback())
            copy.push_back(res_pb);
    }
    if (copy.empty())
        return d;

    uint64_t sum_input_dropped = 0, sum_output_underrun = 0, sum_output_blocked = 0, sum_mix_calls = 0, sum_blocks = 0;
    uint64_t sum_passthrough = 0, sum_reverb_miss = 0, max_block_us = 0;
    uint64_t sum_late_mix = 0, sum_param_syncs = 0, sum_zero_input = 0;
    uint64_t max_last_mix_gap_us = 0, max_mix_gap_us = 0, max_expected_mix_gap_us = 0;
    int32_t agg_mix_frames_min = std::numeric_limits<int32_t>::max();
    int32_t agg_mix_frames_max = 0;
    uint64_t sum_silent_blocks = 0;
    float max_last_rms = 0.0f;
    float max_path_sh_rms = 0.0f, max_path_sh_energy = 0.0f, max_path_out_rms = 0.0f;
    int32_t max_path_order = -1;
    uint64_t sum_reverb_ring_samples = 0; // Test hook: split-reverb tail residue across all voices.

    for (ResonanceStreamPlayback* res_pb : copy) {
        if (!res_pb)
            continue;
        uint64_t input_dropped = 0, output_underrun = 0, output_blocked = 0, mix_calls = 0, blocks = 0;
        uint64_t passthrough = 0, reverb_miss = 0, block_us = 0;
        uint64_t late_mix = 0, last_mix_gap_us = 0, max_mix_gap_us_local = 0, expected_mix_gap_us = 0;
        uint64_t param_syncs = 0, zero_input = 0;
        int32_t mix_frames_min = std::numeric_limits<int32_t>::max(), mix_frames_max = 0;
        uint64_t silent_blocks = 0;
        float last_rms = 0.0f;
        float path_sh_rms = 0.0f, path_sh_energy = 0.0f, path_out_rms = 0.0f;
        int32_t path_order = -1;
        res_pb->get_instrumentation_snapshot(input_dropped, output_underrun, output_blocked, mix_calls, blocks,
                                             passthrough, reverb_miss, block_us, late_mix, last_mix_gap_us, max_mix_gap_us_local,
                                             expected_mix_gap_us, param_syncs, zero_input,
                                             mix_frames_min, mix_frames_max, silent_blocks, last_rms,
                                             path_sh_rms, path_sh_energy, path_out_rms, path_order);
        sum_input_dropped += input_dropped;
        sum_output_underrun += output_underrun;
        sum_output_blocked += output_blocked;
        sum_mix_calls += mix_calls;
        sum_blocks += blocks;
        sum_passthrough += passthrough;
        sum_reverb_miss += reverb_miss;
        max_block_us = std::max(max_block_us, block_us);
        sum_late_mix += late_mix;
        sum_param_syncs += param_syncs;
        sum_zero_input += zero_input;
        max_last_mix_gap_us = std::max(max_last_mix_gap_us, last_mix_gap_us);
        max_mix_gap_us = std::max(max_mix_gap_us, max_mix_gap_us_local);
        max_expected_mix_gap_us = std::max(max_expected_mix_gap_us, expected_mix_gap_us);
        // Debugging signal completion: whether the playback ever returns 0 (EOS) and how long it stayed gated.
        // (Aggregated like the other counters; only surfaced in the dictionary below.)
        // Reuse sum_* locals for aggregation.
        // NOTE: We intentionally do not cap these; they are for troubleshooting.
        // (Declared above as sum_* variables.)
        // --- aggregation ---
        // (see vars declared at top of function)
        if (mix_frames_min < agg_mix_frames_min)
            agg_mix_frames_min = mix_frames_min;
        agg_mix_frames_max = std::max(agg_mix_frames_max, mix_frames_max);
        sum_silent_blocks += silent_blocks;
        max_last_rms = std::max(max_last_rms, last_rms);
        max_path_sh_rms = std::max(max_path_sh_rms, path_sh_rms);
        max_path_sh_energy = std::max(max_path_sh_energy, path_sh_energy);
        max_path_out_rms = std::max(max_path_out_rms, path_out_rms);
        max_path_order = std::max(max_path_order, path_order);
        sum_reverb_ring_samples += (uint64_t)res_pb->get_reverb_ring_available_read();
    }

    if (agg_mix_frames_min == std::numeric_limits<int32_t>::max())
        agg_mix_frames_min = 0;

    d["input_dropped"] = (int64_t)sum_input_dropped;
    d["output_underrun"] = (int64_t)sum_output_underrun;
    d["output_blocked"] = (int64_t)sum_output_blocked;
    d["mix_calls"] = (int64_t)sum_mix_calls;
    d["blocks_processed"] = (int64_t)sum_blocks;
    d["passthrough_blocks"] = (int64_t)sum_passthrough;
    d["reverb_miss_blocks"] = (int64_t)sum_reverb_miss;
    d["max_block_time_us"] = (int64_t)max_block_us;
    d["late_mix_count"] = (int64_t)sum_late_mix;
    d["last_mix_gap_us"] = (int64_t)max_last_mix_gap_us;
    d["max_mix_gap_us"] = (int64_t)max_mix_gap_us;
    d["expected_mix_gap_us"] = (int64_t)max_expected_mix_gap_us;
    d["param_sync_count"] = (int64_t)sum_param_syncs;
    d["zero_input_count"] = (int64_t)sum_zero_input;
    d["mix_frames_min"] = (int)agg_mix_frames_min;
    d["mix_frames_max"] = (int)agg_mix_frames_max;
    d["silent_output_blocks"] = (int64_t)sum_silent_blocks;
    d["last_output_rms"] = max_last_rms;
    d["pathing_sh_rms"] = max_path_sh_rms;
    d["pathing_sh_energy"] = max_path_sh_energy;
    d["pathing_out_rms"] = max_path_out_rms;
    d["pathing_sh_order"] = (int)max_path_order;
    d["polyphony_voice_count"] = (int)copy.size();
    d["reverb_ring_samples"] = (int64_t)sum_reverb_ring_samples;
    return d;
}

void ResonancePlayer::reset_audio_instrumentation() {
    std::vector<ResonanceStreamPlayback*> copy;
    internal_copy_internal_playbacks(copy);
    if (copy.empty()) {
        if (ResonanceStreamPlayback* res_pb = _get_resonance_playback())
            copy.push_back(res_pb);
    }
    for (ResonanceStreamPlayback* res_pb : copy) {
        if (res_pb)
            res_pb->reset_instrumentation();
    }
}

void ResonancePlayer::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_stream", "stream"), &ResonancePlayer::set_stream);
    ClassDB::bind_method(D_METHOD("get_stream"), &ResonancePlayer::get_stream);
    ClassDB::bind_method(D_METHOD("play", "from_position"), &ResonancePlayer::play, DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("stop"), &ResonancePlayer::stop);
    ClassDB::bind_method(D_METHOD("play_stream", "from_position"), &ResonancePlayer::play_stream, DEFVAL(0.0));
    ClassDB::bind_method(D_METHOD("play_animation_audio_clip", "stream", "from_position"), &ResonancePlayer::play_animation_audio_clip,
                         DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("_deferred_try_ensure_source_after_config"), &ResonancePlayer::_deferred_try_ensure_source_after_config);
    ClassDB::bind_method(D_METHOD("set_pathing_probe_volume", "p_path"), &ResonancePlayer::set_pathing_probe_volume);
    ClassDB::bind_method(D_METHOD("get_pathing_probe_volume"), &ResonancePlayer::get_pathing_probe_volume);
    ClassDB::bind_method(D_METHOD("set_exclude_from_debug", "p_exclude"), &ResonancePlayer::set_exclude_from_debug);
    ClassDB::bind_method(D_METHOD("get_exclude_from_debug"), &ResonancePlayer::get_exclude_from_debug);
    ClassDB::bind_method(D_METHOD("set_physics_ray_auto_exclude_collision_bodies", "p_enable"), &ResonancePlayer::set_physics_ray_auto_exclude_collision_bodies);
    ClassDB::bind_method(D_METHOD("get_physics_ray_auto_exclude_collision_bodies"), &ResonancePlayer::get_physics_ray_auto_exclude_collision_bodies);
    ClassDB::bind_method(D_METHOD("set_player_config", "p_config"), &ResonancePlayer::set_player_config);
    ClassDB::bind_method(D_METHOD("get_player_config"), &ResonancePlayer::get_player_config);
    ClassDB::bind_method(D_METHOD("set_reverb_split_output", "p_enable", "p_reverb_bus"), &ResonancePlayer::set_reverb_split_output, DEFVAL(StringName()));
    ClassDB::bind_method(D_METHOD("get_reverb_split_output"), &ResonancePlayer::get_reverb_split_output);
    ClassDB::bind_method(D_METHOD("get_audio_instrumentation"), &ResonancePlayer::get_audio_instrumentation);
    ClassDB::bind_method(D_METHOD("reset_audio_instrumentation"), &ResonancePlayer::reset_audio_instrumentation);
    ClassDB::bind_method(D_METHOD("_deferred_push_playback_parameters"), &ResonancePlayer::_deferred_push_playback_parameters);
    ClassDB::bind_method(D_METHOD("_nexus_deferred_spawn_anim_audio_helper"), &ResonancePlayer::_nexus_deferred_spawn_anim_audio_helper);
    ClassDB::bind_method(D_METHOD("set_convert_animation_audio_tracks_at_runtime", "p_enable"),
                         &ResonancePlayer::set_convert_animation_audio_tracks_at_runtime);
    ClassDB::bind_method(D_METHOD("get_convert_animation_audio_tracks_at_runtime"),
                         &ResonancePlayer::get_convert_animation_audio_tracks_at_runtime);
    ClassDB::bind_method(D_METHOD("set_show_directivity_gizmo", "p_enable"), &ResonancePlayer::set_show_directivity_gizmo);
    ClassDB::bind_method(D_METHOD("get_show_directivity_gizmo"), &ResonancePlayer::get_show_directivity_gizmo);
    ClassDB::bind_method(D_METHOD("_on_player_config_changed_refresh_gizmo"), &ResonancePlayer::_on_player_config_changed_refresh_gizmo);
    ClassDB::bind_static_method("ResonancePlayer",
                                D_METHOD("build_directivity_gizmo_lines", "enabled", "input_mode", "weight", "power", "user_value", "size"),
                                &ResonancePlayer::build_directivity_gizmo_lines);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "player_config", PROPERTY_HINT_RESOURCE_TYPE, "ResonancePlayerConfig"), "set_player_config", "get_player_config");
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "pathing_probe_volume", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "ResonanceProbeVolume"), "set_pathing_probe_volume", "get_pathing_probe_volume");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "exclude_from_debug"), "set_exclude_from_debug", "get_exclude_from_debug");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "physics_ray_auto_exclude_collision_bodies"), "set_physics_ray_auto_exclude_collision_bodies",
                 "get_physics_ray_auto_exclude_collision_bodies");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "convert_animation_audio_tracks_at_runtime"), "set_convert_animation_audio_tracks_at_runtime",
                 "get_convert_animation_audio_tracks_at_runtime");
    ADD_GROUP("Debug", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_directivity_gizmo"), "set_show_directivity_gizmo", "get_show_directivity_gizmo");

    BIND_ENUM_CONSTANT(ATTENUATION_INVERSE);
    BIND_ENUM_CONSTANT(ATTENUATION_LINEAR);
    BIND_ENUM_CONSTANT(ATTENUATION_CUSTOM_CURVE);
    BIND_ENUM_CONSTANT(ATTENUATION_INVERSE_NO_SIM);
}