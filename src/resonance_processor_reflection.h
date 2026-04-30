#ifndef RESONANCE_PROCESSOR_REFLECTION_H
#define RESONANCE_PROCESSOR_REFLECTION_H

#include "resonance_constants.h"
#include <phonon.h>

namespace godot {

enum class ReflectionInitFlags : int {
    NONE = 0,
    REFLECTIONEFFECT = 1 << 0,
    BUFFERS = 1 << 1,
};
inline ReflectionInitFlags operator|(ReflectionInitFlags a, ReflectionInitFlags b) {
    return static_cast<ReflectionInitFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline bool operator&(ReflectionInitFlags a, ReflectionInitFlags b) { return (static_cast<int>(a) & static_cast<int>(b)) != 0; }

class ResonanceReflectionProcessor {
  private:
    IPLContext context = nullptr;
    IPLReflectionEffect reflection_effect = nullptr;

    // Intermediate Buffer (Mono Input for Convolution)
    IPLAudioBuffer sa_mono_buffer{};
    IPLAudioBuffer sa_temp_out_buffer{}; // Required by API even if mixing

    ReflectionInitFlags init_flags = ReflectionInitFlags::NONE;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int sample_rate = 48000;
    int num_channels = 4;                                    // Ambisonic channels for convolution, 1 for parametric
    int reflection_type = resonance::kReflectionConvolution; // 0=Convolution, 1=Parametric, 2=Hybrid, 3=TAN
    /// IR length (seconds) used for iplReflectionEffectCreate and param sanitize; aligned with ResonanceServer max_reverb_duration.
    float effect_ir_duration_sec_ = resonance::kDefaultReverbDurationSec;
    /// 0 = no cap. Clamp applied IR length (convolution/hybrid/tan) to at most this and effect allocation.
    int convolution_ir_max_samples_ = 0;
    int effect_max_ir_samples_ = 0;

  public:
    ResonanceReflectionProcessor() = default;
    ~ResonanceReflectionProcessor();

    ResonanceReflectionProcessor(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor& operator=(const ResonanceReflectionProcessor&) = delete;
    ResonanceReflectionProcessor(ResonanceReflectionProcessor&&) = delete;
    ResonanceReflectionProcessor& operator=(ResonanceReflectionProcessor&&) = delete;

    /// [param p_max_reverb_duration_sec] clamped to [0.1, 10.0] like server config; must match simulation IR cap.
    /// [param p_convolution_ir_max_samples] 0 = no cap; otherwise min with allocated IR.
    void initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                    float p_max_reverb_duration_sec, int p_convolution_ir_max_samples = 0);
    void cleanup();

    // Mixes into the provided Mixer handle (unused if using direct path).
    // Unity spatialize_effect: applyVolumeRamp only on reflectionsMixLevel (mono), then source-side scaling separately.
    // prev_reflections_mix_level: use -1 to skip ramp on first block (constant reflections_mix_level); else per-sample ramp to reflections_mix_level.
    // wet_extra_gain: applied uniformly after the ramp (no inter-frame ramp on this product — avoids zipper when distance/occlusion move).
    /// Returns true if \c iplReflectionEffectApply ran (feed reached mixer). Unity passes simulation outputs with IR;
    /// skip Apply without advancing caller ramp state when IR invalid — avoids zipper/clicks.
    bool process_mix(const IPLAudioBuffer& in_buffer,
                     const IPLReflectionEffectParams& reverb_params,
                     IPLReflectionMixer mixer_handle,
                     float prev_reflections_mix_level,
                     float reflections_mix_level,
                     float wet_extra_gain);

    /// Bypass mixer: apply convolution with mixer=null, output in internal buffer.
    /// Returns pointer to sa_temp_out_buffer (ambisonic) for external decode. Valid until next process call.
    /// Ramps reflections_mix_level on downmixed mono before apply (Unity Steam Audio spatializer parity).
    /// Unity applies reflections mix on mono before Apply only; caller does not add a second distance/air gain on wet.
    /// Returns true if \c iplReflectionEffectApply ran.
    bool process_mix_direct(const IPLAudioBuffer& in_buffer, const IPLReflectionEffectParams& reverb_params,
                            float prev_reflections_mix_level, float reflections_mix_level);
    IPLAudioBuffer* get_direct_output_buffer() { return &sa_temp_out_buffer; }
    bool is_parametric() const { return reflection_type == resonance::kReflectionParametric; }

    /// Steam Audio tail after input ended (parametric/hybrid direct path; use instead of Apply until TAILCOMPLETE).
    IPLAudioEffectState tail_apply_direct(IPLReflectionEffectParams* params);
    /// Convolution/TAN: mix tail into [param mixer] (parity with iplReflectionEffectApply ... mixer). Required for reverb-bus output.
    IPLAudioEffectState tail_apply_to_mixer(IPLReflectionEffectParams* params, IPLReflectionMixer mixer);
    int get_tail_size_samples() const;
    void reset_effect();

  private:
    void sanitize_reflection_params(IPLReflectionEffectParams* params) const;
};

} // namespace godot

#endif