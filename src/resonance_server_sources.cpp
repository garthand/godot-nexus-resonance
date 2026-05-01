#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <cstdint>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <vector>

using namespace godot;

int32_t ResonanceServer::create_source_handle(Vector3 pos, float radius) {
    if (!_ctx() || !simulator)
        return -1;
    IPLSourceSettings settings{};
    settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (pathing_enabled)
        settings.flags = static_cast<IPLSimulationFlags>(settings.flags | IPL_SIMULATIONFLAGS_PATHING);
    IPLSource src = nullptr;
    if (iplSourceCreate(simulator, &settings, &src) != IPL_STATUS_SUCCESS || !src) {
        ResonanceLog::error("ResonanceServer: iplSourceCreate failed (create_source_handle).");
        return -1;
    }
    // Phase 1: no blocking simulation_mutex on main thread. SourceManager retains src; after this
    // block we own one retain to be released once iplSourceAdd has actually run on the worker.
    const int32_t handle = source_manager.add_source(src);
    if (handle < 0) {
        iplSourceRelease(&src);
        return -1;
    }
    if (handle < kMaxCacheHandles) {
        // Clear caches for recycled handle IDs.
        for (int slot = 0; slot < kCacheSlots; slot++) {
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
        }
        reflections_pending_[static_cast<size_t>(handle)].store(true, std::memory_order_release);
        // Default per-handle flags until first _update_source_internal runs on the worker.
        source_outputs_reflections_[static_cast<size_t>(handle)].store(1, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(pathing_enabled ? 1 : 0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        pending_attach_handles_.insert(handle);
    }
    {
        PendingSourceAdd pa{};
        pa.handle = handle;
        pa.initial.position = pos;
        pa.initial.radius = radius;
        pa.initial.source_forward = Vector3(0, 0, -1);
        pa.initial.source_up = Vector3(0, 1, 0);
        pa.initial.directivity_weight = 0.0f;
        pa.initial.directivity_power = 1.0f;
        pa.initial.air_absorption_enabled = true;
        pa.initial.use_sim_distance_attenuation = false;
        pa.initial.min_distance = 1.0f;
        pa.initial.path_validation_enabled = false;
        pa.initial.find_alternate_paths = false;
        pa.initial.occlusion_samples = resonance::kDefaultOcclusionSamples;
        pa.initial.num_transmission_rays = max_transmission_surfaces;
        pa.initial.baked_data_variation = 0;
        pa.initial.baked_endpoint_center = Vector3(0, 0, 0);
        pa.initial.baked_endpoint_radius = 0.0f;
        pa.initial.pathing_probe_batch_handle = -1;
        pa.initial.reflections_enabled_override = -1;
        pa.initial.pathing_enabled_override = -1;
        pa.initial.occlusion_type_override = -1;
        pa.initial.simulation_occlusion_enabled = true;
        pa.initial.simulation_transmission_enabled = true;
        pa.initial.direct_mix_level = 1.0f;
        pa.initial.reflections_mix_level = 1.0f;
        pa.initial.pathing_mix_level = 1.0f;
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        pending_source_adds_.push_back(pa);
    }
    iplSourceRelease(&src);
    // Kick the worker so the attach happens on the very next tick rather than the next simulation window.
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
    }
    worker_cv.notify_one();
    return handle;
}

bool ResonanceServer::_is_source_attach_pending(int32_t handle) const {
    if (handle < 0)
        return false;
    std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
    return pending_attach_handles_.count(handle) != 0;
}

void ResonanceServer::ensure_fmod_reverb_source() {
    if (fmod_reverb_source_handle_ >= 0)
        return;
    fmod_reverb_source_handle_ = create_source_handle(Vector3(0, 0, 0), 1.0f);
}

void ResonanceServer::_destroy_source_handle_under_simulation_lock(int32_t handle) {
    IPLSource src = source_manager.get_source(handle);
    if (src) {
        if (simulator) {
            iplSourceRemove(src, simulator);
            // Required by Steam Audio API: staging list updates apply only after commit (same as iplSourceAdd).
            iplSimulatorCommit(simulator);
        }
        iplSourceRelease(&src);
    }
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        _source_attenuation_entries.erase(handle);
    }
    _source_update_snapshot_.erase(handle);
    realtime_reflection_log_once_handles_.erase(handle);
    if (handle >= 0 && handle < kMaxCacheHandles) {
        source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
    }
    source_manager.remove_source(handle);
}

