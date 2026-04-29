#include "resonance_constants.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <chrono>
#include <cstring>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace godot;

namespace {

void pathing_copy_sh_coeffs(std::vector<float>& dst, const float* src, int sh_count) {
    if (sh_count <= 0 || !src) {
        dst.clear();
        return;
    }
    const size_t n = static_cast<size_t>(sh_count);
    dst.resize(n);
    std::memcpy(dst.data(), src, n * sizeof(float));
}

} // namespace

OcclusionData ResonanceServer::get_source_occlusion_data(int32_t handle) {
    OcclusionData result;
    // Default "no data": treat as unoccluded. Steam uses 1 = LOS, 0 = blocked.
    result.occlusion = resonance::kOcclusionFetchDefaultVisible;
    result.transmission[0] = 1.0f;
    result.transmission[1] = 1.0f;
    result.transmission[2] = 1.0f;
    result.air_absorption[0] = 1.0f;
    result.air_absorption[1] = 1.0f;
    result.air_absorption[2] = 1.0f;
    result.directivity = 1.0f;
    result.distance_attenuation = 1.0f;
    if (handle < 0 || !_ctx())
        return result;

    // Phase 2: lock-free read from the worker-populated double-buffered cache. The worker publishes fresh
    // direct outputs from _worker_sync_fetch_caches after every RunDirect tick. No simulation_mutex needed.
    std::lock_guard<std::mutex> c_lock(occlusion_cache_mutex_);
    if (occlusion_cache_dirty_.exchange(false, std::memory_order_relaxed))
        occlusion_cache_read_.swap(occlusion_cache_write_);
    auto it = occlusion_cache_read_.find(handle);
    if (it != occlusion_cache_read_.end() && it->second.valid)
        return it->second.data;
    return result;
}

void ResonanceServer::_set_reverb_params_likely_available_hint(int32_t handle, bool likely) {
    std::lock_guard<std::mutex> lock(reverb_params_likely_available_mutex_);
    reverb_params_likely_available_[handle] = likely;
}

void ResonanceServer::_clear_reverb_params_likely_available_hints() {
    std::lock_guard<std::mutex> lock(reverb_params_likely_available_mutex_);
    reverb_params_likely_available_.clear();
}

bool ResonanceServer::peek_reverb_params_likely_available(int32_t handle) const {
    if (handle < 0)
        return false;
    std::lock_guard<std::mutex> lock(reverb_params_likely_available_mutex_);
    auto it = reverb_params_likely_available_.find(handle);
    return it != reverb_params_likely_available_.end() && it->second;
}

bool ResonanceServer::fetch_reverb_params(int32_t handle, IPLReflectionEffectParams& out_params) {
    if (handle < 0 || !_ctx())
        return false;
    // Phase 1: skip until worker has run iplSourceAdd for this handle.
    if (_is_source_attach_pending(handle))
        return false;

    // Steam Audio can return non-zero reverb times even with no probes and numRays=0 (e.g. scene-based
    // estimate or internal default). For reliable output: only treat as valid when we have a real
    // data source (probe batches for baked, or realtime rays).
    if (_uses_parametric_or_hybrid() || reflection_type == resonance::kReflectionTan) {
        if (max_rays == 0) {
            if (!probe_batch_registry_.has_any_batches())
                return false;
        }
    }

    // For Parametric/Hybrid: only report valid after RunReflections ran at least once.
    if (_uses_parametric_or_hybrid() && !reflections_have_run_once_.load(std::memory_order_acquire))
        return false;
    // Per-source: only after this source has been through at least one RunReflections.
    if (_uses_parametric_or_hybrid()) {
        std::lock_guard<std::mutex> lock(reflections_pending_mutex_);
        if (reflections_pending_handles_.count(handle) != 0)
            return false;
    }

    // Phase 2: Unity-style lock-free read. The worker populates reverb_param_cache_write_ (Parametric) /
    // reflection_param_cache_write_ (Convolution/Hybrid/TAN) from _worker_sync_fetch_caches and flips the
    // _dirty atomic. Audio/main threads only swap-on-dirty + read from the _read slot. No simulation_mutex.
    bool result = false;
    if (reflection_type == resonance::kReflectionParametric) {
        std::lock_guard<std::mutex> c_lock(reverb_cache_mutex_);
        if (reverb_cache_dirty_.exchange(false, std::memory_order_acq_rel))
            reverb_param_cache_read_.swap(reverb_param_cache_write_);
        auto it = reverb_param_cache_read_.find(handle);
        if (it != reverb_param_cache_read_.end() && it->second.valid) {
            memset(&out_params, 0, sizeof(out_params));
            out_params.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
            for (int i = 0; i < resonance::kReverbBandCount; i++) {
                out_params.reverbTimes[i] = resonance::clamp_reverb_time(it->second.reverbTimes[i]);
                out_params.eq[i] = resonance::sanitize_audio_float(it->second.eq[i]);
            }
            result = true;
            instrumentation_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
        } else {
            instrumentation_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
        }
    } else if (_uses_convolution_or_hybrid_or_tan()) {
        std::lock_guard<std::mutex> c_lock(reflection_cache_mutex_);
        if (reflection_cache_dirty_.exchange(false, std::memory_order_acq_rel))
            reflection_param_cache_read_.swap(reflection_param_cache_write_);
        auto it = reflection_param_cache_read_.find(handle);
        if (it != reflection_param_cache_read_.end() && it->second.valid) {
            out_params = it->second.params;
            if (reflection_type == resonance::kReflectionTan)
                out_params.tanDevice = _tan();
            result = true;
            instrumentation_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
        } else {
            instrumentation_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
        }
    }

    return result;
}

