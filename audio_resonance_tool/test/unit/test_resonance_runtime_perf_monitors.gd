extends GutTest

## Unit tests for [ResonanceRuntimePerfMonitors] static helpers.


func test_simulation_worker_timing_sum_empty_dict() -> void:
	assert_eq(ResonanceRuntimePerfMonitors.simulation_worker_timing_sum({}), 0)


func test_simulation_worker_timing_sum_partial_keys() -> void:
	assert_eq(
		ResonanceRuntimePerfMonitors.simulation_worker_timing_sum({"us_run_direct": 5}),
		5
	)


func test_simulation_worker_timing_sum_all_keys() -> void:
	var w: Dictionary = {
		"us_dynamic_instanced_apply": 10,
		"us_scene_graph_commit": 20,
		"us_run_direct": 1,
		"us_run_reflections": 2,
		"us_run_pathing": 3,
		"us_sync_fetch": 4,
		"us_simulator_commit": 5,
	}
	assert_eq(ResonanceRuntimePerfMonitors.simulation_worker_timing_sum(w), 45)


func test_monitor_ids_off_empty() -> void:
	var ids: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_OFF
	)
	assert_eq(ids.size(), 0)


func test_monitor_ids_core_minimal() -> void:
	var ids: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_CORE
	)
	assert_eq(ids.size(), 6)
	assert_true(ids.has("Nexus Resonance/Main/runtime_last_tick_us"))
	assert_true(ids.has("Nexus Resonance/Worker/last_tick_sum_us"))
	assert_true(ids.has("Nexus Resonance/Worker/reflections_us"))
	assert_true(ids.has("Nexus Resonance/Worker/heavy_tick_flag"))
	assert_true(ids.has("Nexus Resonance/Audio/output_underruns_total"))
	assert_true(ids.has("Nexus Resonance/Server/active_source_count"))


func test_monitor_ids_standard_smaller_than_full() -> void:
	var st: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_STANDARD
	)
	var full: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_FULL
	)
	assert_gt(full.size(), st.size())
	assert_eq(st.size(), 17)
	assert_eq(full.size(), 32)


func test_monitor_ids_standard_has_actionable_diagnostics() -> void:
	var ids: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_STANDARD
	)
	assert_true(ids.has("Nexus Resonance/Worker/sync_fetch_reflections_us"))
	assert_true(ids.has("Nexus Resonance/Main/last_dynamic_transform_enqueue_us"))
	assert_true(ids.has("Nexus Resonance/Audio/convolution_reflection_apply_last_us"))
	assert_true(ids.has("Nexus Resonance/Audio/convolution_reverb_bus_last_us"))
	assert_true(ids.has("Nexus Resonance/Audio/late_mix_total"))
	assert_true(ids.has("Nexus Resonance/Audio/max_block_time_us"))
	assert_true(ids.has("Nexus Resonance/Audio/active_voice_count"))
	# runtime_server_tick / runtime_physics_tick are FULL-only because they
	# only receive a value when full-frame timing is active.
	assert_false(ids.has("Nexus Resonance/Main/runtime_server_tick_us"))
	assert_false(ids.has("Nexus Resonance/Main/runtime_physics_tick_us"))
	assert_false(ids.has("Nexus Resonance/Main/runtime_viewport_sync_us"))
	assert_false(ids.has("Nexus Resonance/Server/active_probe_batch_count"))


func test_monitor_ids_full_matches_legacy_monitor_ids() -> void:
	var full: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_FULL
	)
	var legacy: Array = ResonanceRuntimePerfMonitors.monitor_ids()
	assert_eq(full.size(), legacy.size())
	for d in legacy:
		assert_true(full.has(d), "full tier should include " + str(d))


func test_monitor_ids_full_has_pathing_and_mixer_sanitize() -> void:
	var ids: Array = ResonanceRuntimePerfMonitors.monitor_ids_for_level(
		ResonanceRuntimePerfMonitors.PERF_MONITORS_FULL
	)
	assert_true(ids.has("Nexus Resonance/Worker/pathing_sim_us"))
	assert_true(ids.has("Nexus Resonance/Worker/pathing_ran_last_tick"))
	assert_true(ids.has("Nexus Resonance/Audio/mixer_sanitize_ambi_last_us"))
