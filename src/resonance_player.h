#ifndef RESONANCE_PLAYER_H
#define RESONANCE_PLAYER_H

#include <godot_cpp/classes/audio_frame.hpp>
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback.hpp>
#include <godot_cpp/classes/audio_stream_player3d.hpp>
#include <godot_cpp/classes/curve.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/templates/safe_refcount.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <phonon.h>
#include <vector>

#include <godot_cpp/variant/rid.hpp>

#include "resonance_constants.h"
#include "resonance_debug_drawer.h"
#include "resonance_directivity_drawer.h"
#include "resonance_mixer_processor.h"
#include "resonance_processor_direct.h"
#include "resonance_processor_path.h"
#include "resonance_processor_reflection.h"
#include "resonance_ring_buffer.h"
#include "resonance_server.h"

#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

class ResonancePlayer;

struct PlaybackParameters {
    int32_t source_handle = -1;              // Source handle for Steam Audio
    float occlusion = 1.0f;                  // Steam: 1=line-of-sight, 0=occluded (see direct_simulator / direct_effect)
    float transmission[3] = {1, 1, 1};       // Wall transparency
    float attenuation = 1.0f;                // Distance attenuation factor (direct, reverb, pathing)
    float reverb_pathing_attenuation = 1.0f; // Attenuation for reverb/pathing; capped to inverse-level when curve/linear (prevents overdrive)
    float distance = 0.0f;                   // Source-listener distance (meters), for reverb falloff
    bool enable_direct = true;               // Enable direct sound processing
    bool enable_reverb = true;               // Enable reverb processing
    bool has_valid_reverb = false;           // Whether the reverb parameters are valid

    Vector3 source_position = Vector3(0, 0, 0); // Source position in world space
    bool use_binaural = true;                   // Use HRTF binaural processing
    float spatial_blend = 1.0f;                 // 0=unspatialized, 1=fully spatialized (e.g. for diegetic/non-diegetic blend)
    bool use_ambisonics_encode = false;         // Encode point source to Ambisonics before binaural (for mixing scenarios)

    bool apply_air_absorption = false;            // Enable air absorption effect
    float air_absorption[3] = {1.0f, 1.0f, 1.0f}; // Air absorption coefficients (Low, Mid, High)

    bool apply_directivity = false; // Enable directivity effect
    float directivity_value = 1.0f; // Directivity factor (0.0 = Omni, 1.0 = Dipole)

    IPLCoordinateSpace3 listener_orientation{};

    // Per-source mix levels (0.0 = mute, 1.0 = full). SteamAudio-style Direct/Reflections/Pathing Mix Level.
    float direct_mix_level = 1.0f;
    float reflections_mix_level = 1.0f;
    float pathing_mix_level = 1.0f;
    bool apply_hrtf_to_reflections = true;
    bool apply_hrtf_to_pathing = true;

    // Per-source hybrid reverb overrides. Only applied when reflection_type is Hybrid. -1 = use simulation value.
    float reflections_eq[3] = {1.0f, 1.0f, 1.0f}; // EQ multipliers for parametric part (1.0 = no change)
    int reflections_delay = -1;                   // Samples before parametric starts; -1 = use simulation

    // Parametric/Hybrid split output: when true, reverb goes to reverb ring instead of main output.
    bool reverb_split_output = false;

    /// Direct effect: 0 = freq-independent transmission, 1 = freq-dependent (3-band).
    int direct_effect_transmission_type = 0;
    /// Direct effect HRTF interpolation: false = nearest, true = bilinear.
    bool direct_effect_hrtf_bilinear = false;

    /// Extra gain multiplier applied to the reflection-effect input (1.0 = pass-through, 0.0 = muted).
    /// Encodes wall damping for baked REVERB paths (where the IR assumes source co-located with listener and
    /// cannot represent source→listener occlusion). Computed on the main thread; audio thread just multiplies.
    /// Realtime reflections and STATICSOURCE/STATICLISTENER keep this at 1.0 because their IRs already encode
    /// the actual source-path attenuation.
    float wet_occlusion_factor = 1.0f;

    /// Distance attenuation applied to the reflection-effect input (matches the player's 3D distance curve).
    /// Mirrors Unity's Spatialize pipeline where the AudioSource's 3D curve multiplies the inBuffer before
    /// iplReflectionEffectApply. Without this factor, baked reverb feeds at constant gain regardless of
    /// source-listener distance. 1.0 = pass-through (opt-out / Use Global=false).
    float refl_distance_attenuation = 1.0f;
};

class ResonanceStreamPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceStreamPlayback, AudioStreamPlayback)
    friend class ResonancePlayer;

  private:
    /// AudioStreamPlayer3D volume is not applied by the engine to this GDExtension playback's _mix output; scale explicitly.
    ResonancePlayer* owner_player_ = nullptr;

    static const int kMaxBlocksPerMixCall = 4;           // Cap blocks per _mix to avoid exceeding audio callback budget (Godot frames 1024 + frame_size 512)
    int frame_size_ = resonance::kGodotDefaultFrameSize; // Steam Audio block size from ResonanceServer (256/512/1024)
    /// Direct spatializer output channels (1/2/4/6/8); Godot _mix is still stereo (fold-down here).
    int direct_out_channels_ = 2;
    Ref<AudioStreamPlayback> base_playback;

    std::atomic<bool> params_dirty = false;
    /// True once update_parameters has been invoked at least once since the last _start().
    /// Used by _mix to gate audio output: before the main thread has pushed the first spatial
    /// parameter snapshot (position/occlusion/attenuation), the default PlaybackParameters leave
    /// the direct effect fully open at listener position, which would momentarily emit every
    /// ResonancePlayer at full gain until the deferred push from ResonancePlayer::play() runs on
    /// the next main-thread frame. Phase 4's prewarm closed the lazy-init delay that used to
    /// mask this window. Matches the Unity Spatialize effect pattern (no output until the host
    /// supplies the first parameter block).
    std::atomic<bool> params_ever_synced_{false};
    PlaybackParameters params_next;
    PlaybackParameters params_current;

    bool is_initialized = false;
    int current_sample_rate = 44100;

    IPLSource local_source = nullptr;
    int32_t current_source_handle = -1;
    IPLContext context = nullptr;

    // --- MODULAR PROCESSORS ---
    ResonanceDirectProcessor direct_processor;
    ResonanceReflectionProcessor reflection_processor;
    ResonancePathProcessor path_processor;
    ResonanceMixerProcessor mixer_processor;

    // Main Buffers (Bridge between Godot & Processors)
    IPLAudioBuffer sa_in_buffer{};
    IPLAudioBuffer sa_direct_out_buffer{}; // Output from Direct Processor
    IPLAudioBuffer sa_path_out_buffer{};   // Output from Path Processor
    IPLAudioBuffer sa_final_mix_buffer{};  // Mixed result

    RingBuffer<float> input_ring_l;
    RingBuffer<float> input_ring_r;
    RingBuffer<float> output_ring_l;
    RingBuffer<float> output_ring_r;
    RingBuffer<float> output_ring_reverb_l;
    RingBuffer<float> output_ring_reverb_r;

    // Temporary linear buffer to hold exactly one Steam Audio frame (1024) for processing
    std::vector<float> temp_process_buffer_l;
    std::vector<float> temp_process_buffer_r;

    // Reusable buffers for read_reverb_frames (avoids allocation in audio path)
    std::vector<float> temp_reverb_buffer_l;
    std::vector<float> temp_reverb_buffer_r;
    // Click guard for split-reverb output: fade to zero when ring runs dry mid-callback.
    float reverb_ring_prev_l_ = 0.0f;
    float reverb_ring_prev_r_ = 0.0f;
    bool reverb_ring_prev_valid_ = false;

    // Volume Ramping State
    float prev_direct_weight = 1.0f;
    float prev_conv_reflections_mix_level_ = -1.0f; // Convolution mixer: ramp reflections_mix_level only (Unity); -1 = no ramp on first use
    /// Ramped reflections mix on mono before iplReflectionEffectApply (parametric/hybrid player path).
    float prev_parametric_reflections_mix_level_ = 1.0f;
    /// Ramped pathing mix on mono before iplPathEffectApply (matches Steam Audio Unity/FMOD spatialize).
    float prev_pathing_mix_level_ = 1.0f;

    // Throttle "no reverb params" warning: skip first 3, then log only every 200+ misses (reset after log)
    int no_reverb_warn_count = 0;

    // Input-start detection: delay processing until first non-zero sample
    // to avoid ramp artifacts when Godot sends incorrect params before playback actually starts.
    bool input_started = false;

    // Parametric pathing fallback: persistent sh coeffs when baked path fails (order 1 = 4 coeffs)
    float parametric_path_sh_coeffs[4];

    IPLReflectionEffectParams reflection_tail_params_{};
    bool reflection_tail_have_params_ = false;
    float reflection_tail_wet_gain_ = 1.0f;
    bool reflection_tail_split_output_ = false;
    /// Convolution/TAN EOS tail: after dry EOS, keep calling Apply(silence) to advance the effect like Unity's
    /// spatialize path (which has no explicit GetTail step). Reset in _start.
    bool conv_reverb_eos_silence_apply_done_ = false;

    /// Pathing EOS tail: Unity advances via iplPathEffectApply every tick; cache last params so we can Apply(silence) after EOS.
    IPLPathEffectParams pathing_tail_params_{};
    bool pathing_tail_have_params_ = false;
    /// Stable storage for tail SH coeffs (max order 3 => 16 coeffs). Avoids dangling pointers when server swaps caches.
    std::array<float, 16> pathing_tail_sh_coeffs_{};

    /// Set when the dry signal has been halted (either via Godot's _stop() or via a soft-stop
    /// request from ResonancePlayer::stop()). Marks the start of the tail-drain window: _mix
    /// continues to be called by Godot until _is_playing() returns false, which only happens
    /// once the parametric/convolution/hybrid reverb tail and the path tail have decayed
    /// or the grace block budget is exhausted.
    std::atomic<bool> stop_requested_{false};

    /// Decremented per tail block produced after the dry signal ends. -1 = grace not yet
    /// armed (still in normal play); >0 = tail blocks remaining; 0 = exhausted, force end.
    /// Hard cap to prevent pathological cases (degenerate effect handles, stalled mixer)
    /// from keeping a playback alive indefinitely. Initial budget is derived from
    /// ResonanceServer::get_max_reverb_duration() on the first transition into the tail
    /// branch (samples_read == 0).
    std::atomic<int64_t> tail_grace_blocks_remaining_{-1};

    // --- AUDIO INSTRUMENTATION (for dropout debugging) ---
    // Atomic counters updated from audio thread; read from main thread
    std::atomic<uint64_t> instrumentation_input_dropped{0};      // Samples dropped when input ring full
    std::atomic<uint64_t> instrumentation_output_underrun{0};    // Output frames filled with silence
    std::atomic<uint64_t> instrumentation_output_blocked{0};     // Processing skipped (output ring full)
    std::atomic<uint64_t> instrumentation_mix_call_count{0};     // Total _mix calls
    std::atomic<uint64_t> instrumentation_blocks_processed{0};   // Blocks processed (512 frames each)
    std::atomic<uint64_t> instrumentation_passthrough_blocks{0}; // Blocks in passthrough (no local_source)
    std::atomic<uint64_t> instrumentation_reverb_miss_blocks{0}; // Wanted reverb but fetch_reverb_params=false
    std::atomic<uint64_t> instrumentation_max_block_time_us{0};  // Max _process_steam_audio_block duration (us)
    std::atomic<uint64_t> instrumentation_late_mix_count{0};     // _mix calls with inter-callback time >15ms
    /// Last measured inter-callback gap (microseconds). Helps determine if late_mix_count is real or threshold mismatch.
    std::atomic<uint64_t> instrumentation_last_mix_gap_us_{0};
    /// Maximum measured inter-callback gap (microseconds) since last reset.
    std::atomic<uint64_t> instrumentation_max_mix_gap_us_{0};
    /// Expected callback gap from requested frames and current sample rate (microseconds).
    std::atomic<uint64_t> instrumentation_expected_mix_gap_us_{0};
    std::atomic<uint64_t> instrumentation_param_sync_count{0};                                // Times params were synced (params_dirty)
    std::atomic<uint64_t> instrumentation_zero_input_count{0};                                // _mix calls with samples_read==0 (tail drain)
    std::atomic<int32_t> instrumentation_mix_frames_min{std::numeric_limits<int32_t>::max()}; // Min samples_read per _mix (when >0)
    std::atomic<int32_t> instrumentation_mix_frames_max{0};                                   // Max samples_read per _mix
    std::atomic<uint64_t> instrumentation_silent_output_blocks{0};                            // Processed blocks with output RMS < 0.0001
    std::atomic<uint32_t> instrumentation_last_output_rms_q8{0};                              // Last block output RMS * 256 (fixed-point for display)
    /// Pathing diagnostics (last audio block, local_source): SH coeff RMS sqrt(mean(c^2)), sum(c^2), path effect stereo RMS before add to mix
    std::atomic<float> instrumentation_last_pathing_sh_rms{0.0f};
    std::atomic<float> instrumentation_last_pathing_sh_energy{0.0f};
    std::atomic<float> instrumentation_last_pathing_out_rms{0.0f};
    std::atomic<int32_t> instrumentation_last_pathing_order{-1}; // -1 = n/a this block
    std::atomic<float> debug_signal_direct{0.0f};                // Effective direct gain (for Debug Sources display)
    std::atomic<float> debug_signal_reverb{0.0f};                // Effective reverb gain (for Debug Sources display)
    std::atomic<float> debug_signal_pathing{0.0f};               // Path wet stereo RMS after path sim (per block, clamped 0..1 for HUD)
    std::chrono::steady_clock::time_point last_mix_time_;        // For inter-callback timing (audio thread only)

    void _lazy_init_steam_audio(int sampling_rate); // Lazy init to avoid overhead if not needed
    void _cleanup_steam_audio();                    // Cleanup all resources
    void _process_steam_audio_block();              // Process a single block of audio through Steam Audio
    void _sync_params();                            // Sync parameters from next to current
    void _add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix, bool split_output,
                               const IPLCoordinateSpace3& listener_coords); // Ambisonic decode must match reverb bus listener
    void _write_output_rings_folded();                                      // sa_final_mix_buffer (N ch) -> stereo rings via temp_process_buffer_*
    void _zero_sa_final_mix();                                              // memset all direct_out_channels_
    void internal_orphan_owner_player() { owner_player_ = nullptr; }

  public:
    ResonanceStreamPlayback();
    ~ResonanceStreamPlayback();

    /// Called by ResonanceServer before iplContextRelease (userdata = this).
    static void ipl_context_reinit_cleanup(void* userdata);

    ResonanceStreamPlayback(const ResonanceStreamPlayback&) = delete;
    ResonanceStreamPlayback(ResonanceStreamPlayback&&) = delete;

    void set_base_playback(const Ref<AudioStreamPlayback>& p_playback);
    void set_owner_player(ResonancePlayer* p_player) { owner_player_ = p_player; }
    void update_parameters(const PlaybackParameters& p_params);

    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override; // Mixes audio frames into the buffer
    virtual void _start(double from_pos) override;                                       // Starts playback from a specific position

    /// Phase 4: main-thread pre-allocation of Steam Audio processors and audio buffers before the first _mix call.
    /// Matches the Unity plugin's spatialize_effect::create pattern (processor allocation outside the audio callback).
    /// Safe to call multiple times (no-op after first success). Returns true on success or if already initialised.
    bool prewarm_steam_audio();
    /// True if the audio-side initialisation is complete and _mix will skip the lazy path.
    bool is_steam_audio_initialised() const { return is_initialized; }

    /// Debug Sources: last block — direct gain, reverb send, path wet output RMS (clamped 0..1). Safe from main thread.
    void get_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) const;
    /// Snapshot of instrumentation counters for debugging dropouts. Safe to call from main thread.
    void get_instrumentation_snapshot(uint64_t& out_input_dropped, uint64_t& out_output_underrun,
                                      uint64_t& out_output_blocked, uint64_t& out_mix_calls, uint64_t& out_blocks_processed,
                                      uint64_t& out_passthrough_blocks, uint64_t& out_reverb_miss_blocks, uint64_t& out_max_block_time_us,
                                      uint64_t& out_late_mix, uint64_t& out_last_mix_gap_us, uint64_t& out_max_mix_gap_us,
                                      uint64_t& out_expected_mix_gap_us, uint64_t& out_param_syncs, uint64_t& out_zero_input,
                                      int32_t& out_mix_frames_min, int32_t& out_mix_frames_max,
                                      uint64_t& out_silent_blocks, float& out_last_rms,
                                      float& out_pathing_sh_rms, float& out_pathing_sh_energy, float& out_pathing_out_rms,
                                      int32_t& out_pathing_order) const;
    /// Reset all instrumentation counters. Call from main thread to clear and re-observe.
    void reset_instrumentation();
    /// Split convolution wet ring depth (for ResonanceReverbPlayback after main stream stops).
    size_t get_reverb_ring_available_read() const { return output_ring_reverb_l.get_available_read(); }

    /// True if the per-source effects still have tail samples to emit, or if the output
    /// rings still hold queued samples. Used by _is_playing() to keep the playback alive
    /// during the reverb / pathing tail decay after the dry signal ends.
    bool has_active_tail_residue() const;

    /// Soft-stop request from main thread: halts base_playback so no further dry samples
    /// are produced, but keeps the playback object alive long enough for the reverb /
    /// pathing tail to ring out. Godot keeps invoking _mix as long as _is_playing()
    /// returns true. Called by ResonancePlayer::stop() in place of AudioStreamPlayer3D::stop().
    void request_soft_stop();

    /// Fills buffer with reverb frames (or silence if unavailable). Always returns frames. Called from reverb playback _mix.
    int32_t read_reverb_frames(AudioFrame* buffer, int32_t frames);
    virtual void _stop() override;                // Stops playback
    virtual bool _is_playing() const override;    // Checks if playback is active
    virtual int _get_loop_count() const override; // Returns the loop count for the playback
    virtual void _seek(double position) override; // Seeks to a specific position in the stream

  protected:
    static void _bind_methods() {}
};

