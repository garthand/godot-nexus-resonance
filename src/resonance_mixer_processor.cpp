#include "resonance_mixer_processor.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include <chrono>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

// Warn once per process for frame-size mismatches.
static bool s_frame_count_small_warned = false;
static bool s_frame_count_large_warned = false;

static void _sanitize_audio_buffer(IPLAudioBuffer* buf) {
    if (!buf || !buf->data)
        return;
    for (int ch = 0; ch < buf->numChannels; ch++) {
        if (!buf->data[ch])
            continue;
        for (int i = 0; i < buf->numSamples; i++) {
            buf->data[ch][i] = resonance::sanitize_audio_float(buf->data[ch][i]);
        }
    }
}

ResonanceMixerProcessor::~ResonanceMixerProcessor() { cleanup(); }

bool ResonanceMixerProcessor::_can_decode() const {
    return (init_flags & MixerInitFlags::BUFFERS) &&
           ((init_flags & MixerInitFlags::DECODEEFFECT) || ((init_flags & MixerInitFlags::DECODEEFFECT_7_1) && (init_flags & MixerInitFlags::VIRTUALSURROUND)));
}

void ResonanceMixerProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order) {
    if (init_flags != MixerInitFlags::NONE)
        return;

    context = p_context;
    frame_size = p_frame_size;
    ambisonic_order = p_ambisonic_order;

    IPLAudioSettings audioSettings{p_sample_rate, p_frame_size};
    int num_channels = (ambisonic_order + 1) * (ambisonic_order + 1);
    if (iplAudioBufferAllocate(context, num_channels, frame_size, &sa_ambisonic_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, 2, frame_size, &sa_stereo_buffer) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceMixerProcessor: Buffer allocation failed.");
        cleanup();
        return;
    }
    init_flags = init_flags | MixerInitFlags::BUFFERS;

    ResonanceServer* srv = ResonanceServer::get_singleton();
    bool use_vs = srv && srv->use_virtual_surround_output();
    IPLHRTF hrtf_handle = srv ? srv->get_hrtf_handle() : nullptr;

    // 1. Create Decoder Effect (stereo for standard path)
    IPLAmbisonicsDecodeEffectSettings decSettings{};
    decSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
    decSettings.speakerLayout.numSpeakers = 2;
    decSettings.maxOrder = ambisonic_order;
    decSettings.hrtf = use_vs ? nullptr : (srv && srv->use_reverb_binaural() ? hrtf_handle : nullptr);

    if (iplAmbisonicsDecodeEffectCreate(context, &audioSettings, &decSettings, &decode_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceMixerProcessor: iplAmbisonicsDecodeEffectCreate failed.");
        cleanup();
        return;
    }
    init_flags = init_flags | MixerInitFlags::DECODEEFFECT;

    // 2. Virtual Surround path: decode to 7.1, then VirtualSurround -> stereo
    if (use_vs && hrtf_handle) {
        IPLAmbisonicsDecodeEffectSettings dec7Settings{};
        dec7Settings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1;
        dec7Settings.speakerLayout.numSpeakers = 8;
        dec7Settings.maxOrder = ambisonic_order;
        dec7Settings.hrtf = nullptr;

        if (iplAmbisonicsDecodeEffectCreate(context, &audioSettings, &dec7Settings, &decode_effect_7_1) == IPL_STATUS_SUCCESS &&
            iplAudioBufferAllocate(context, 8, frame_size, &sa_7_1_buffer) == IPL_STATUS_SUCCESS) {
            init_flags = init_flags | MixerInitFlags::DECODEEFFECT_7_1;
        } else {
            ResonanceLog::error("ResonanceMixerProcessor: 7.1 decode/buffer allocation failed.");
        }

        IPLVirtualSurroundEffectSettings vsSettings{};
        vsSettings.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_SURROUND_7_1;
        vsSettings.speakerLayout.numSpeakers = 8;
        vsSettings.hrtf = hrtf_handle;

        if (iplVirtualSurroundEffectCreate(context, &audioSettings, &vsSettings, &virtual_surround_effect) == IPL_STATUS_SUCCESS) {
            init_flags = init_flags | MixerInitFlags::VIRTUALSURROUND;
        } else {
            ResonanceLog::error("ResonanceMixerProcessor: iplVirtualSurroundEffectCreate failed.");
            if (decode_effect_7_1) {
                iplAmbisonicsDecodeEffectRelease(&decode_effect_7_1);
                decode_effect_7_1 = nullptr;
                init_flags = static_cast<MixerInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(MixerInitFlags::DECODEEFFECT_7_1));
            }
            if (sa_7_1_buffer.data)
                iplAudioBufferFree(context, &sa_7_1_buffer);
            sa_7_1_buffer.data = nullptr;
        }
    }
}