void ResonanceServer::destroy_source_handle(int32_t handle) {
    if (handle < 0 || is_shutting_down_flag.load(std::memory_order_acquire) || !_ctx())
        return;
    // Phase 1: do not block on simulation_mutex. Remove from source_manager immediately so the audio thread
    // stops seeing the handle; hand the retained IPLSource to the worker for iplSourceRemove + commit + release.
    IPLSource src = source_manager.get_source(handle); // retains
    source_manager.remove_source(handle);              // releases the map retain
    {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        pending_attach_handles_.erase(handle);
    }
    if (src) {
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        pending_source_removes_.push_back(src);
        pending_source_post_remove_cleanup_.push_back(handle);
    }
    if (handle < kMaxCacheHandles) {
        for (int slot = 0; slot < kCacheSlots; slot++) {
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
        }
        reflections_pending_[static_cast<size_t>(handle)].store(false, std::memory_order_release);
        source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> h_lock(reverb_params_likely_available_mutex_);
        reverb_params_likely_available_.erase(handle);
    }
    // Cache invalidation now handled by the lock-free cache arrays above.
    {
        std::lock_guard<std::mutex> b(source_update_batch_mutex_);
        source_update_batch_.erase(handle);
    }
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
    }
    worker_cv.notify_one();
}

void ResonanceServer::update_source(int32_t handle, Vector3 pos, float radius,
                                    Vector3 source_forward, Vector3 source_up,
                                    float directivity_weight, float directivity_power, bool air_absorption_enabled,
                                    bool use_sim_distance_attenuation, float min_distance,
                                    bool path_validation_enabled, bool find_alternate_paths,
                                    int occlusion_samples, int num_transmission_rays,
                                    int baked_data_variation, Vector3 baked_endpoint_center, float baked_endpoint_radius,
                                    int32_t pathing_probe_batch_handle,
                                    int reflections_enabled_override,
                                    int pathing_enabled_override,
                                    int occlusion_type_override,
                                    bool simulation_occlusion_enabled,
                                    bool simulation_transmission_enabled,
                                    float direct_mix_level,
                                    float reflections_mix_level,
                                    float pathing_mix_level) {
    // Phase 1: no blocking simulation_mutex on main thread. Forwards to enqueue_source_update; the
    // pending batch is drained by [method flush_pending_source_updates] (called by ResonanceRuntime
    // once per frame) and by the worker's lifecycle drain when ticks fire.
    enqueue_source_update(handle, pos, radius, source_forward, source_up,
                          directivity_weight, directivity_power, air_absorption_enabled,
                          use_sim_distance_attenuation, min_distance,
                          path_validation_enabled, find_alternate_paths,
                          occlusion_samples, num_transmission_rays,
                          baked_data_variation, baked_endpoint_center, baked_endpoint_radius,
                          pathing_probe_batch_handle, reflections_enabled_override, pathing_enabled_override,
                          occlusion_type_override, simulation_occlusion_enabled, simulation_transmission_enabled,
                          direct_mix_level, reflections_mix_level, pathing_mix_level);
}

bool ResonanceServer::try_update_source(int32_t handle, Vector3 pos, float radius,
                                        Vector3 source_forward, Vector3 source_up,
                                        float directivity_weight, float directivity_power, bool air_absorption_enabled,
                                        bool use_sim_distance_attenuation, float min_distance,
                                        bool path_validation_enabled, bool find_alternate_paths,
                                        int occlusion_samples, int num_transmission_rays,
                                        int baked_data_variation, Vector3 baked_endpoint_center, float baked_endpoint_radius,
                                        int32_t pathing_probe_batch_handle,
                                        int reflections_enabled_override,
                                        int pathing_enabled_override,
                                        int occlusion_type_override,
                                        bool simulation_occlusion_enabled,
                                        bool simulation_transmission_enabled,
                                        float direct_mix_level,
                                        float reflections_mix_level,
                                        float pathing_mix_level) {
    if (handle < 0)
        return false;
    std::unique_lock<std::mutex> lock(simulation_mutex, std::defer_lock);
    if (!lock.try_lock())
        return false;
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return false;
    _update_source_internal(src, handle, pos, radius, source_forward, source_up,
                            directivity_weight, directivity_power, air_absorption_enabled,
                            use_sim_distance_attenuation, min_distance,
                            path_validation_enabled, find_alternate_paths,
                            occlusion_samples, num_transmission_rays,
                            baked_data_variation, baked_endpoint_center, baked_endpoint_radius,
                            pathing_probe_batch_handle, reflections_enabled_override, pathing_enabled_override,
                            occlusion_type_override, simulation_occlusion_enabled, simulation_transmission_enabled,
                            direct_mix_level, reflections_mix_level, pathing_mix_level);
    iplSourceRelease(&src);
    return true;
}

