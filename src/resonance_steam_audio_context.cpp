#include "resonance_steam_audio_context.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include <climits>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
#endif

namespace godot {

namespace {

bool project_steam_audio_verbose() {
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (!ps)
        return false;
    const String key = String(resonance::kProjectSettingsResonancePrefix) + "logger/steam_audio_verbose";
    if (!ps->has_setting(key))
        return false;
    return ps->get_setting(key).booleanize();
}

void IPLCALL log_callback(IPLLogLevel level, const char* message) {
    String msg = "SteamAudio: " + String(message);
    if (level == IPL_LOGLEVEL_ERROR)
        UtilityFunctions::push_error(msg);
    else if (level == IPL_LOGLEVEL_WARNING)
        UtilityFunctions::push_warning(msg);
    else if (level == IPL_LOGLEVEL_INFO || level == IPL_LOGLEVEL_DEBUG) {
        if (project_steam_audio_verbose())
            UtilityFunctions::print(msg);
    }
}
/// Valve Steam Audio: Radeon Rays and TrueAudio Next are supported on 64-bit Windows only.
/// Avoid OpenCL device setup on Linux, macOS, Android, iOS, etc. to match that matrix and reduce driver crash risk.
/// For `iplOpenCLDeviceCreateFromExisting` / Linux GPU experiments, see wiki ResonanceRuntime-and-RuntimeConfig.md.
static bool opencl_radeon_tan_supported_host_os() {
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__))
    return true;
#else
    return false;
#endif
}

} // namespace

ResonanceSteamAudioContext::~ResonanceSteamAudioContext() {
    shutdown();
}

