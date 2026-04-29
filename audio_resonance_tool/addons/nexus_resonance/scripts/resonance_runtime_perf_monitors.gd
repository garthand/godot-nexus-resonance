extends Object
class_name ResonanceRuntimePerfMonitors

## Registers Debugger [Performance] custom monitors for [ResonanceRuntime]. Use [method register] with a level:
## [constant PERF_MONITORS_OFF] … [constant PERF_MONITORS_FULL].

const PERF_MONITORS_OFF := 0
const PERF_MONITORS_CORE := 1
const PERF_MONITORS_STANDARD := 2
const PERF_MONITORS_FULL := 3

const MON_MAIN := "Nexus Resonance/Main/runtime_last_tick_us"
const MON_MAIN_VP := "Nexus Resonance/Main/runtime_viewport_sync_us"
## Sub-timing of the server tick inside [code]_process[/code]. Only populated when
## full-frame timing is active (level [constant PERF_MONITORS_FULL]); a subset of
## [constant MON_MAIN], so at lower tiers it would always read 0.
const MON_MAIN_TICK := "Nexus Resonance/Main/runtime_server_tick_us"
const MON_MAIN_FLUSH := "Nexus Resonance/Main/runtime_flush_sources_us"
## [code]_physics_process[/code] timing. Only active with the Custom ray tracer
## ([code]SCENETYPE_CUSTOM[/code]); for Embree/Radeon scenes these monitors never
## receive values and are therefore FULL-only.
const MON_PHYS := "Nexus Resonance/Main/runtime_physics_tick_us"
const MON_PHYS_VP := "Nexus Resonance/Main/runtime_physics_viewport_us"
const MON_PHYS_TICK := "Nexus Resonance/Main/runtime_physics_server_tick_us"
const MON_PHYS_FLUSH := "Nexus Resonance/Main/runtime_physics_flush_us"
const MON_W_SUM := "Nexus Resonance/Worker/last_tick_sum_us"

