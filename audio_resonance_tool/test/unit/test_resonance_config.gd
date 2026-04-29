extends GutTest

## Unit tests for ResonanceRuntimeConfig (config resource).


func test_create_default_has_required_keys():
	var rt = ResonanceRuntimeConfig.create_default()
	var config = rt.get_config()
	var required = [
		"sample_rate",
		"ambisonic_order",
		"max_reverb_duration",
		"reverb_influence_radius",
		"direct_speaker_channels",
		"max_transmission_surfaces"
	]
	for key in required:
		assert_has(config, key, "config missing: " + key)


func test_create_default_scene_type_is_builtin():
	var rt = ResonanceRuntimeConfig.create_default()
	assert_eq(
		rt.scene_type,
		0,
		"default scene_type should be Default (0); matches Godot omitting 0 in .tres"
	)
	var config = rt.get_config()
	assert_eq(config.get("scene_type", -1), 0, "get_config should expose scene_type 0")
	assert_eq(
		config.get("physics_ray_batch_size", -1),
		16,
		"get_config should include physics_ray_batch_size (default 16)"
	)


func test_physics_ray_batch_size_in_get_config():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.physics_ray_batch_size = 32
	rt.scene_type = 3
	var config = rt.get_config()
	assert_eq(config.get("physics_ray_batch_size", -1), 32, "physics_ray_batch_size passthrough")


func test_get_config_applies_properties():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.ambisonic_order = 2
	rt.pathing_enabled = true
	rt.scene_type = 2
	var config = rt.get_config()
	assert_eq(config.get("ambisonic_order", -1), 2, "ambisonic_order not applied")
	assert_true(config.get("pathing_enabled", false), "pathing_enabled not applied")
	assert_eq(config.get("scene_type", -1), 2, "scene_type not applied")


func test_get_config_sample_rate_valid():
	var rt = ResonanceRuntimeConfig.create_default()
	var config = rt.get_config()
	assert_eq(typeof(config.get("sample_rate", 0)), TYPE_INT, "sample_rate should be int")
	assert_gt(config.get("sample_rate", 0), 0, "sample_rate should be > 0")


func test_sample_rate_override_when_zero_uses_mix_rate():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.sample_rate_override = 0
	var config = rt.get_config()
	var mix_rate := int(AudioServer.get_mix_rate())
	assert_eq(
		config.get("sample_rate", -1), mix_rate, "sample_rate_override=0 should use Godot mix rate"
	)


func test_sample_rate_override_when_nonzero_uses_override():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.sample_rate_override = 44100
	var config = rt.get_config()
	assert_eq(
		config.get("sample_rate", -1), 44100, "sample_rate_override should be applied when > 0"
	)


func test_audio_frame_size_auto_returns_valid_value():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.audio_frame_size = 0  # Auto
	var config = rt.get_config()
	var fs = config.get("audio_frame_size", -1)
	var valid = [256, 512, 1024, 2048]
	assert_true(fs in valid, "audio_frame_size Auto should return one of %s, got %s" % [valid, fs])


func test_audio_frame_size_manual_passthrough():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.audio_frame_size = 1024
	var config = rt.get_config()
	assert_eq(
		config.get("audio_frame_size", -1), 1024, "manual audio_frame_size should pass through"
	)


func test_direct_speaker_channels_default_and_passthrough():
	var rt = ResonanceRuntimeConfig.create_default()
	assert_eq(rt.direct_speaker_channels, 2, "default direct_speaker_channels")
	var config = rt.get_config()
	assert_eq(config.get("direct_speaker_channels", -1), 2, "get_config default")
	rt.direct_speaker_channels = 6
	assert_eq(
		rt.get_config().get("direct_speaker_channels", -1), 6, "5.1 channel count passthrough"
	)