bool ResonanceSteamAudioContext::init(ResonanceSteamAudioContextConfig& config) {
    IPLContextSettings ctxSettings{};
    ctxSettings.version = STEAMAUDIO_VERSION;
    ctxSettings.logCallback = log_callback;
    ctxSettings.allocateCallback = nullptr;
    ctxSettings.freeCallback = nullptr;
    // Phonon: SSE2=0, SSE4=1, AVX=2, AVX2=3, AVX512=4.
    // config 0=highest (AVX512), 4=lowest (SSE2). Maps to 4 - config.
    ctxSettings.simdLevel = (config.context_simd_level >= 0 && config.context_simd_level <= 4)
                                ? static_cast<IPLSIMDLevel>(4 - config.context_simd_level)
                                : IPL_SIMDLEVEL_AVX512;
    // Respect config: validation can produce warnings (reverbTimes=0, eqCoeffs>1) from Steam Audio
    // simulation edge cases. Set context_validation=true in config when debugging API misuse.
    bool use_validation = config.context_validation;
    ctxSettings.flags = use_validation ? static_cast<IPLContextFlags>(IPL_CONTEXTFLAGS_VALIDATION) : static_cast<IPLContextFlags>(0);

    if (iplContextCreate(&ctxSettings, &context_) != IPL_STATUS_SUCCESS) {
        UtilityFunctions::push_error("Nexus Resonance: Failed to create IPL Context!");
        return false;
    }

    IPLAudioSettings audioSettings{config.sample_rate, config.frame_size};
    IPLHRTFSettings hrtfSettings{};
    if (config.hrtf_sofa_asset.is_valid() && config.hrtf_sofa_asset->is_valid()) {
        const uint8_t* ptr = config.hrtf_sofa_asset->get_data_ptr();
        int64_t sz = config.hrtf_sofa_asset->get_size();
        if (sz > INT_MAX) {
            UtilityFunctions::push_error("Nexus Resonance: SOFA HRTF data size exceeds INT_MAX; cannot load.");
            return false;
        }
        if (ptr && sz > 0) {
            hrtfSettings.type = IPL_HRTFTYPE_SOFA;
            hrtfSettings.sofaData = reinterpret_cast<const IPLuint8*>(ptr);
            hrtfSettings.sofaDataSize = static_cast<int>(sz);
            const float sofa_db = config.hrtf_sofa_asset->get_volume_db();
            hrtfSettings.volume = ResonanceSOFAAsset::db_to_gain(config.hrtf_volume_db + sofa_db);
            hrtfSettings.normType = (config.hrtf_sofa_asset->get_norm_type() == ResonanceSOFAAsset::NORM_RMS)
                                        ? IPL_HRTFNORMTYPE_RMS
                                        : IPL_HRTFNORMTYPE_NONE;
        }
    }
    if (hrtfSettings.type != IPL_HRTFTYPE_SOFA) {
        hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
        hrtfSettings.volume = ResonanceSOFAAsset::db_to_gain(config.hrtf_volume_db);
        hrtfSettings.normType = (config.hrtf_normalization_type == 1) ? IPL_HRTFNORMTYPE_RMS : IPL_HRTFNORMTYPE_NONE;
    }
    IPLHRTF new_hrtf = nullptr;
    if (iplHRTFCreate(context_, &audioSettings, &hrtfSettings, &new_hrtf) == IPL_STATUS_SUCCESS && new_hrtf) {
        hrtf_[1] = iplHRTFRetain(new_hrtf);
        iplHRTFRelease(&new_hrtf);
        new_hrtf_written_.store(true, std::memory_order_release);
    } else {
        UtilityFunctions::push_error("Nexus Resonance: HRTF Init Failed. SOFA files must use SimpleFreeFieldHRIR convention; check Steam Audio documentation.");
        return false;
    }

    // OpenCL/TAN init may crash on systems without AMD GPU (TAN) or compatible OpenCL. SEH (__try/__except)
    // is used on Windows/MSVC to catch crashes and fall back to Convolution/Default.
    const bool wants_opencl_features = (config.scene_type == 2) || (config.reflection_type == resonance::kReflectionTan);
    if (wants_opencl_features && !opencl_radeon_tan_supported_host_os()) {
        if (config.scene_type == 2) {
            config.scene_type = 0;
            UtilityFunctions::push_warning(
                "Nexus Resonance: Radeon Rays (OpenCL) is only supported on 64-bit Windows per Steam Audio. "
                "Using built-in ray tracer (Default) on this platform.");
        }
        if (config.reflection_type == resonance::kReflectionTan) {
            config.reflection_type = resonance::kReflectionConvolution;
            UtilityFunctions::push_warning(
                "Nexus Resonance: TrueAudio Next is only supported on 64-bit Windows per Steam Audio. "
                "Using Convolution on this platform.");
        }
    }

    bool needs_opencl = (config.scene_type == 2) || (config.reflection_type == resonance::kReflectionTan);
    if (needs_opencl && context_) {
#if defined(_WIN32) && defined(_MSC_VER)
        __try {
#endif
            IPLOpenCLDeviceSettings openclSettings{};
            openclSettings.type = (config.reflection_type == resonance::kReflectionTan) ? IPL_OPENCLDEVICETYPE_GPU
                                  : (config.opencl_device_type == 0)                    ? IPL_OPENCLDEVICETYPE_GPU
                                  : (config.opencl_device_type == 1)                    ? IPL_OPENCLDEVICETYPE_CPU
                                                                                        : IPL_OPENCLDEVICETYPE_ANY;
            openclSettings.numCUsToReserve = 0;
            openclSettings.fractionCUsForIRUpdate = 0.5f;
            openclSettings.requiresTAN = (config.reflection_type == resonance::kReflectionTan) ? IPL_TRUE : IPL_FALSE;
            IPLerror opencl_list_status = iplOpenCLDeviceListCreate(context_, &openclSettings, &opencl_device_list_);
            if (opencl_list_status != IPL_STATUS_SUCCESS) {
                ResonanceLog::error("ResonanceSteamAudioContext: iplOpenCLDeviceListCreate failed (status=" + String::num(opencl_list_status) + ").");
            }
            if (opencl_list_status == IPL_STATUS_SUCCESS && opencl_device_list_ && iplOpenCLDeviceListGetNumDevices(opencl_device_list_) > 0) {
                int num_devs = iplOpenCLDeviceListGetNumDevices(opencl_device_list_);
                int dev_idx = (config.opencl_device_index >= 0 && config.opencl_device_index < num_devs) ? config.opencl_device_index : 0;
                IPLerror opencl_dev_status = iplOpenCLDeviceCreate(context_, opencl_device_list_, dev_idx, &opencl_device_);
                if (opencl_dev_status != IPL_STATUS_SUCCESS) {
                    ResonanceLog::error("ResonanceSteamAudioContext: iplOpenCLDeviceCreate failed (status=" + String::num(opencl_dev_status) + ").");
                }
                if (opencl_dev_status == IPL_STATUS_SUCCESS && opencl_device_) {
                    if (config.scene_type == 2) {
                        IPLRadeonRaysDeviceSettings rrSettings{};
                        IPLerror rr_status = iplRadeonRaysDeviceCreate(opencl_device_, &rrSettings, &radeon_rays_device_);
                        if (rr_status == IPL_STATUS_SUCCESS && radeon_rays_device_) {
                            scene_type_ = IPL_SCENETYPE_RADEONRAYS;
                            UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Using Radeon Rays (GPU) for ray tracing.");
                        } else {
                            ResonanceLog::error("ResonanceSteamAudioContext: iplRadeonRaysDeviceCreate failed (status=" + String::num(rr_status) + ").");
                            iplOpenCLDeviceRelease(&opencl_device_);
                            opencl_device_ = nullptr;
                        }
                    }
                    if (config.reflection_type == resonance::kReflectionTan && opencl_device_) {
                        IPLTrueAudioNextDeviceSettings tanSettings{};
                        tanSettings.frameSize = config.frame_size;
                        tanSettings.irSize = (IPLint32)(config.max_reverb_duration * (float)config.sample_rate);
                        tanSettings.order = config.ambisonic_order;
                        tanSettings.maxSources = config.max_simulation_sources;
                        IPLerror tan_status = iplTrueAudioNextDeviceCreate(opencl_device_, &tanSettings, &tan_device_);
                        if (tan_status != IPL_STATUS_SUCCESS || !tan_device_) {
                            ResonanceLog::error("ResonanceSteamAudioContext: iplTrueAudioNextDeviceCreate failed (status=" + String::num(tan_status) + ").");
                            tan_device_ = nullptr;
                            config.reflection_type = resonance::kReflectionConvolution;
                            UtilityFunctions::push_warning("Nexus Resonance: TrueAudio Next (TAN) init failed. Falling back to Convolution. TAN requires AMD GPU with TrueAudio Next support.");
                        } else {
                            UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Using TrueAudio Next (AMD GPU) for reverb convolution.");
                        }
                    }
                }
                if (opencl_device_list_)
                    iplOpenCLDeviceListRelease(&opencl_device_list_);
                opencl_device_list_ = nullptr;
            }
            if (!opencl_device_ && opencl_device_list_) {
                iplOpenCLDeviceListRelease(&opencl_device_list_);
                opencl_device_list_ = nullptr;
            }
            if (config.reflection_type == resonance::kReflectionTan && !tan_device_) {
                if (opencl_device_) {
                    iplOpenCLDeviceRelease(&opencl_device_);
                    opencl_device_ = nullptr;
                }
            }
#if defined(_WIN32) && defined(_MSC_VER)
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            opencl_device_list_ = nullptr;
            opencl_device_ = nullptr;
            tan_device_ = nullptr;
            if (config.reflection_type == resonance::kReflectionTan) {
                config.reflection_type = resonance::kReflectionConvolution;
                UtilityFunctions::push_warning(
                    "Nexus Resonance: TrueAudio Next (TAN) crashed during init (no AMD GPU?). Falling back to Convolution.");
            }
        }
#endif
    }

    if (config.reflection_type == resonance::kReflectionTan && !tan_device_) {
        config.reflection_type = resonance::kReflectionConvolution;
        UtilityFunctions::push_warning(
            "Nexus Resonance: TrueAudio Next (TAN) is not available. Falling back to Convolution. "
            "TAN requires an AMD GPU with TrueAudio Next support.");
    }

    if (scene_type_ != IPL_SCENETYPE_RADEONRAYS) {
        if (config.scene_type == 3) {
            scene_type_ = IPL_SCENETYPE_CUSTOM;
            UtilityFunctions::print_rich(
                "[color=cyan]Nexus Resonance:[/color] Using Custom scene (Godot 3D physics raycasts; simulation on main thread).");
        } else if (config.scene_type == 0) {
            scene_type_ = IPL_SCENETYPE_DEFAULT;
            UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Using built-in ray tracer (Default).");
        } else {
            IPLEmbreeDeviceSettings embreeSettings{};
            IPLerror embree_status = iplEmbreeDeviceCreate(context_, &embreeSettings, &embree_device_);
            if (embree_status != IPL_STATUS_SUCCESS || !embree_device_) {
                embree_device_ = nullptr;
                scene_type_ = IPL_SCENETYPE_DEFAULT;
                UtilityFunctions::push_warning("Nexus Resonance: Embree unavailable, using built-in ray tracer (Default).");
            } else {
                scene_type_ = IPL_SCENETYPE_EMBREE;
                UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Using Embree (Intel) for ray tracing.");
            }
        }
    }

    // Align config.scene_type with the effective ray tracer (Embree/Radeon init may fall back to Default).
    if (scene_type_ == IPL_SCENETYPE_RADEONRAYS)
        config.scene_type = 2;
    else if (scene_type_ == IPL_SCENETYPE_CUSTOM)
        config.scene_type = 3;
    else if (scene_type_ == IPL_SCENETYPE_EMBREE)
        config.scene_type = 1;
    else
        config.scene_type = 0;

    if (project_steam_audio_verbose()) {
        const char* tracer = "built-in (Default)";
        if (config.scene_type == 1)
            tracer = "Embree";
        else if (config.scene_type == 2)
            tracer = "Radeon Rays (OpenCL)";
        else if (config.scene_type == 3)
            tracer = "Custom (Godot Physics)";
        const String verbose_msg = String("SteamAudio verbose: ") +
                                   "IPL INFO/DEBUG lines are printed only when the Steam Audio library emits them; with the built-in ray tracer that is often almost never. "
                                   "Extra Nexus geometry messages use the same setting (see Output). Effective ray tracer: " +
                                   String(tracer) + "; context_validation=" + (config.context_validation ? "on" : "off") + ".";
        UtilityFunctions::print(verbose_msg);
    }

    return true;
}