## Main / physics aggregate monitors: [code]min_level[/code] is [constant PERF_MONITORS_CORE] … [constant PERF_MONITORS_FULL].
const _MAIN_PHYS_MONITORS: Array[Dictionary] = [
	{"id": MON_MAIN, "callable": "_nexus_perf_read_main_usec", "min_level": PERF_MONITORS_CORE},
	{
		"id": MON_MAIN_VP,
		"callable": "_nexus_perf_read_main_viewport_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_MAIN_TICK,
		"callable": "_nexus_perf_read_main_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_MAIN_FLUSH,
		"callable": "_nexus_perf_read_main_flush_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS,
		"callable": "_nexus_perf_read_physics_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_VP,
		"callable": "_nexus_perf_read_physics_viewport_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_TICK,
		"callable": "_nexus_perf_read_physics_server_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_FLUSH,
		"callable": "_nexus_perf_read_physics_flush_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{"id": MON_W_SUM, "callable": "_nexus_perf_read_worker_sum", "min_level": PERF_MONITORS_CORE},
]

## [method ResonanceServer.get_simulation_worker_timing] keys → graph titles (microseconds unless noted). [code]min_level[/code] per row.
const _WORKER_US_MONITORS: Array[Dictionary] = [
	{
		"id": "Nexus Resonance/Worker/direct_us",
		"key": "us_run_direct",
		"min_level": PERF_MONITORS_STANDARD
	},
	{
		"id": "Nexus Resonance/Worker/reflections_us",
		"key": "us_run_reflections",
		"min_level": PERF_MONITORS_CORE
	},
	{
		"id": "Nexus Resonance/Worker/pathing_sim_us",
		"key": "us_run_pathing",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_total_us",
		"key": "us_sync_fetch",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_occlusion_us",
		"key": "us_sync_fetch_occlusion",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_reflections_us",
		"key": "us_sync_fetch_reflections",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_pathing_us",
		"key": "us_sync_fetch_pathing",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Worker/simulator_commit_us",
		"key": "us_simulator_commit",
		"min_level": PERF_MONITORS_STANDARD
	},
	{
		"id": "Nexus Resonance/Worker/scene_graph_commit_us",
		"key": "us_scene_graph_commit",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/dynamic_instanced_apply_us",
		"key": "us_dynamic_instanced_apply",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Main/last_dynamic_transform_enqueue_us",
		"key": "main_last_dynamic_transform_enqueue_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	## 0/1 flag (not microseconds) — set by the worker when its last wake did
	## heavy work (reflections/pathing/commit). Kept under the legacy id to stay
	## backwards compatible with existing dashboards.
	{
		"id": "Nexus Resonance/Worker/heavy_tick_flag",
		"key": "worker_last_wake_heavy",
		"min_level": PERF_MONITORS_CORE
	},
]

const MON_PATHING_RAN := "Nexus Resonance/Worker/pathing_ran_last_tick"
const MON_AUDIO_CONV_APPLY := "Nexus Resonance/Audio/convolution_reflection_apply_last_us"
const MON_AUDIO_CONV_BUS := "Nexus Resonance/Audio/convolution_reverb_bus_last_us"
const MON_AUDIO_MIXER_SANITIZE_AMBI := "Nexus Resonance/Audio/mixer_sanitize_ambi_last_us"
const MON_AUDIO_MIXER_SANITIZE_STEREO := "Nexus Resonance/Audio/mixer_sanitize_stereo_last_us"
## Aggregated audio-thread health. See [ResonanceRuntime]
## [code]_sample_audio_aggregates_if_due[/code] — summed across all living [code]resonance_player[/code]s at
## ~4 Hz. Cumulative counters are per-player-lifetime; call [method ResonancePlayer.reset_audio_instrumentation]
## to restart the sums.
const MON_AUDIO_OUTPUT_UNDERRUNS := "Nexus Resonance/Audio/output_underruns_total"
const MON_AUDIO_LATE_MIX := "Nexus Resonance/Audio/late_mix_total"
const MON_AUDIO_MAX_BLOCK := "Nexus Resonance/Audio/max_block_time_us"
const MON_AUDIO_VOICE_COUNT := "Nexus Resonance/Audio/active_voice_count"
## Registered [code]IPLSource[/code] handles on the server — one per active
## [ResonancePlayer] / [code]ResonanceSource[/code] with a valid config.
const MON_SERVER_SOURCE_COUNT := "Nexus Resonance/Server/active_source_count"
const MON_SERVER_PROBE_BATCH_COUNT := "Nexus Resonance/Server/active_probe_batch_count"

## Extra monitors after worker rows: id, callable on [param owner], min_level.
const _EXTRA_MONITORS: Array[Dictionary] = [
	{
		"id": MON_PATHING_RAN,
		"callable": "_nexus_perf_read_pathing_ran_tick",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_AUDIO_CONV_APPLY,
		"callable": "_nexus_perf_read_convolution_apply_last_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_CONV_BUS,
		"callable": "_nexus_perf_read_convolution_reverb_bus_last_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_MIXER_SANITIZE_AMBI,
		"callable": "_nexus_perf_read_mixer_sanitize_ambi_last_us",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": MON_AUDIO_MIXER_SANITIZE_STEREO,
		"callable": "_nexus_perf_read_mixer_sanitize_stereo_last_us",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": MON_AUDIO_OUTPUT_UNDERRUNS,
		"callable": "_nexus_perf_read_audio_output_underruns_total",
		"min_level": PERF_MONITORS_CORE,
	},
	{
		"id": MON_AUDIO_LATE_MIX,
		"callable": "_nexus_perf_read_audio_late_mix_total",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_MAX_BLOCK,
		"callable": "_nexus_perf_read_audio_max_block_time_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_VOICE_COUNT,
		"callable": "_nexus_perf_read_audio_active_voice_count",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_SERVER_SOURCE_COUNT,
		"callable": "_nexus_perf_read_server_active_source_count",
		"min_level": PERF_MONITORS_CORE,
	},
	{
		"id": MON_SERVER_PROBE_BATCH_COUNT,
		"callable": "_nexus_perf_read_server_active_probe_batch_count",
		"min_level": PERF_MONITORS_FULL,
	},
]


static func monitor_ids_for_level(level: int) -> Array[String]:
	if level <= PERF_MONITORS_OFF:
		return [] as Array[String]
	var out: Array[String] = [] as Array[String]
	for row in _MAIN_PHYS_MONITORS:
		if int(row["min_level"]) <= level:
			out.append(row["id"])
	for row in _WORKER_US_MONITORS:
		if int(row["min_level"]) <= level:
			out.append(row["id"])
	for row in _EXTRA_MONITORS:
		if int(row["min_level"]) <= level:
			out.append(row["id"])
	return out


## Same as [method monitor_ids_for_level] with [constant PERF_MONITORS_FULL] (full legacy list).
static func monitor_ids() -> Array[String]:
	return monitor_ids_for_level(PERF_MONITORS_FULL)


## Sum of last worker tick wall-time components ([method ResonanceServer.get_simulation_worker_timing]), microseconds.
## Includes dynamic instanced-mesh apply and scene-graph commit when present.
static func simulation_worker_timing_sum(w: Dictionary) -> int:
	return (
		int(w.get("us_dynamic_instanced_apply", 0))
		+ int(w.get("us_scene_graph_commit", 0))
		+ int(w.get("us_run_direct", 0))
		+ int(w.get("us_run_reflections", 0))
		+ int(w.get("us_run_pathing", 0))
		+ int(w.get("us_sync_fetch", 0))
		+ int(w.get("us_simulator_commit", 0))
	)


static func _remove_all_nexus_monitors() -> void:
	var existing: Array = Performance.get_custom_monitor_names()
	for name in existing:
		if str(name).begins_with("Nexus Resonance/"):
			Performance.remove_custom_monitor(name)


static func register(owner: Node, level: int = PERF_MONITORS_STANDARD) -> void:
	_remove_all_nexus_monitors()
	if level <= PERF_MONITORS_OFF:
		return
	for row in _MAIN_PHYS_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(row["id"], Callable(owner, row["callable"]))
	for row in _WORKER_US_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(
				row["id"], Callable(owner, "_nexus_perf_read_worker_timing_field").bind(row["key"])
			)
	for row in _EXTRA_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(row["id"], Callable(owner, row["callable"]))


static func unregister_all() -> void:
	if Engine.is_editor_hint():
		return
	_remove_all_nexus_monitors()
