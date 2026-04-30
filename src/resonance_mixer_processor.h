#ifndef RESONANCE_MIXER_PROCESSOR_H
#define RESONANCE_MIXER_PROCESSOR_H

#include "resonance_constants.h"
#include <godot_cpp/classes/audio_frame.hpp>
#include <phonon.h>
#include <vector>

namespace godot {

enum class MixerInitFlags : int {
    NONE = 0,
    BUFFERS = 1 << 0,
    DECODEEFFECT = 1 << 1,
    DECODEEFFECT_7_1 = 1 << 2,
    VIRTUALSURROUND = 1 << 3,
};
inline MixerInitFlags operator|(MixerInitFlags a, MixerInitFlags b) {
    return static_cast<MixerInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline MixerInitFlags& operator|=(MixerInitFlags& a, MixerInitFlags b) {
    a = a | b;
    return a;
}
inline bool operator&(MixerInitFlags a, MixerInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

class ResonanceMixerProcessor {
  private:
    IPLContext context = nullptr;
    IPLAmbisonicsDecodeEffect decode_effect = nullptr;
    // Virtual Surround path: decode to 7.1 -> iplVirtualSurroundEffect -> stereo
    IPLAmbisonicsDecodeEffect decode_effect_7_1 = nullptr;
    IPLVirtualSurroundEffect virtual_surround_effect = nullptr;

    // Buffers
    IPLAudioBuffer sa_ambisonic_buffer{}; // Mixer -> Decode
    IPLAudioBuffer sa_ambisonic_prev{};   // Previous mixer block (1-block delay to avoid Godot feed/pull ordering clicks)
    bool ambisonic_prev_valid = false;
    IPLAudioBuffer sa_stereo_buffer{}; // Decode -> Godot (or VirtualSurround -> Godot)
    IPLAudioBuffer sa_7_1_buffer{};    // Decode 7.1 intermediate when use_virtual_surround
    std::vector<float> last_stereo_left{};
    std::vector<float> last_stereo_right{};
    bool last_stereo_valid = false;
    uint64_t last_seen_mixer_feed_count_ = 0;
    bool have_seen_mixer_feed_count_ = false;
    std::vector<float> pending_stereo_left{};
    std::vector<float> pending_stereo_right{};
    size_t pending_read_index = 0;

    void _write_stereo_to_audio_frames_with_carry(AudioFrame* out_frames, int frame_count);
    void _decode_ambisonic_to_stereo_buffer(IPLAudioBuffer* ambi_in, const IPLCoordinateSpace3& listener_coords);
    void _cache_last_stereo_block();
    bool _restore_last_stereo_block();
    bool _can_decode() const;

    MixerInitFlags init_flags = MixerInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int ambisonic_order = 1;

  public:
    ResonanceMixerProcessor() = default;
    ~ResonanceMixerProcessor();

    ResonanceMixerProcessor(const ResonanceMixerProcessor&) = delete;
    ResonanceMixerProcessor& operator=(const ResonanceMixerProcessor&) = delete;
    ResonanceMixerProcessor(ResonanceMixerProcessor&&) = delete;
    ResonanceMixerProcessor& operator=(ResonanceMixerProcessor&&) = delete;

    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order);
    void cleanup();

    /// True if initialization succeeded and decode path is usable (for AudioEffect to avoid processing with invalid state).
    bool is_ready() const { return _can_decode(); }

    // Pulls audio from the provided mixer handle, decodes it using listener orientation, and writes to output.
    bool process_mixer_return(IPLReflectionMixer mixer_handle, const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count);

    // Decode external ambisonic buffer to stereo (bypasses mixer - for direct convolution output)
    bool decode_ambisonic_to_stereo(IPLAudioBuffer* ambi_buf, const IPLCoordinateSpace3& listener_coords, AudioFrame* out_frames, int frame_count);
};

} // namespace godot

#endif