class ResonanceStream : public AudioStream {
    GDCLASS(ResonanceStream, AudioStream)
  private:
    Ref<AudioStream> base_stream;
    ResonancePlayer* stream_owner_ = nullptr;

  public:
    void set_base_stream(const Ref<AudioStream>& p_stream);
    void set_stream_owner(ResonancePlayer* p_player) { stream_owner_ = p_player; }
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "Resonance"; }
    virtual double _get_length() const override { return base_stream.is_valid() ? base_stream->get_length() : 0.0; }
    virtual bool _is_monophonic() const override { return false; }

  protected:
    static void _bind_methods() {}
};

class ResonanceReverbPlayback : public AudioStreamPlayback {
    GDCLASS(ResonanceReverbPlayback, AudioStreamPlayback)
  private:
    ResonancePlayer* parent_player = nullptr;

  public:
    ResonanceReverbPlayback() = default;
    ResonanceReverbPlayback(const ResonanceReverbPlayback&) = delete;
    ResonanceReverbPlayback(ResonanceReverbPlayback&&) = delete;
    void set_parent_player(ResonancePlayer* p_player);
    virtual int32_t _mix(AudioFrame* buffer, float rate_scale, int32_t frames) override;
    virtual void _start(double from_pos) override;
    virtual void _stop() override;
    virtual bool _is_playing() const override;
    virtual int _get_loop_count() const override;
    virtual void _seek(double position) override;

