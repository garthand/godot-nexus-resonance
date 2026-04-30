#include "resonance_math.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <climits>
#include <cstring>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

IPLCoordinateSpace3 ResonanceServer::get_current_listener_coords() {
    if (new_listener_written_.exchange(false, std::memory_order_acq_rel)) {
        listener_coords_[0] = listener_coords_[1];
    }
    return listener_coords_[0];
}

IPLReflectionMixer ResonanceServer::get_reflection_mixer_handle() const {
    std::lock_guard<std::mutex> lock(mixer_access_mutex);
    if (new_reflection_mixer_written_.exchange(false, std::memory_order_acq_rel)) {
        if (reflection_mixer_[0]) {
            iplReflectionMixerRelease(&reflection_mixer_[0]);
            reflection_mixer_[0] = nullptr;
        }
        if (reflection_mixer_[1]) {
            reflection_mixer_[0] = reflection_mixer_[1];
            reflection_mixer_[1] = nullptr;
        }
    }
    return reflection_mixer_[0];
}

void ResonanceServer::fill_reflection_mixer_apply_params(IPLReflectionEffectParams* p) const {
    if (!p)
        return;
    std::memset(p, 0, sizeof(IPLReflectionEffectParams));
    p->numChannels = get_num_channels_for_order();
    if (reflection_type == resonance::kReflectionTan) {
        p->type = IPL_REFLECTIONEFFECTTYPE_TAN;
        p->tanDevice = _tan();
        p->tanSlot = 0;
    } else {
        p->type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        p->irSize = static_cast<IPLint32>(resonance::reverb_ir_size_samples(get_sample_rate(), max_reverb_duration));
    }
}

static int snap_to_supported_frame_size(int value) {
    const int supported[] = {256, resonance::kGodotDefaultFrameSize, 1024, resonance::kMaxAudioFrameSize};
    int best = resonance::kGodotDefaultFrameSize;
    int best_dist = INT_MAX;
    for (int s : supported) {
        int d = (value > s) ? (value - s) : (s - value);
        if (d < best_dist) {
            best_dist = d;
            best = s;
        }
    }
    return best;
}
void ResonanceServer::request_reinit_with_frame_size(int detected_frame_count) {
    if (detected_frame_count <= 0)
        return;
    if (!audio_frame_size_was_auto_.load(std::memory_order_acquire))
        return; // User set explicit value; do not override
    int snapped = snap_to_supported_frame_size(detected_frame_count);
    if (snapped == frame_size)
        return; // Already at nearest supported; avoid redundant reinit
    int prev = pending_reinit_frame_size_.exchange(snapped, std::memory_order_release);
    (void)prev; // Ignore overwrites; main thread consumes once
}

int ResonanceServer::consume_pending_reinit_frame_size() {
    return pending_reinit_frame_size_.exchange(0, std::memory_order_acq_rel);
}

void ResonanceServer::set_listener_valid(bool valid) {
    pending_listener_valid.store(valid);
}

void ResonanceServer::notify_listener_changed() {
    // API compatibility with Steam Audio. ResonanceRuntime updates listener every frame from camera.
    // Use when listener is created/swapped and you drive it manually via update_listener.
}

void ResonanceServer::notify_listener_changed_to(Node* listener_node) {
    if (!listener_node || !_ctx())
        return;
    Node3D* n3d = Object::cast_to<Node3D>(listener_node);
    if (!n3d)
        return;
    Transform3D tr = n3d->get_global_transform();
    Vector3 pos = tr.origin;
    Vector3 forward = -tr.basis.get_column(2);
    Vector3 up = tr.basis.get_column(1);
    update_listener(pos, forward, up);
}

void ResonanceServer::update_listener(Vector3 pos, Vector3 dir, Vector3 up) {
    if (!_ctx())
        return;

    // Orthonormalize basis for safety; use safe_unit_vector to avoid NaN from degenerate transforms
    Vector3 dir_n = ResonanceUtils::safe_unit_vector(dir, Vector3(0, 0, -1));
    Vector3 up_raw = ResonanceUtils::safe_unit_vector(up, Vector3(0, 1, 0));
    Vector3 right_n = ResonanceUtils::safe_unit_vector(dir_n.cross(up_raw), Vector3(1, 0, 0));
    Vector3 up_n = ResonanceUtils::safe_unit_vector(right_n.cross(dir_n), Vector3(0, 1, 0));

    IPLCoordinateSpace3 listener;
    listener.origin = ResonanceUtils::to_ipl_vector3(pos);
    listener.ahead = ResonanceUtils::to_ipl_vector3(dir_n);
    listener.up = ResonanceUtils::to_ipl_vector3(up_n);
    listener.right = ResonanceUtils::to_ipl_vector3(right_n);

    listener_coords_[1] = listener;
    new_listener_written_.store(true, std::memory_order_release);

    // FMOD Bridge: keep reverb IPLSource in sync with listener. Use try_update_source so the main thread
    // never blocks on simulation_mutex while the worker holds it during RunReflections/RunPathing.
    if (fmod_reverb_source_handle_ >= 0) {
        try_update_source(fmod_reverb_source_handle_, pos, 1.0f);
    }
}