void ResonanceServer::enqueue_source_update(int32_t handle, Vector3 pos, float radius,
                                            Vector3 source_forward, Vector3 source_up,
                                            float directivity_weight, float directivity_power, bool air_absorption_enabled,
                                            bool use_sim_distance_attenuation, float min_distance,
                                            bool path_validation_enabled, bool find_alternate_paths,
                                            int occlusion_samples, int num_transmission_rays,
                                            int baked_data_variation, Vector3 baked_endpoint_center, float baked_endpoint_radius,
                                            int32_t pathing_probe_batch_handle,
                                            int reflections_enabled_override,
                                            int pathing_enabled_override,
                                            int occlusion_type_override,
                                            bool simulation_occlusion_enabled,
                                            bool simulation_transmission_enabled,
                                            float direct_mix_level,
                                            float reflections_mix_level,
                                            float pathing_mix_level) {
    if (handle < 0)
        return;
    PendingSourceUpdate u{};
    u.position = pos;
    u.radius = radius;
    u.source_forward = source_forward;
    u.source_up = source_up;
    u.directivity_weight = directivity_weight;
    u.directivity_power = directivity_power;
    u.air_absorption_enabled = air_absorption_enabled;
    u.use_sim_distance_attenuation = use_sim_distance_attenuation;
    u.min_distance = min_distance;
    u.path_validation_enabled = path_validation_enabled;
    u.find_alternate_paths = find_alternate_paths;
    u.occlusion_samples = occlusion_samples;
    u.num_transmission_rays = num_transmission_rays;
    u.baked_data_variation = baked_data_variation;
    u.baked_endpoint_center = baked_endpoint_center;
    u.baked_endpoint_radius = baked_endpoint_radius;
    u.pathing_probe_batch_handle = pathing_probe_batch_handle;
    u.reflections_enabled_override = reflections_enabled_override;
    u.pathing_enabled_override = pathing_enabled_override;
    u.occlusion_type_override = occlusion_type_override;
    u.simulation_occlusion_enabled = simulation_occlusion_enabled;
    u.simulation_transmission_enabled = simulation_transmission_enabled;
    u.direct_mix_level = direct_mix_level;
    u.reflections_mix_level = reflections_mix_level;
    u.pathing_mix_level = pathing_mix_level;
    std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
    source_update_batch_[handle] = u;
}

void ResonanceServer::flush_pending_source_updates() {
    std::vector<std::pair<int32_t, PendingSourceUpdate>> batch;
    {
        std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
        if (source_update_batch_.empty())
            return;
        batch.reserve(source_update_batch_.size());
        for (const auto& kv : source_update_batch_) {
            batch.push_back(kv);
        }
        source_update_batch_.clear();
    }
    std::unique_lock<std::mutex> sim_lock(simulation_mutex, std::defer_lock);
    if (!sim_lock.try_lock()) {
        std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
        for (const auto& kv : batch) {
            if (source_update_batch_.find(kv.first) == source_update_batch_.end()) {
                source_update_batch_.emplace(kv.first, kv.second);
            }
        }
        return;
    }
    for (const auto& kv : batch) {
        const int32_t handle = kv.first;
        const PendingSourceUpdate& u = kv.second;
        IPLSource src = source_manager.get_source(handle);
        if (!src)
            continue;
        _update_source_internal(src, handle, u.position, u.radius, u.source_forward, u.source_up,
                                u.directivity_weight, u.directivity_power, u.air_absorption_enabled,
                                u.use_sim_distance_attenuation, u.min_distance,
                                u.path_validation_enabled, u.find_alternate_paths,
                                u.occlusion_samples, u.num_transmission_rays,
                                u.baked_data_variation, u.baked_endpoint_center, u.baked_endpoint_radius,
                                u.pathing_probe_batch_handle, u.reflections_enabled_override, u.pathing_enabled_override,
                                u.occlusion_type_override, u.simulation_occlusion_enabled, u.simulation_transmission_enabled,
                                u.direct_mix_level, u.reflections_mix_level, u.pathing_mix_level);
        iplSourceRelease(&src);
    }
}