func test_get_config_includes_simulator_and_hrtf_keys():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.hrtf_volume_db = -3.0
	rt.hrtf_normalization_type = 1
	rt.max_occlusion_samples = 32
	rt.max_simulation_sources = 48
	rt.max_transmission_surfaces = 48
	var config = rt.get_config()
	assert_eq(config.get("hrtf_volume_db", 999.0), -3.0, "hrtf_volume_db passthrough")
	assert_eq(config.get("hrtf_normalization_type", -1), 1, "hrtf_normalization_type passthrough")
	assert_eq(config.get("max_occlusion_samples", -1), 32, "max_occlusion_samples passthrough")
	assert_eq(config.get("max_simulation_sources", -1), 48, "max_simulation_sources passthrough")
	assert_eq(
		config.get("max_transmission_surfaces", -1), 48, "max_transmission_surfaces passthrough"
	)
	assert_false(
		config.has("context_validation"),
		"context flags live on ResonanceRuntime, not resource get_config"
	)
	assert_false(
		config.has("context_simd_level"),
		"context flags live on ResonanceRuntime, not resource get_config"
	)


func test_get_config_includes_reflection_performance_keys():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.reflections_adaptive_budget_us = 50000
	rt.reflections_adaptive_step_sec = 0.03
	rt.reflections_adaptive_max_extra_interval = 0.25
	rt.reflections_adaptive_decay_per_sec = 0.08
	rt.reflections_defer_after_scene_commit_us = 12000
	rt.convolution_ir_max_samples = 24000
	var config = rt.get_config()
	assert_eq(config.get("reflections_adaptive_budget_us", -1), 50000)
	assert_eq(config.get("reflections_adaptive_step_sec", -1.0), 0.03)
	assert_eq(config.get("reflections_adaptive_max_extra_interval", -1.0), 0.25)
	assert_eq(config.get("reflections_adaptive_decay_per_sec", -1.0), 0.08)
	assert_eq(config.get("reflections_defer_after_scene_commit_us", -1), 12000)
	assert_eq(config.get("convolution_ir_max_samples", -1), 24000)


func test_get_hrtf_sofa_effective_empty_list_returns_null():
	var rt = ResonanceRuntimeConfig.create_default()
	assert_null(rt.get_hrtf_sofa_effective(), "no SOFA when library empty and legacy slot null")


func test_apply_performance_schedule_preset_performance_sets_fields():
	var rt = ResonanceRuntimeConfig.create_default()
	rt.apply_performance_schedule_preset = 3
	assert_eq(rt.simulation_update_interval, 0.2, "Performance preset interval")
	assert_eq(rt.simulation_tick_throttle, 2, "Performance preset tick throttle")
	assert_eq(rt.direct_sim_interval, 0.03, "Performance preset direct interval")
	assert_eq(rt.geometry_update_throttle, 8, "Performance preset geometry throttle")
	assert_eq(rt.apply_performance_schedule_preset, 0, "preset snaps back to Custom after apply")


# --- get_effective_realtime_rays: pass-through on all platforms (v0.9.4+) ---
# Android no longer forces baked-only (0); realtime rays follow project settings.
# Do not reintroduce test_effective_realtime_rays_android_forces_zero — that was pre-0.9.4 behavior.


func test_effective_realtime_rays_android_passthrough():
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(2048, "Android"),
		2048,
		"Android should pass through configured rays"
	)
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(512, "Android"),
		512,
		"Android should pass through low value unchanged"
	)


func test_effective_realtime_rays_android_zero_stays_zero():
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(0, "Android"), 0, "Android 0 stays 0"
	)


func test_effective_realtime_rays_windows_passthrough():
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(2048, "Windows"),
		2048,
		"Windows should pass through"
	)
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(0, "Windows"), 0, "Windows 0 stays 0"
	)


func test_effective_realtime_rays_linux_passthrough():
	assert_eq(
		ResonanceRuntimeConfig.get_effective_realtime_rays(1024, "Linux"),
		1024,
		"Linux should pass through"
	)