void ResonanceMixerProcessor::cleanup() {
    if (decode_effect)
        iplAmbisonicsDecodeEffectRelease(&decode_effect);
    if (decode_effect_7_1)
        iplAmbisonicsDecodeEffectRelease(&decode_effect_7_1);
    if (virtual_surround_effect)
        iplVirtualSurroundEffectRelease(&virtual_surround_effect);
    decode_effect = nullptr;
    decode_effect_7_1 = nullptr;
    virtual_surround_effect = nullptr;
    if (context) {
        if (sa_ambisonic_buffer.data) {
            iplAudioBufferFree(context, &sa_ambisonic_buffer);
            sa_ambisonic_buffer.data = nullptr;
        }
        if (sa_stereo_buffer.data) {
            iplAudioBufferFree(context, &sa_stereo_buffer);
            sa_stereo_buffer.data = nullptr;
        }
        if (sa_7_1_buffer.data) {
            iplAudioBufferFree(context, &sa_7_1_buffer);
            sa_7_1_buffer.data = nullptr;
        }
    }
    init_flags = MixerInitFlags::NONE;
    context = nullptr;
    pending_stereo_left.clear();
    pending_stereo_right.clear();
    pending_read_index = 0;
}

void ResonanceMixerProcessor::_write_stereo_to_audio_frames_with_carry(AudioFrame* out_frames, int frame_count) {
    if (!out_frames || frame_count <= 0 || !sa_stereo_buffer.data || !sa_stereo_buffer.data[0] || !sa_stereo_buffer.data[1])
        return;

    // Append the newest decoded block to our carry queue.
    const size_t append_base = pending_stereo_left.size();
    pending_stereo_left.resize(append_base + static_cast<size_t>(frame_size));
    pending_stereo_right.resize(append_base + static_cast<size_t>(frame_size));
    for (int i = 0; i < frame_size; i++) {
        pending_stereo_left[append_base + static_cast<size_t>(i)] = sa_stereo_buffer.data[0][i];
        pending_stereo_right[append_base + static_cast<size_t>(i)] = sa_stereo_buffer.data[1][i];
    }

    const size_t pending_count = pending_stereo_left.size();
    int written = 0;
    if (pending_read_index < pending_count && pending_stereo_right.size() == pending_count) {
        const int to_copy = std::min(frame_count, static_cast<int>(pending_count - pending_read_index));
        for (int i = 0; i < to_copy; i++) {
            const size_t idx = pending_read_index + static_cast<size_t>(i);
            out_frames[i].left += pending_stereo_left[idx];
            out_frames[i].right += pending_stereo_right[idx];
        }
        pending_read_index += static_cast<size_t>(to_copy);
        written = to_copy;
    }

    if (pending_read_index >= pending_stereo_left.size()) {
        pending_stereo_left.clear();
        pending_stereo_right.clear();
        pending_read_index = 0;
    }

    // If caller requested more than what has been decoded so far, leave zeros for the rest.
    if (written < frame_count && !s_frame_count_large_warned) {
        s_frame_count_large_warned = true;
        UtilityFunctions::push_warning(
            "Nexus Resonance: Reverb output frame_count (" + String::num_int64(frame_count) +
            ") > audio_frame_size (" + String::num_int64(frame_size) +
            "). Zero-padding missing samples until audio engine reinitializes to a matching frame size.");
    }
}