void ResonanceServer::set_source_attenuation_callback_data(int32_t handle, int attenuation_mode, float min_distance, float max_distance, const PackedFloat32Array& curve_samples) {
    if (handle < 0)
        return;
    if (!source_manager.has_handle(handle))
        return;
    std::lock_guard<std::recursive_mutex> lock(_attenuation_callback_mutex);
    auto& entry_ptr = _source_attenuation_entries[handle];
    if (!entry_ptr)
        entry_ptr = std::make_unique<AttenuationEntry>();
    if (!entry_ptr->data)
        entry_ptr->data = std::make_unique<AttenuationCallbackData>();
    AttenuationCallbackData& d = *entry_ptr->data;
    d.mode = attenuation_mode;
    d.min_distance = min_distance;
    d.max_distance = max_distance;
    const int64_t curve_sz = curve_samples.size();
    d.num_curve_samples = static_cast<int>((!curve_samples.is_empty() && curve_sz <= resonance::kAttenuationCurveSamples) ? curve_sz
                                                                                                                          : static_cast<int64_t>(resonance::kAttenuationCurveSamples));
    for (int i = 0; i < d.num_curve_samples && i < curve_samples.size(); i++) {
        d.curve_samples[i] = curve_samples[i];
    }
}

void ResonanceServer::clear_source_attenuation_callback_data(int32_t handle) {
    if (handle < 0 || !_ctx())
        return;
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
    _source_attenuation_entries.erase(handle);
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return;
    auto snap_it = _source_update_snapshot_.find(handle);
    if (snap_it == _source_update_snapshot_.end() || !snap_it->second.valid) {
        iplSourceRelease(&src);
        return;
    }
    const SourceUpdateSnapshot& p = snap_it->second;
    _update_source_internal(src, handle, p.position, p.radius, p.source_forward, p.source_up, p.directivity_weight, p.directivity_power,
                            p.air_absorption_enabled, p.use_sim_distance_attenuation, p.min_distance, p.path_validation_enabled, p.find_alternate_paths,
                            p.occlusion_samples, p.num_transmission_rays, p.baked_data_variation, p.baked_endpoint_center, p.baked_endpoint_radius,
                            p.pathing_probe_batch_handle, p.reflections_enabled_override, p.pathing_enabled_override,
                            p.occlusion_type_override, p.simulation_occlusion_enabled, p.simulation_transmission_enabled,
                            p.direct_mix_level, p.reflections_mix_level, p.pathing_mix_level);
    iplSourceRelease(&src);
}

static float IPLCALL distance_attenuation_callback(IPLfloat32 distance, void* userData) {
    const ResonanceServer::AttenuationCallbackContext* ctx = static_cast<const ResonanceServer::AttenuationCallbackContext*>(userData);
    if (!ctx || !ctx->mutex || !ctx->data)
        return 1.0f;
    std::lock_guard<std::recursive_mutex> lock(*ctx->mutex);
    const ResonanceServer::AttenuationCallbackData* d = ctx->data;
    if (!d || d->max_distance <= d->min_distance)
        return 1.0f;
    if (distance <= d->min_distance)
        return (d->mode == 2 && d->num_curve_samples > 0) ? resonance::sanitize_audio_float(d->curve_samples[0]) : 1.0f;
    if (distance >= d->max_distance)
        return (d->mode == 2 && d->num_curve_samples > 0) ? resonance::sanitize_audio_float(d->curve_samples[d->num_curve_samples - 1]) : 0.0f;
    float t = (distance - d->min_distance) / (d->max_distance - d->min_distance);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f
                                       : t;
    if (d->mode == 1)
        return 1.0f - t;
    if (d->mode == 2 && d->num_curve_samples > 1) {
        float idx = t * static_cast<float>(d->num_curve_samples - 1);
        int i0 = (int)idx;
        int i1 = (i0 + 1 < d->num_curve_samples) ? i0 + 1 : i0;
        float frac = idx - (float)i0;
        float v = d->curve_samples[i0] * (1.0f - frac) + d->curve_samples[i1] * frac;
        return resonance::sanitize_audio_float(v);
    }
    return resonance::sanitize_audio_float(1.0f - t);
}