  protected:
    static void _bind_methods();
};

class ResonanceReverbStream : public AudioStream {
    GDCLASS(ResonanceReverbStream, AudioStream)
  private:
    ResonancePlayer* parent_player = nullptr;

  public:
    ResonanceReverbStream() = default;
    ResonanceReverbStream(const ResonanceReverbStream&) = delete;
    ResonanceReverbStream(ResonanceReverbStream&&) = delete;
    void set_parent_player(ResonancePlayer* p_player);
    virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
    virtual String _get_stream_name() const override { return "ResonanceReverb"; }
    virtual double _get_length() const override { return 0.0; }
    virtual bool _is_monophonic() const override { return false; }

  protected:
    static void _bind_methods();
};

class ResonancePlayer : public AudioStreamPlayer3D {
    GDCLASS(ResonancePlayer, AudioStreamPlayer3D)
    friend class ResonanceStreamPlayback;
    friend class ResonanceReverbPlayback;

  public:
    enum AttenuationMode {
        ATTENUATION_INVERSE,        // Physics based (1/dist), Steam distance attenuation on
        ATTENUATION_LINEAR,         // Linear falloff to 0 at max_dist
        ATTENUATION_CUSTOM_CURVE,   // User defined curve
        ATTENUATION_INVERSE_NO_SIM, // Inverse-style mix: no Steam distance attenuation (full LOS gain from sim)
    };

