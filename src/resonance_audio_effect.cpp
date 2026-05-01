#include "resonance_audio_effect.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include <algorithm>
#include <chrono>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// Warn once per process when frame_count != server frame_size to avoid log spam.
static bool s_frame_size_mismatch_warned = false;

// --- INSTANCE ---
// Reinit/shutdown: ResonanceServer drains registered clients under AudioServer::lock before iplContextRelease.

void ResonanceAudioEffectInstance::ipl_context_reinit_cleanup(void* userdata) {
    if (!userdata)
        return;
    static_cast<ResonanceAudioEffectInstance*>(userdata)->_reset_ipl_mixer_for_context_lifecycle();
}

void ResonanceAudioEffectInstance::_reset_ipl_mixer_for_context_lifecycle() {
    if (ResonanceServer* srv = ResonanceServer::get_singleton())
        srv->unregister_ipl_context_client(this);
    processor.cleanup();
    initialized_processor = false;
}

ResonanceAudioEffectInstance::~ResonanceAudioEffectInstance() {
    _reset_ipl_mixer_for_context_lifecycle();
}

void ResonanceAudioEffectInstance::_process(const void* src_buffer, AudioFrame* dst_buffer, int32_t frame_count) {
    ResonanceServer* srv = ResonanceServer::get_singleton();

    // Bail during exit or Steam Audio reinit/teardown (TOCTOU guard below before process_mixer_return).
    if (!srv || !srv->is_initialized() || ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    int server_frame_size = srv->get_audio_frame_size();

    // --- INITIALIZATION ---
    if (!initialized_processor) {
        ResonanceLog::info("AudioEffect: Initializing MixerProcessor with frame size: " + String::num(server_frame_size));
        processor.initialize(
            srv->get_context_handle(),
            srv->get_sample_rate(),
            server_frame_size, // Must match ReflectionMixer / Steam Audio block size
            srv->get_ambisonic_order());
        if (!processor.is_ready()) {
            ResonanceLog::error("ResonanceAudioEffect: MixerProcessor initialization failed. Reverb will be silent until init succeeds.");
            return;
        }
        if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
            reg_srv->register_ipl_context_client(this, &ResonanceAudioEffectInstance::ipl_context_reinit_cleanup);
        initialized_processor = true;
    }

    // Validate frame_count matches server (mismatch causes crackling / buffer overrun)
    if (frame_count != server_frame_size) {
        if (srv->get_audio_frame_size_was_auto()) {
            srv->request_reinit_with_frame_size(frame_count);
        }
        if (!s_frame_size_mismatch_warned) {
            s_frame_size_mismatch_warned = true;
            UtilityFunctions::push_warning("Nexus Resonance: Reverb bus frame_count (" + String::num_int64(frame_count) + ") != audio_frame_size (" + String::num_int64(server_frame_size) + "). Set ResonanceRuntimeConfig.audio_frame_size to Auto (0) to derive from Project Settings, or match manually.");
        }
    }

    auto mixer_guard = srv->scoped_mixer_read();
    IPLReflectionMixer mixer = mixer_guard.get();
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }
    // No mixer for Parametric (1) or Hybrid (2) - per Steam Audio docs mixer cannot be used with those
    if (!mixer) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        srv->update_reverb_effect_instrumentation(true, false, 0, 0.0f);
        return;
    }

    // Clear output; we replace with mixer content (ignore bus input)
    for (int i = 0; i < frame_count; i++) {
        dst_buffer[i].left = 0.0f;
        dst_buffer[i].right = 0.0f;
    }

    // --- PROCESSING ---
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int i = 0; i < frame_count; i++) {
            dst_buffer[i].left = 0.0f;
            dst_buffer[i].right = 0.0f;
        }
        return;
    }

    IPLCoordinateSpace3 listener_coords = srv->get_current_listener_coords();

    // Steam Audio Unity: per-source effects (spatialize) call iplReflectionEffectApply into the shared mixer
    // before the Mix Return effect runs iplReflectionMixerApply on the same DSP tick. Godot should run
    // ResonanceStreamPlayback::_mix (which feeds the mixer) before this bus _process for send routing;
    // matching ResonanceServer::audio_frame_size to the bus frame_count avoids a one-block feed/pull skew.
    const auto bus_t0 = std::chrono::steady_clock::now();
    bool success = processor.process_mixer_return(mixer, listener_coords, dst_buffer, frame_count);
    const auto bus_t1 = std::chrono::steady_clock::now();
    srv->record_convolution_reverb_bus_usec(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(bus_t1 - bus_t0).count()));

    float peak = 0.0f;
    int32_t frames_written = 0;
    if (success) {
        // Do not call iplReflectionMixerReset here. Steam Audio Unity mix_return_effect.cpp only uses
        // iplReflectionMixerApply to pull accumulated mixer audio; Reset would clear internal mixer
        // state between Apply (sources) and Apply (bus), causing choppy / gated wet output when feed
        // and pull run on different scheduling phases.
        frames_written = frame_count;

        float gain = 1.0f;
        if (effect_ref.is_valid()) {
            gain = static_cast<float>(UtilityFunctions::db_to_linear(effect_ref->get_gain_db()));
        }

        // --- SAFETY: Apply gain, then output limiter to prevent ear damage from overflow/NaN
        for (int i = 0; i < frame_count; i++) {
            const float left = resonance::sanitize_audio_float(dst_buffer[i].left * gain);
            const float right = resonance::sanitize_audio_float(dst_buffer[i].right * gain);
            dst_buffer[i].left = std::clamp(left, -1.0f, 1.0f);
            dst_buffer[i].right = std::clamp(right, -1.0f, 1.0f);
            float v = std::max(std::abs(dst_buffer[i].left), std::abs(dst_buffer[i].right));
            if (v > peak)
                peak = v;
        }
    }

    // Click guard (Godot scheduling edge case): if the wet bus transitions from an audible block to near-silence
    // in one callback, fade the current block to 0 so the next all-zero block doesn't click.
    // Unity's DSP chain avoids this by deterministic per-tick feed/pull; in Godot the last fed block can be cut
    // by callback ordering or voice lifetime edges.
    const float silent_thr = 1.0e-4f;
    if (frames_written > 0 && prev_peak_ >= silent_thr && peak < silent_thr) {
        for (int i = 0; i < frame_count; i++) {
            const float fade = 1.0f - (float(i + 1) / float(frame_count));
            dst_buffer[i].left *= fade;
            dst_buffer[i].right *= fade;
        }
        peak = 0.0f;
        for (int i = 0; i < frame_count; i++) {
            const float v = std::max(std::abs(dst_buffer[i].left), std::abs(dst_buffer[i].right));
            if (v > peak)
                peak = v;
        }
    }
    prev_peak_ = peak;

    srv->update_reverb_effect_instrumentation(false, success, frames_written, peak);
}

// --- EFFECT RESOURCE ---

ResonanceAudioEffect::ResonanceAudioEffect() { set_name("Resonance Reverb"); } // for Godot Audio Bus editor

Ref<AudioEffectInstance> ResonanceAudioEffect::_instantiate() {
    Ref<ResonanceAudioEffectInstance> ins;
    ins.instantiate();
    ins->set_effect(Ref<ResonanceAudioEffect>(this));
    return ins;
}

void ResonanceAudioEffect::set_gain_db(float p_db) { gain_db = p_db; }
float ResonanceAudioEffect::get_gain_db() const { return gain_db; }

void ResonanceAudioEffect::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_gain_db", "p_db"), &ResonanceAudioEffect::set_gain_db);
    ClassDB::bind_method(D_METHOD("get_gain_db"), &ResonanceAudioEffect::get_gain_db);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gain_db", PROPERTY_HINT_RANGE, "-60,24,0.1,suffix:dB"), "set_gain_db", "get_gain_db");
}