void ResonanceServer::_update_source_internal(IPLSource src, int32_t handle, Vector3 pos, float radius,
                                              Vector3 source_forward, Vector3 source_up,
                                              float directivity_weight, float directivity_power, bool air_absorption_enabled,
                                              bool use_sim_distance_attenuation, float min_distance,
                                              bool path_validation_enabled, bool find_alternate_paths,
                                              int occlusion_samples, int num_transmission_rays,
                                              int baked_data_variation, Vector3 baked_endpoint_center, float baked_endpoint_radius,
                                              int32_t pathing_probe_batch_handle,
                                              int reflections_enabled_override,
                                              int pathing_enabled_override,
                                              int occlusion_type_override,
                                              bool simulation_occlusion_enabled,
                                              bool simulation_transmission_enabled,
                                              float direct_mix_level,
                                              float reflections_mix_level,
                                              float pathing_mix_level) {
    if (!src || !_ctx())
        return;
    SourceUpdateSnapshot& snap = _source_update_snapshot_[handle];
    snap.position = pos;
    snap.radius = radius;
    snap.source_forward = source_forward;
    snap.source_up = source_up;
    snap.directivity_weight = directivity_weight;
    snap.directivity_power = directivity_power;
    snap.air_absorption_enabled = air_absorption_enabled;
    snap.use_sim_distance_attenuation = use_sim_distance_attenuation;
    snap.min_distance = min_distance;
    snap.path_validation_enabled = path_validation_enabled;
    snap.find_alternate_paths = find_alternate_paths;
    snap.occlusion_samples = occlusion_samples;
    snap.num_transmission_rays = num_transmission_rays;
    snap.baked_data_variation = baked_data_variation;
    snap.baked_endpoint_center = baked_endpoint_center;
    snap.baked_endpoint_radius = baked_endpoint_radius;
    snap.pathing_probe_batch_handle = pathing_probe_batch_handle;
    snap.reflections_enabled_override = reflections_enabled_override;
    snap.pathing_enabled_override = pathing_enabled_override;
    snap.occlusion_type_override = occlusion_type_override;
    snap.simulation_occlusion_enabled = simulation_occlusion_enabled;
    snap.simulation_transmission_enabled = simulation_transmission_enabled;
    snap.direct_mix_level = direct_mix_level;
    snap.reflections_mix_level = reflections_mix_level;
    snap.pathing_mix_level = pathing_mix_level;
    snap.valid = true;
    IPLSimulationInputs inputs{};
    const float dm = resonance::sanitize_audio_float(direct_mix_level);
    const float rm = resonance::sanitize_audio_float(reflections_mix_level);
    const float pm = resonance::sanitize_audio_float(pathing_mix_level);
    const bool any_mix = (dm > 0.0f) || (rm > 0.0f) || (pm > 0.0f);
    bool enable_reflections = (reflections_enabled_override == -1) ? true : (reflections_enabled_override != 0);
    enable_reflections = enable_reflections && (rm > 0.0f);
    if (enable_reflections && baked_data_variation == -1 && realtime_reflection_max_distance_m > 0.0f) {
        Vector3 lip = ResonanceUtils::to_godot_vector3(listener_coords_[0].origin);
        if (pos.distance_to(lip) > static_cast<real_t>(realtime_reflection_max_distance_m))
            enable_reflections = false;
    }
    bool enable_pathing = (pathing_enabled_override == -1) ? pathing_enabled : (pathing_enabled_override != 0);
    enable_pathing = enable_pathing && (pm > 0.0f);
    IPLSimulationFlags sim_flags = static_cast<IPLSimulationFlags>(0);
    if (any_mix) {
        sim_flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT);
        if (enable_reflections)
            sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    }
    IPLDirectSimulationFlags dflags = (IPLDirectSimulationFlags)0;
    if (simulation_occlusion_enabled)
        dflags = (IPLDirectSimulationFlags)(dflags | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
    if (simulation_transmission_enabled)
        dflags = (IPLDirectSimulationFlags)(dflags | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    inputs.directFlags = dflags;
    if (use_sim_distance_attenuation)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
    if (air_absorption_enabled)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
    if (directivity_weight != 0.0f || directivity_power != 1.0f)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY);
    inputs.source.origin = ResonanceUtils::to_ipl_vector3(pos);
    Vector3 ahead_n = ResonanceUtils::safe_unit_vector(source_forward, Vector3(0, 0, -1));
    Vector3 up_raw = ResonanceUtils::safe_unit_vector(source_up, Vector3(0, 1, 0));
    Vector3 right_n = ResonanceUtils::safe_unit_vector(ahead_n.cross(up_raw), Vector3(1, 0, 0));
    Vector3 up_n = ResonanceUtils::safe_unit_vector(right_n.cross(ahead_n), Vector3(0, 1, 0));
    inputs.source.ahead = ResonanceUtils::to_ipl_vector3(ahead_n);
    inputs.source.up = ResonanceUtils::to_ipl_vector3(up_n);
    inputs.source.right = ResonanceUtils::to_ipl_vector3(right_n);
    inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    inputs.directivity.dipoleWeight = directivity_weight;
    inputs.directivity.dipolePower = directivity_power;
    inputs.directivity.callback = nullptr;
    inputs.directivity.userData = nullptr;
    bool use_callback = false;
    AttenuationCallbackContext* callback_ctx = nullptr;
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        auto it = _source_attenuation_entries.find(handle);
        if (it != _source_attenuation_entries.end() && it->second && it->second->data) {
            AttenuationCallbackData* pdata = it->second->data.get();
            if (pdata->mode == 1 || pdata->mode == 2) {
                use_callback = true;
                AttenuationEntry& entry = *it->second;
                entry.ctx.mutex = &_attenuation_callback_mutex;
                entry.ctx.data = pdata;
                callback_ctx = &entry.ctx;
            }
        }
    }
    if (use_sim_distance_attenuation) {
        if (use_callback && callback_ctx) {
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
            inputs.distanceAttenuationModel.minDistance = callback_ctx->data->min_distance;
            inputs.distanceAttenuationModel.callback = distance_attenuation_callback;
            inputs.distanceAttenuationModel.userData = callback_ctx;
            inputs.distanceAttenuationModel.dirty = IPL_FALSE;
        } else {
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
            inputs.distanceAttenuationModel.minDistance = min_distance;
            inputs.distanceAttenuationModel.callback = nullptr;
            inputs.distanceAttenuationModel.userData = nullptr;
        }
    } else {
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        inputs.distanceAttenuationModel.callback = nullptr;
        inputs.distanceAttenuationModel.userData = nullptr;
    }
    int eff_occlusion_type = occlusion_type;
    if (occlusion_type_override == 0 || occlusion_type_override == 1)
        eff_occlusion_type = occlusion_type_override;
    inputs.occlusionType = (eff_occlusion_type == 0) ? IPL_OCCLUSIONTYPE_RAYCAST : IPL_OCCLUSIONTYPE_VOLUMETRIC;
    inputs.occlusionRadius = radius;
    inputs.numOcclusionSamples = CLAMP(occlusion_samples, 1, simulation_settings.maxNumOcclusionSamples);
    inputs.numTransmissionRays = CLAMP(num_transmission_rays, 1, resonance::kMaxTransmissionRays);

    // Reflections: baked_data_variation -1=Realtime, 0=REVERB, 1=STATICSOURCE, 2=STATICLISTENER
    if (baked_data_variation == -1) {
        if (realtime_reflection_log_once_handles_.insert(handle).second) {
            String src_msg = "Source " + String::num_int64(handle) + " first realtime reflections update (baked=FALSE). Rays: " + String::num_int64(max_rays);
            UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] " + src_msg);
        }
        inputs.baked = IPL_FALSE;
        inputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
        inputs.bakedDataIdentifier.endpointInfluence.center = {0.0f, 0.0f, 0.0f};
        inputs.bakedDataIdentifier.endpointInfluence.radius = 0.0f;
    } else {
        inputs.baked = IPL_TRUE;
        inputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        if (baked_data_variation == 1) {
            inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_STATICSOURCE;
            inputs.bakedDataIdentifier.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(baked_endpoint_center);
            inputs.bakedDataIdentifier.endpointInfluence.radius = (baked_endpoint_radius > 0.0f) ? baked_endpoint_radius : reverb_influence_radius;
        } else if (baked_data_variation == 2) {
            inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_STATICLISTENER;
            inputs.bakedDataIdentifier.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(baked_endpoint_center);
            inputs.bakedDataIdentifier.endpointInfluence.radius = (baked_endpoint_radius > 0.0f) ? baked_endpoint_radius : reverb_influence_radius;
        } else {
            inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
            inputs.bakedDataIdentifier.endpointInfluence.center = {0.0f, 0.0f, 0.0f};
            inputs.bakedDataIdentifier.endpointInfluence.radius = 0.0f;
        }
    }

    inputs.reverbScale[0] = 1.0f;
    inputs.reverbScale[1] = 1.0f;
    inputs.reverbScale[2] = 1.0f;
    inputs.hybridReverbTransitionTime = hybrid_reverb_transition_time;
    inputs.hybridReverbOverlapPercent = hybrid_reverb_overlap_percent;

    // get_pathing_batch_for_source Retains the batch. Steam Audio keeps using pathingProbes until
    // iplSimulatorRunPathing completes; releasing immediately after SetInputs can AV inside RunPathing.
    // Queue releases; worker drains after RunPathing (simulation_mutex covers both sides).
    IPLProbeBatch pathing_batch_retained = nullptr;
    if (enable_pathing && pathing_enabled) {
        IPLProbeBatch path_batch = _get_pathing_batch_for_source(pathing_probe_batch_handle);
        if (path_batch) {
            sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_PATHING);
            inputs.pathingProbes = path_batch;
            // Pathing spatial order follows ambisonic_order (same as realtime reflection simulation maxOrder / shared order).
            inputs.pathingOrder = ambisonic_order;
            inputs.visRadius = pathing_vis_radius;
            inputs.visThreshold = pathing_vis_threshold;
            inputs.visRange = pathing_vis_range;
            bool eff_validation = path_validation_enabled;
            bool eff_alternate = find_alternate_paths;
            if (!eff_validation)
                eff_alternate = false;

            inputs.enableValidation = eff_validation ? IPL_TRUE : IPL_FALSE;
            inputs.findAlternatePaths = eff_alternate ? IPL_TRUE : IPL_FALSE;
            {
                std::lock_guard<std::mutex> d_lock(_pathing_deviation_mutex);
                inputs.deviationModel = (_pathing_deviation_callback_enabled && _pathing_deviation_model.callback) ? &_pathing_deviation_model : nullptr;
            }
            pathing_batch_retained = path_batch;
        }
    }
    inputs.flags = sim_flags;

    if (handle >= 0 && handle < kMaxCacheHandles) {
        source_outputs_reflections_[static_cast<size_t>(handle)].store(enable_reflections ? 1 : 0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store((enable_reflections && baked_data_variation == -1) ? 1 : 0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(((sim_flags & IPL_SIMULATIONFLAGS_PATHING) != 0) ? 1 : 0, std::memory_order_release);
    }

    iplSourceSetInputs(src, sim_flags, &inputs);

    if (pathing_batch_retained)
        pathing_probe_batches_pending_release_.push_back(pathing_batch_retained);
}

void ResonanceServer::_drain_pending_source_lifecycle_assume_locked() {
    if (!_ctx() || !simulator)
        return;

    std::vector<PendingSourceAdd> local_adds;
    std::vector<IPLSource> local_removes;
    std::vector<int32_t> local_post_remove;
    {
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        local_adds.swap(pending_source_adds_);
        local_removes.swap(pending_source_removes_);
        local_post_remove.swap(pending_source_post_remove_cleanup_);
    }
    if (local_adds.empty() && local_removes.empty())
        return;

    for (const PendingSourceAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle); // retains; may be null if already destroyed
        if (!src)
            continue;
        iplSourceAdd(src, simulator);
        iplSourceRelease(&src);
    }
    for (IPLSource src : local_removes) {
        if (!src)
            continue;
        iplSourceRemove(src, simulator);
    }
    // One batched commit covers every add and remove that happened since the last worker tick.
    iplSimulatorCommit(simulator);
    // Now that the removed sources are no longer referenced by the simulator staging lists, drop the final retain.
    for (IPLSource src : local_removes) {
        if (src) {
            IPLSource tmp = src;
            iplSourceRelease(&tmp);
        }
    }
    // Mark attached handles ready for fetch/cache paths.
    if (!local_adds.empty()) {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        for (const PendingSourceAdd& pa : local_adds)
            pending_attach_handles_.erase(pa.handle);
    }
    // Post-remove housekeeping (map entries keyed by handle that are worker-owned).
    for (int32_t handle : local_post_remove) {
        {
            std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
            _source_attenuation_entries.erase(handle);
        }
        _source_update_snapshot_.erase(handle);
        realtime_reflection_log_once_handles_.erase(handle);
        if (handle >= 0 && handle < kMaxCacheHandles) {
            source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
            source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
            source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        }
    }
    // Apply initial inputs now that iplSourceAdd + Commit have run.
    for (const PendingSourceAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle);
        if (!src)
            continue;
        const PendingSourceUpdate& u = pa.initial;
        _update_source_internal(src, pa.handle, u.position, u.radius, u.source_forward, u.source_up,
                                u.directivity_weight, u.directivity_power, u.air_absorption_enabled,
                                u.use_sim_distance_attenuation, u.min_distance,
                                u.path_validation_enabled, u.find_alternate_paths,
                                u.occlusion_samples, u.num_transmission_rays,
                                u.baked_data_variation, u.baked_endpoint_center, u.baked_endpoint_radius,
                                u.pathing_probe_batch_handle, u.reflections_enabled_override, u.pathing_enabled_override,
                                u.occlusion_type_override, u.simulation_occlusion_enabled, u.simulation_transmission_enabled,
                                u.direct_mix_level, u.reflections_mix_level, u.pathing_mix_level);
        iplSourceRelease(&src);
    }
}