  private:
    Ref<ResonanceStream> internal_stream;
    /// User-facing stream when player_config is set (serialization / inspector); engine stream is ResonanceStream.
    Ref<AudioStream> logical_stream_;
    /// Active ResonanceStreamPlayback voices (polyphony); audio + main thread; see internal_register_playback.
    mutable std::mutex internal_playbacks_mutex_;
    std::vector<ResonanceStreamPlayback*> internal_playbacks_;
    void internal_register_playback(ResonanceStreamPlayback* p);
    void internal_unregister_playback(ResonanceStreamPlayback* p);
    void internal_copy_internal_playbacks(std::vector<ResonanceStreamPlayback*>& out) const;
    void _broadcast_update_parameters(const PlaybackParameters& p);

    int32_t source_handle = -1;
    // Cached effective volume (linear), updated on main thread, read on audio thread.
    std::atomic<float> cached_effective_volume_linear_{1.0f};
    NodePath pathing_probe_volume; // Scene-specific: which ProbeVolume to use for pathing
    Ref<Resource> player_config;   // ResonancePlayerConfig; null = behave as plain AudioStreamPlayer3D

    // Config cache: refresh every N frames to reduce _config_* overhead while still reflecting edits
    static const int kConfigCacheRefreshInterval = 15;
    int config_cache_frame_countdown = 0;
    struct ConfigCache {
        float min_distance, max_distance, source_radius;
        int attenuation_mode;
        Ref<Curve> attenuation_curve;
        bool air_absorption_enabled;
        int air_absorption_input;
        float air_absorption_low, air_absorption_mid, air_absorption_high;
        int direct_binaural_override;
        bool directivity_enabled;
        float directivity_weight, directivity_power, spatial_blend;
        bool use_ambisonics_encode;
        int path_validation_override, find_alternate_paths_override;
        int reflections_type, reflections_enabled, pathing_enabled_override;
        int apply_hrtf_to_reflections_override, apply_hrtf_to_pathing_override;
        int occlusion_input, transmission_input, directivity_input;
        float occlusion_value, transmission_low, transmission_mid, transmission_high, directivity_value;
        int occlusion_samples, max_transmission_surfaces;
        /// 0 = use ResonanceRuntimeConfig.max_transmission_surfaces; 1 = use max_transmission_surfaces from resource. (-1 from legacy .tres is normalized to 0 in refresh.)
        int max_transmission_surfaces_override = 0;
        float direct_mix_level, reflections_mix_level, pathing_mix_level;
        float reflections_eq_low, reflections_eq_mid, reflections_eq_high;
        int reflections_delay;
        int perspective_override;
        float perspective_factor;
        /// 0 = push playback params every frame. >0 = min seconds between full occlusion/reverb/param pushes (LOD).
        float playback_parameter_min_interval;
        /// 0 = ignore. Else full push when source moved at least this many meters from last push anchor.
        float playback_parameter_min_move;
        /// Exponential smoothing time constant (s) for simulation-derived occlusion/transmission; 0 = off.
        float playback_coeff_smoothing_time;
        /// For Linear/Curve modes only: legacy [code]distance_attenuation_simulation_enabled[/code] from resource (Steam DIRECT distance flag).
        bool linear_curve_use_sim_distance_attenuation = true;
        /// When false, direct simulation skips occlusion rays (sim-defined occlusion path).
        bool simulation_occlusion_enabled = true;
        /// When false, direct simulation skips transmission through geometry.
        bool simulation_transmission_enabled = true;
        /// -1 = use ResonanceRuntimeConfig.occlusion_type; 0 = raycast; 1 = volumetric.
        int occlusion_type_override = -1;
        /// -1 = use runtime transmission_type; 0 = freq independent; 1 = freq dependent.
        int transmission_type_override = -1;
        /// -1 = use runtime hrtf_interpolation_bilinear; 0 = nearest; 1 = bilinear.
        int hrtf_interpolation_override = -1;
        /// -1 = use ResonanceServer.apply_occlusion_to_baked_reflections; 0 = Disabled; 1 = Enabled.
        int apply_occlusion_to_baked_reflections_override = -1;
        /// -1 = use ResonanceServer.apply_distance_curve_to_reflections; 0 = Disabled; 1 = Enabled.
        int apply_distance_curve_to_reflections_override = -1;
        /// 0 = use ResonanceServer.reverb_transmission_amount (global); 1 = use reverb_transmission_amount below.
        int reverb_transmission_amount_input = 0;
        /// Per-source transmission damping on reverb (only used when reverb_transmission_amount_input == 1).
        float reverb_transmission_amount = 1.0f;
    } config_cache_;
    bool config_cache_valid_ = false;