bool ResonanceServer::fetch_pathing_params(int32_t handle, IPLPathEffectParams& out_params) {
    if (handle < 0 || !_ctx() || !pathing_enabled) {
        instrumentation_pathing_fetch_early_exit.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // Phase 1: skip until worker has run iplSourceAdd for this handle.
    if (_is_source_attach_pending(handle)) {
        instrumentation_pathing_fetch_early_exit.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Phase 2: Unity-style lock-free read. The worker populates pathing_param_cache_write_ in
    // _worker_sync_fetch_caches after every RunPathing tick. No simulation_mutex; read from the _read slot.
    bool result = false;
    std::lock_guard<std::mutex> c_lock(pathing_cache_mutex_);
    if (pathing_cache_dirty_.exchange(false, std::memory_order_acq_rel)) {
        pathing_param_cache_read_.swap(pathing_param_cache_write_);
    }
    auto it = pathing_param_cache_read_.find(handle);
    if (it != pathing_param_cache_read_.end() && it->second.valid && !it->second.sh_coeffs.empty()) {
        // Copy sh_coeffs into the stable pathing_param_output_ buffer so out_params.shCoeffs stays valid
        // after this function returns (caller uses the pointer during iplPathEffectApply).
        CachedPathingParams& out_entry = pathing_param_output_[handle];
        out_entry.eqCoeffs[0] = it->second.eqCoeffs[0];
        out_entry.eqCoeffs[1] = it->second.eqCoeffs[1];
        out_entry.eqCoeffs[2] = it->second.eqCoeffs[2];
        out_entry.sh_coeffs = it->second.sh_coeffs;
        out_entry.order = it->second.order;
        out_entry.valid = true;
        memset(&out_params, 0, sizeof(out_params));
        for (int i = 0; i < resonance::kReverbBandCount; i++)
            out_params.eqCoeffs[i] = out_entry.eqCoeffs[i];
        out_params.shCoeffs = out_entry.sh_coeffs.data();
        out_params.order = out_entry.order;
        out_params.binaural = reverb_binaural ? IPL_TRUE : IPL_FALSE;
        out_params.hrtf = _hrtf();
        out_params.normalizeEQ = pathing_normalize_eq ? IPL_TRUE : IPL_FALSE;
        result = true;
        instrumentation_pathing_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
    } else {
        instrumentation_pathing_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void ResonanceServer::_worker_sync_fetch_caches(bool refresh_direct_outputs, bool refresh_reflection_outputs) {
    if (!_ctx() || !simulator)
        return;

    uint64_t us_occ = 0;
    uint64_t us_refl = 0;
    uint64_t us_path = 0;

    std::vector<int32_t> handles;
    source_manager.get_all_handles(handles);

    std::vector<std::pair<int32_t, CachedOcclusionData>> occ_batch;
    occ_batch.reserve(handles.size());
    std::vector<std::pair<int32_t, CachedParametricReverb>> reverb_batch;
    reverb_batch.reserve(handles.size());
    std::vector<std::pair<int32_t, CachedReflectionParams>> refl_batch;
    refl_batch.reserve(handles.size());
    std::vector<std::pair<int32_t, CachedPathingParams>> path_batch;
    path_batch.reserve(handles.size());
    std::vector<std::pair<int32_t, bool>> reverb_hint_batch;
    reverb_hint_batch.reserve(handles.size());

    std::unordered_set<int32_t> reflections_pending_snapshot;
    if (refresh_reflection_outputs && _uses_parametric_or_hybrid()) {
        std::lock_guard<std::mutex> lock(reflections_pending_mutex_);
        reflections_pending_snapshot = reflections_pending_handles_;
    }

    const bool pathing_refresh = pathing_enabled && pathing_ran_this_tick.load(std::memory_order_acquire);
    const bool reflections_have_run = reflections_have_run_once_.load(std::memory_order_acquire);

    for (int32_t handle : handles) {
        // Phase 1: new sources whose iplSourceAdd has not run yet (shouldn't happen here since the worker
        // drains pending adds at the start of the tick, but defensive against races with destroy + re-create).
        if (_is_source_attach_pending(handle))
            continue;
        IPLSource src = source_manager.get_source(handle);
        if (!src)
            continue;

        if (refresh_direct_outputs) {
            const auto t0 = std::chrono::steady_clock::now();
            IPLSimulationOutputs direct_out{};
            iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_DIRECT, &direct_out);
            CachedOcclusionData cd{};
            cd.data.occlusion = direct_out.direct.occlusion;
            cd.data.transmission[0] = direct_out.direct.transmission[0];
            cd.data.transmission[1] = direct_out.direct.transmission[1];
            cd.data.transmission[2] = direct_out.direct.transmission[2];
            cd.data.air_absorption[0] = direct_out.direct.airAbsorption[0];
            cd.data.air_absorption[1] = direct_out.direct.airAbsorption[1];
            cd.data.air_absorption[2] = direct_out.direct.airAbsorption[2];
            cd.data.directivity = direct_out.direct.directivity;
            cd.data.distance_attenuation = direct_out.direct.distanceAttenuation;
            cd.valid = true;
            occ_batch.emplace_back(handle, std::move(cd));
            const auto t1 = std::chrono::steady_clock::now();
            us_occ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }

        bool allow_reverb_fetch = true;
        if (_uses_parametric_or_hybrid() || reflection_type == resonance::kReflectionTan) {
            if (max_rays == 0 && !probe_batch_registry_.has_any_batches())
                allow_reverb_fetch = false;
        }
        if (allow_reverb_fetch && _uses_parametric_or_hybrid() && !reflections_have_run)
            allow_reverb_fetch = false;
        if (allow_reverb_fetch && _uses_parametric_or_hybrid() && reflections_pending_snapshot.count(handle) != 0)
            allow_reverb_fetch = false;

        bool source_refl_sim = true;
        {
            auto it = source_outputs_reflections_.find(handle);
            if (it != source_outputs_reflections_.end())
                source_refl_sim = it->second;
        }

        if (refresh_reflection_outputs && allow_reverb_fetch && source_refl_sim) {
            const auto t0 = std::chrono::steady_clock::now();
            IPLSimulationOutputs outputs{};
            if (_uses_parametric_or_hybrid()) {
                outputs.reflections.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
            }
            iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);
            bool has_convolution = (outputs.reflections.ir != nullptr);
            bool has_parametric = (outputs.reflections.reverbTimes[0] > 0 || outputs.reflections.reverbTimes[1] > 0 || outputs.reflections.reverbTimes[2] > 0);
            bool has_hybrid = (reflection_type == resonance::kReflectionHybrid && (has_convolution || outputs.reflections.reverbTimes[0] > 0));
            bool has_tan = (reflection_type == resonance::kReflectionTan && outputs.reflections.tanSlot >= 0 && _tan());
            bool refl_hint = false;
            if (has_convolution || (reflection_type == resonance::kReflectionParametric && has_parametric) || has_hybrid || has_tan) {
                IPLReflectionEffectParams out_params = outputs.reflections;
                for (int i = 0; i < resonance::kReverbBandCount; i++) {
                    out_params.reverbTimes[i] = resonance::clamp_reverb_time(out_params.reverbTimes[i]);
                    out_params.eq[i] = resonance::sanitize_audio_float(out_params.eq[i]);
                }
                out_params.delay = resonance::sanitize_delay_samples(out_params.delay);
                if (has_convolution && out_params.ir != nullptr) {
                    const int max_ir_samples = 480000;
                    const int max_ir_channels = 64;
                    if (out_params.irSize <= 0 || out_params.irSize > max_ir_samples ||
                        out_params.numChannels <= 0 || out_params.numChannels > max_ir_channels) {
                        out_params.ir = nullptr;
                        has_convolution = false;
                        has_hybrid = (reflection_type == resonance::kReflectionHybrid && outputs.reflections.reverbTimes[0] > 0);
                    }
                }
                bool use_hybrid_type = (reflection_type == resonance::kReflectionHybrid && has_convolution && has_parametric);
                out_params.type = (reflection_type == resonance::kReflectionParametric) ? IPL_REFLECTIONEFFECTTYPE_PARAMETRIC : (reflection_type == resonance::kReflectionHybrid && use_hybrid_type) ? IPL_REFLECTIONEFFECTTYPE_HYBRID
                                                                                                                            : (reflection_type == resonance::kReflectionHybrid)                      ? IPL_REFLECTIONEFFECTTYPE_PARAMETRIC
                                                                                                                            : (reflection_type == resonance::kReflectionTan)                         ? IPL_REFLECTIONEFFECTTYPE_TAN
                                                                                                                                                                                                     : IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
                if (reflection_type == resonance::kReflectionTan)
                    out_params.tanDevice = _tan();
                if (reflection_type == resonance::kReflectionParametric && has_parametric) {
                    CachedParametricReverb cr{};
                    for (int i = 0; i < resonance::kReverbBandCount; i++) {
                        cr.reverbTimes[i] = resonance::clamp_reverb_time(outputs.reflections.reverbTimes[i]);
                        cr.eq[i] = outputs.reflections.eq[i];
                    }
                    cr.valid = true;
                    reverb_batch.emplace_back(handle, std::move(cr));
                }
                if (_uses_convolution_or_hybrid_or_tan() && (has_convolution || has_hybrid || has_tan)) {
                    CachedReflectionParams rp{};
                    rp.params = out_params;
                    rp.valid = true;
                    refl_batch.emplace_back(handle, std::move(rp));
                }
                refl_hint = true;
            }
            reverb_hint_batch.emplace_back(handle, refl_hint);
            const auto t1 = std::chrono::steady_clock::now();
            us_refl += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }

        if (pathing_refresh) {
            const auto t0 = std::chrono::steady_clock::now();
            IPLSimulationOutputs pout{};
            iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_PATHING, &pout);
            if (pout.pathing.shCoeffs != nullptr) {
                int order = pout.pathing.order;
                int sh_count = (order >= 0) ? (order + 1) * (order + 1) : 0;
                if (sh_count > 0) {
                    CachedPathingParams pm{};
                    pm.eqCoeffs[0] = pout.pathing.eqCoeffs[0];
                    pm.eqCoeffs[1] = pout.pathing.eqCoeffs[1];
                    pm.eqCoeffs[2] = pout.pathing.eqCoeffs[2];
                    pathing_copy_sh_coeffs(pm.sh_coeffs, pout.pathing.shCoeffs, sh_count);
                    pm.order = order;
                    pm.valid = true;
                    path_batch.emplace_back(handle, std::move(pm));
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            us_path += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }

        iplSourceRelease(&src);
    }

    if (!reverb_hint_batch.empty()) {
        std::lock_guard<std::mutex> h_lock(reverb_params_likely_available_mutex_);
        for (const auto& kv : reverb_hint_batch)
            reverb_params_likely_available_[kv.first] = kv.second;
    }

    if (!occ_batch.empty()) {
        std::lock_guard<std::mutex> c_lock(occlusion_cache_mutex_);
        for (auto& kv : occ_batch) {
            occlusion_cache_write_[kv.first] = std::move(kv.second);
        }
        occlusion_cache_dirty_.store(true);
    }

    if (!reverb_batch.empty()) {
        std::lock_guard<std::mutex> c_lock(reverb_cache_mutex_);
        for (auto& kv : reverb_batch) {
            reverb_param_cache_write_[kv.first] = std::move(kv.second);
        }
        reverb_cache_dirty_.store(true);
    }

    if (!refl_batch.empty()) {
        std::lock_guard<std::mutex> c_lock(reflection_cache_mutex_);
        for (auto& kv : refl_batch) {
            reflection_param_cache_write_[kv.first] = std::move(kv.second);
        }
        reflection_cache_dirty_.store(true);
    }

    if (!path_batch.empty()) {
        std::lock_guard<std::mutex> c_lock(pathing_cache_mutex_);
        for (auto& kv : path_batch) {
            pathing_param_output_[kv.first] = kv.second;
            pathing_param_cache_write_[kv.first] = kv.second;
        }
        pathing_cache_dirty_.store(true);
    }

    instrumentation_worker_us_sync_fetch_occlusion.store(us_occ, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_reflections.store(us_refl, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_pathing.store(us_path, std::memory_order_relaxed);
}

void ResonanceServer::set_pathing_deviation_callback(IPLDeviationCallback callback, void* userData) {
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
    if (callback) {
        _pathing_deviation_model.type = IPL_DEVIATIONTYPE_CALLBACK;
        _pathing_deviation_model.callback = callback;
        _pathing_deviation_model.userData = userData;
        _pathing_deviation_callback_enabled = true;
    } else {
        _pathing_deviation_model.type = IPL_DEVIATIONTYPE_DEFAULT;
        _pathing_deviation_model.callback = nullptr;
        _pathing_deviation_model.userData = nullptr;
        _pathing_deviation_callback_enabled = false;
    }
}

void ResonanceServer::clear_pathing_deviation_callback() {
    set_pathing_deviation_callback(nullptr, nullptr);
}