void ResonanceMixerProcessor::_decode_ambisonic_to_stereo_buffer(IPLAudioBuffer* ambi_in, const IPLCoordinateSpace3& listener_coords) {
    if (!ambi_in || !ambi_in->data)
        return;
    IPLAmbisonicsDecodeEffectParams decParams{};
    decParams.order = ambisonic_order;
    decParams.orientation = listener_coords;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    bool use_vs = srv && srv->use_virtual_surround_output() && decode_effect_7_1 && virtual_surround_effect;

    if (use_vs) {
        decParams.hrtf = nullptr;
        decParams.binaural = IPL_FALSE;
        iplAmbisonicsDecodeEffectApply(decode_effect_7_1, &decParams, ambi_in, &sa_7_1_buffer);
        IPLVirtualSurroundEffectParams vsParams{};
        vsParams.hrtf = srv->get_hrtf_handle();
        iplVirtualSurroundEffectApply(virtual_surround_effect, &vsParams, &sa_7_1_buffer, &sa_stereo_buffer);
    } else {
        decParams.hrtf = (srv && srv->use_reverb_binaural()) ? srv->get_hrtf_handle() : nullptr;
        decParams.binaural = (srv && srv->use_reverb_binaural()) ? IPL_TRUE : IPL_FALSE;
        iplAmbisonicsDecodeEffectApply(decode_effect, &decParams, ambi_in, &sa_stereo_buffer);
    }
}

bool ResonanceMixerProcessor::process_mixer_return(IPLReflectionMixer mixer_handle, const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count) {
    if (!_can_decode() || !mixer_handle)
        return false;

    // 1. Apply Mixer: Extracts the accumulated audio from the mixer into our local Ambisonic buffer.
    // The mixer apply step consumes accumulated audio; params are required by API but not used for convolution.
    // Use PARAMETRIC type so validation does not require ir/irSize (avoids Steam Audio validation warnings).
    IPLReflectionEffectParams params{};
    params.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    params.numChannels = sa_ambisonic_buffer.numChannels;
    params.reverbTimes[0] = params.reverbTimes[1] = params.reverbTimes[2] = resonance::kMixerParametricDummyReverbTime;

    iplReflectionMixerApply(mixer_handle, &params, &sa_ambisonic_buffer);
    {
        const auto t0 = std::chrono::steady_clock::now();
        _sanitize_audio_buffer(&sa_ambisonic_buffer);
        const auto t1 = std::chrono::steady_clock::now();
        if (ResonanceServer* srv = ResonanceServer::get_singleton())
            srv->record_mixer_sanitize_ambi_usec(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    }

    _decode_ambisonic_to_stereo_buffer(&sa_ambisonic_buffer, listener_coords);
    {
        const auto t0 = std::chrono::steady_clock::now();
        _sanitize_audio_buffer(&sa_stereo_buffer);
        const auto t1 = std::chrono::steady_clock::now();
        if (ResonanceServer* srv = ResonanceServer::get_singleton())
            srv->record_mixer_sanitize_stereo_usec(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    }

    if (frame_count < frame_size && !s_frame_count_small_warned) {
        s_frame_count_small_warned = true;
        UtilityFunctions::push_warning(
            "Nexus Resonance: Reverb output frame_count (" + String::num_int64(frame_count) +
            ") < audio_frame_size (" + String::num_int64(frame_size) +
            "). Carrying tail samples across callbacks until audio engine reinitializes to a matching frame size.");
    }
    _write_stereo_to_audio_frames_with_carry(out_frames, frame_count);
    return true;
}

bool ResonanceMixerProcessor::decode_ambisonic_to_stereo(IPLAudioBuffer* ambi_buf,
                                                         const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count) {
    if (!_can_decode() || !ambi_buf || !ambi_buf->data)
        return false;

    _decode_ambisonic_to_stereo_buffer(ambi_buf, listener_coords);
    {
        const auto t0 = std::chrono::steady_clock::now();
        _sanitize_audio_buffer(&sa_stereo_buffer);
        const auto t1 = std::chrono::steady_clock::now();
        if (ResonanceServer* srv = ResonanceServer::get_singleton())
            srv->record_mixer_sanitize_stereo_usec(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
    }

    _write_stereo_to_audio_frames_with_carry(out_frames, frame_count);
    return true;
}
} // namespace godot