    float coeff_smooth_occ_ = 0.0f;
    float coeff_smooth_tx_[3] = {1.0f, 1.0f, 1.0f};
    bool coeff_smooth_initialized_ = false;
    int32_t coeff_smooth_source_handle_ = -1;

    double playback_lod_time_since_full_ = 0.0;
    bool playback_lod_have_anchor_ = false;
    Vector3 playback_lod_anchor_pos_;

    // Debug visualization
    ResonanceDebugDrawer debug_drawer;
    ResonanceDebugData debug_overlay_last_data_{};
    bool debug_overlay_has_last_data_ = false;
    double debug_overlay_grace_timer_ = 0.0;

    // Directivity gizmo (runtime wireframe; toggled by [member show_directivity_gizmo]).
    ResonanceDirectivityDrawer directivity_drawer_;
    bool show_directivity_gizmo_ = false;

    void _update_stream_setup();  // Ensures the internal stream is set up correctly
    void _ensure_source_exists(); // Ensures the source handle exists in the ResonanceServer
    /// Creates Steam source handle when server becomes available (children _ready before parent ResonanceRuntime).
    /// Returns true if a new handle was created. Optionally defers playback param push when already playing.
    bool _try_ensure_source_and_sync(ResonanceServer* srv, bool deferred_playback_push_if_playing);
    void _deferred_try_ensure_source_after_config();
    void _start_reverb_split_child_if_needed();
    bool warned_source_handle_create_failed_ = false;
    void _ensure_config_valid();                                 // Refreshes config cache if countdown expired or invalid
    void _ensure_config_and_apply_source(int32_t pathing_batch); // Ensures config valid, then applies update_source (blocking; clear_pathing etc.)
    /// defer_if_sim_mutex_busy: enqueue (batched) or try_update_source from _process so the main thread does not wait on RunReflections.
    void _apply_update_source(int32_t pathing_batch, bool defer_if_sim_mutex_busy = false);