IPLHRTF ResonanceSteamAudioContext::get_hrtf() const {
    if (new_hrtf_written_.exchange(false, std::memory_order_acq_rel)) {
        if (hrtf_[0]) {
            iplHRTFRelease(&hrtf_[0]);
            hrtf_[0] = nullptr;
        }
        if (hrtf_[1]) {
            hrtf_[0] = iplHRTFRetain(hrtf_[1]);
        }
    }
    return hrtf_[0];
}

void ResonanceSteamAudioContext::shutdown() {
    if (!context_)
        return;
    new_hrtf_written_.store(false);
    if (hrtf_[0]) {
        iplHRTFRelease(&hrtf_[0]);
        hrtf_[0] = nullptr;
    }
    if (hrtf_[1]) {
        iplHRTFRelease(&hrtf_[1]);
        hrtf_[1] = nullptr;
    }
    if (radeon_rays_device_) {
        iplRadeonRaysDeviceRelease(&radeon_rays_device_);
        radeon_rays_device_ = nullptr;
    }
    if (tan_device_) {
        iplTrueAudioNextDeviceRelease(&tan_device_);
        tan_device_ = nullptr;
    }
    if (opencl_device_) {
        iplOpenCLDeviceRelease(&opencl_device_);
        opencl_device_ = nullptr;
    }
    if (opencl_device_list_) {
        iplOpenCLDeviceListRelease(&opencl_device_list_);
        opencl_device_list_ = nullptr;
    }
    if (embree_device_) {
        iplEmbreeDeviceRelease(&embree_device_);
        embree_device_ = nullptr;
    }
    if (context_) {
        iplContextRelease(&context_);
        context_ = nullptr;
    }
}

} // namespace godot