void ResonanceServer::_drain_pathing_probe_batch_releases() {
    for (IPLProbeBatch& b : pathing_probe_batches_pending_release_) {
        if (b)
            iplProbeBatchRelease(&b);
    }
    pathing_probe_batches_pending_release_.clear();
}

IPLSource ResonanceServer::get_source_from_handle(int32_t handle) {
    return source_manager.get_source(handle);
}

// --- CALCULATIONS ---

float ResonanceServer::calculate_distance_attenuation(Vector3 source_pos, Vector3 listener_pos, float min_dist, float max_dist) {
    if (!_ctx())
        return 1.0f;
    IPLDistanceAttenuationModel model{};
    model.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
    model.minDistance = min_dist;
    IPLVector3 src = ResonanceUtils::to_ipl_vector3(source_pos);
    IPLVector3 dst = ResonanceUtils::to_ipl_vector3(listener_pos);
    return iplDistanceAttenuationCalculate(_ctx(), src, dst, &model);
}

Vector3 ResonanceServer::calculate_air_absorption(Vector3 source_pos, Vector3 listener_pos) {
    if (!_ctx())
        return Vector3(1, 1, 1);
    IPLAirAbsorptionModel model{};
    model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    IPLVector3 src = ResonanceUtils::to_ipl_vector3(source_pos);
    IPLVector3 dst = ResonanceUtils::to_ipl_vector3(listener_pos);
    float air_abs[3] = {1.0f, 1.0f, 1.0f};
    iplAirAbsorptionCalculate(_ctx(), src, dst, &model, air_abs);
    return Vector3(air_abs[0], air_abs[1], air_abs[2]);
}

float ResonanceServer::calculate_directivity(Vector3 source_pos, Vector3 fwd, Vector3 up, Vector3 right, Vector3 listener_pos, float weight, float power) {
    if (!_ctx())
        return 1.0f;
    IPLDirectivity dSettings{};
    dSettings.dipoleWeight = weight;
    dSettings.dipolePower = power;
    IPLCoordinateSpace3 source_space{};
    source_space.origin = ResonanceUtils::to_ipl_vector3(source_pos);
    source_space.ahead = ResonanceUtils::to_ipl_vector3(fwd);
    source_space.up = ResonanceUtils::to_ipl_vector3(up);
    source_space.right = ResonanceUtils::to_ipl_vector3(right);
    IPLVector3 lst = ResonanceUtils::to_ipl_vector3(listener_pos);
    return iplDirectivityCalculate(_ctx(), source_space, lst, &dSettings);
}