    /// Computes IPLBakedDataVariation for reflections: -1 = realtime, 0 = REVERB, 1 = STATICSOURCE, 2 = STATICLISTENER.
    /// Mirrors the branch in [_apply_update_source]; shared so the audio-thread-facing playback parameters know
    /// whether baked REVERB is active (needed for wet-path occlusion damping).
    int _compute_baked_data_variation(const ResonanceServer* srv) const;

    void _setup_attenuation(ResonanceServer* srv);
    /// Config, attenuation callback, pathing batch, and simulation source update (try_update / enqueue).
    void _prepare_source_for_simulation(ResonanceServer* srv);
    /// Occlusion/reverb readback and ResonanceStreamPlayback::update_parameters (after [member _prepare_source_for_simulation]).
    void _apply_playback_params_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds);
    bool _playback_lod_should_apply_playback_params(double delta, bool debug_hud_active, const Vector3& source_pos);
    /// Pushes occlusion/attenuation from simulation into the audio thread (same as one _process tick without debug UI).
    /// When opt_debug_out is set, fills debug fields (not signal levels).
    void _push_playback_parameters_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds);
    void _deferred_push_playback_parameters();
    void _sync_player_debug_drawer(double delta, ResonanceServer* srv, const ResonanceDebugData& dbg_data, bool hud_active);
    void _compute_listener_data(Viewport* vp, Vector3& out_listener_pos, IPLCoordinateSpace3& out_listener_orient);
    void _compute_attenuation(float dist, const OcclusionData& occ_data, float& out_attenuation, float& out_reverb_pathing_attenuation);
    Vector3 _apply_perspective_correction(Vector3 listener_pos, Viewport* vp, bool apply_perspective, float perspective_factor_val);
    PlaybackParameters _build_playback_params(const Vector3& listener_pos, const IPLCoordinateSpace3& listener_orient,
                                              float attenuation, float reverb_pathing_attenuation, float dist, const Vector3& effective_source_pos,
                                              float occ_val, float tx_low, float tx_mid, float tx_high, float directivity_val, const Vector3& air_abs,
                                              bool has_reverb, bool direct_enabled, bool reverb_enabled);

    ResonanceStreamPlayback* _get_resonance_playback();
    void _aggregate_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing);
    void _update_reverb_split_child(const StringName& p_reverb_bus = StringName());
    void _nexus_deferred_spawn_anim_audio_helper();

    bool reverb_split_output_ = false;
    /// When [member player_config] is set: spawn a helper that converts TYPE_AUDIO tracks on the current scene
    /// to [method play_animation_audio_clip] once (Steam-safe path; native polyphonic cannot be wrapped).
    bool convert_animation_audio_tracks_at_runtime_ = false;
    bool exclude_from_debug_ = false;
    /// When true (default), register descendant CollisionObject3D RIDs with ResonanceServer for Custom-scene ray excludes.
    bool physics_ray_auto_exclude_collision_bodies_ = true;
    int physics_auto_exclude_resync_counter_ = 0;
    std::vector<RID> registered_physics_auto_exclude_rids_;
    void _sync_physics_ray_auto_exclude_rids();
    void _clear_physics_ray_auto_exclude_rids();

    float _config_float(const char* key, float default_val) const;
    int _config_int(const char* key, int default_val) const;
    bool _config_bool(const char* key, bool default_val) const;
    Ref<Curve> _config_curve(const char* key, const Ref<Curve>& default_val) const;
    NodePath _config_node_path(const char* key) const;
    void _refresh_config_cache();
    /// Steam Audio IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION + distance model for this source.
    static bool _steam_sim_distance_attenuation_enabled(const ConfigCache& c);

  protected:
    static void _bind_methods();
    void _notification(int p_what);

  public:
    ResonancePlayer() = default;
    ~ResonancePlayer() override;

    void _ready() override;
    void _process(double delta) override;
    void _exit_tree() override;
    /// Wraps AudioStreamPlayer3D::play: ensures internal stream, retries source handle, reverb split child, deferred sim push.
    void set_stream(const Ref<AudioStream>& p_stream);
    Ref<AudioStream> get_stream() const;

    void play(float from_position = 0.0f);
    void play_stream(double from_position = 0.0);
    /// Use from AnimationPlayer **method** tracks (or enable [member convert_animation_audio_tracks_at_runtime]).
    /// Sets [member stream] and calls [method play]. [param from_position] is usually the clip's start offset (trim).
    void play_animation_audio_clip(const Ref<AudioStream>& p_stream, float from_position = 0.0f);
    void stop();

    void set_pathing_probe_volume(const NodePath& p_path);
    NodePath get_pathing_probe_volume() const;
    float get_effective_volume_linear_cached() const { return cached_effective_volume_linear_.load(std::memory_order_relaxed); }
    /// Called by ResonanceProbeVolume when it is removed; immediately clears pathing batch so worker does not use freed data.
    void clear_pathing_probe_immediate();
    void set_player_config(const Ref<Resource>& p_config);
    Ref<Resource> get_player_config() const;
    void set_reverb_split_output(bool p_enable, const StringName& p_reverb_bus = StringName());
    bool get_reverb_split_output() const { return reverb_split_output_; }

    void set_exclude_from_debug(bool p_exclude);
    bool get_exclude_from_debug() const { return exclude_from_debug_; }

    void set_physics_ray_auto_exclude_collision_bodies(bool p_enable);
    bool get_physics_ray_auto_exclude_collision_bodies() const { return physics_ray_auto_exclude_collision_bodies_; }

    void set_convert_animation_audio_tracks_at_runtime(bool p_enable);
    bool get_convert_animation_audio_tracks_at_runtime() const { return convert_animation_audio_tracks_at_runtime_; }

    void set_show_directivity_gizmo(bool p_enable);
    bool get_show_directivity_gizmo() const { return show_directivity_gizmo_; }

    /// Build wireframe line segments for a directivity gizmo as pairs of Vector3 (line list).
    /// [param enabled] master toggle from [code]directivity_enabled[/code]; when false draws a unit sphere + forward arrow.
    /// [param input_mode] [code]0[/code] = Simulation Defined (dipole from weight/power), [code]1[/code] = User Defined (scalar; draws sphere + scaled arrow).
    /// [param weight] dipole blend (0 = monopole, 1 = dipole) matching Steam Audio [code]IPLDirectivity.dipoleWeight[/code].
    /// [param power] dipole sharpness matching Steam Audio [code]IPLDirectivity.dipolePower[/code].
    /// [param user_value] scalar 0..1 used to scale the forward arrow when [param input_mode] is User Defined.
    /// [param size] world-space radius of the gizmo in meters (typical value: [code]1.0[/code]).
    static PackedVector3Array build_directivity_gizmo_lines(bool enabled, int input_mode, float weight, float power, float user_value, float size);

    /// Called from [member player_config] [signal Resource.changed] to refresh the editor gizmo and runtime drawer.
    void _on_player_config_changed_refresh_gizmo();

    /// Returns audio instrumentation dict for dropout debugging. Keys include pathing_sh_rms, pathing_sh_energy (sum c^2),
    /// pathing_out_rms (path effect stereo RMS before add to final mix), pathing_sh_order (-1 if n/a). When player_config is set,
    /// also godot_attenuation_model, godot_pitch_scale, godot_max_distance, godot_max_db, godot_unit_size (AudioStreamPlayer3D snapshot).
    /// Empty when no player_config.
    Dictionary get_audio_instrumentation();
    /// Reset instrumentation counters on this player's playback. Call to clear and re-observe dropouts.
    void reset_audio_instrumentation();

    /// True if any active ResonanceStreamPlayback voice still has split-reverb samples queued
    /// in its output_ring_reverb. Used by ResonanceReverbPlayback::_is_playing() to keep the
    /// reverb-split child alive while the parent's tail decays.
    bool any_internal_playback_has_reverb_ring_data() const;
};
} // namespace godot

// Register Enum for Godot Inspector
VARIANT_ENUM_CAST(ResonancePlayer::AttenuationMode);

#endif