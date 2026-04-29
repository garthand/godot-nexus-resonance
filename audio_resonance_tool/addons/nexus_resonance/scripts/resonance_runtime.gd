@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config_icon.svg")
class_name ResonanceRuntime
extends Node

## Nexus Resonance scene configuration node.
## Add via **Add Child Node** → **ResonanceRuntime** (global class; always includes this script).
## Assign a ResonanceRuntimeConfig resource (create new or link existing) for runtime settings.

const ResonanceRuntimeConfig = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_config.gd"
)
const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")
const ResonanceFMODBridgeScript = preload(
	"res://addons/nexus_resonance/scripts/resonance_fmod_bridge.gd"
)

var _runtime: ResonanceRuntimeConfig
## Runtime configuration resource. Create or link .tres. Auto-created default if empty.
@export var runtime: ResonanceRuntimeConfig:
	get:
		return _runtime
	set(val):
		_disconnect_runtime_signals()
		_runtime = val
		_connect_runtime_signals()
		_warn_restart_if_needed()

@export_group("FMOD Bridge")
## Enable Steam Audio + FMOD bridge. Requires fmod-gdextension and phonon_fmod plugin in FMOD path.
@export var fmod_bridge_enabled: bool = false

@export_group("Runtime Debug")
## When off, overlay keys do nothing (hide dev tools from players). Turn on for development builds only.
@export var enable_debug: bool = false:
	get:
		return _enable_debug
	set(v):
		if _enable_debug == v:
			return
		var was_on := _enable_debug
		_enable_debug = v
		if was_on and not v:
			_disable_runtime_debug_ui()

## Key to toggle debug overlay (audio/server status, log).
@export var debug_overlay_toggle_key: Key = KEY_F1
## Key to toggle performance overlay (FPS, frame time). Off until first toggle; node is created lazily.
@export var performance_overlay_toggle_key: Key = KEY_F2
## Key to toggle player/source ray visualization (occlusion + reflection rays in engine). Requires ResonanceServer debug support.
@export var player_overlay_toggle_key: Key = KEY_F3
## Debugger [Performance] custom monitors from Nexus (main/worker/audio µs). [b]Off[/b] removes all; [b]Standard[/b] is default (~12 graphs). [b]Full[/b] matches pre-0.9.5 verbosity and enables per-phase main/physics timing in [method _process].
@export_enum("Off:0", "Core:1", "Standard:2", "Full:3",) var performance_custom_monitors: int = 2:
	get:
		return _performance_custom_monitors
	set(v):
		var nv := clampi(
			v,
			ResonanceRuntimePerfMonitors.PERF_MONITORS_OFF,
			ResonanceRuntimePerfMonitors.PERF_MONITORS_FULL
		)
		if _performance_custom_monitors == nv:
			return
		_performance_custom_monitors = nv
		_refresh_performance_custom_monitors_if_ready()

## Caps the Steam Audio IPL context SIMD level (older CPUs / crash debugging). **Default** lets Phonon pick the highest usable set. Lower sets trade some speed for broader CPU support.
@export_enum("Default:-1", "AVX-512:0", "AVX2:1", "AVX:2", "SSE4:3", "SSE2:4")
var context_simd_level: int = -1:
	get:
		return _context_simd_level
	set(v):
		if _context_simd_level == v:
			return
		_context_simd_level = v
		_warn_restart_if_needed()

## Enables Steam Audio [code]IPL_CONTEXTFLAGS_VALIDATION[/code]: extra API checks and more console warnings. For diagnosing integration issues only; hurts performance.
@export var context_validation: bool = false:
	get:
		return _context_validation
	set(v):
		if _context_validation == v:
			return
		_context_validation = v
		_warn_restart_if_needed()

var _performance_custom_monitors: int = 2
var _context_simd_level: int = -1
var _context_validation: bool = false
var _enable_debug: bool = false
var _debug_overlay_visible: bool = false
var _performance_overlay_visible: bool = false
var _static_scene_reload_pending: bool = false
## Effective source/reflection debug draw (was debug_sources bool).
var _player_overlay_visible: bool = false

var _fmod_bridge: RefCounted = null  # ResonanceFMODBridgeScript
var _runtime_bus: ResonanceRuntimeBus
var _reverb_activator: ResonanceReverbActivator

## Reverb Bus activator instrumentation (for debugging convolution routing).
## Read from debug overlay via get_tree().get_first_node_in_group("resonance_runtime").
var activator_instrumentation: Dictionary:
	get:
		return _reverb_activator.instrumentation if _reverb_activator else {}

## Duration of the last [method _process] work in microseconds (sum of sub-phases below, including any overhead between them). See [code]docs/NEXUS_PERFORMANCE_TUNING.md[/code] for comparison with worker totals.
var main_thread_last_tick_usec: int = 0
## Last [method _process]: [method _fill_activator_buffer] only.
var main_thread_activator_usec: int = 0
## Last [method _process]: audio engine reinit when the reverb bus reports a new frame size ([code]consume_pending_reinit_frame_size[/code]). Usually 0.
var main_thread_reinit_usec: int = 0
## Last [method _process]: [method _apply_resonance_viewport_to_server] (Embree/Default path only; 0 when Custom tracer — work moves to [method _physics_process]).
var main_thread_viewport_usec: int = 0
## Last [method _process]: [method ResonanceServer.tick] (0 when Custom tracer).
var main_thread_tick_usec: int = 0
## Last [method _process]: [method ResonanceServer.flush_pending_source_updates] if present (0 when Custom tracer).
var main_thread_flush_usec: int = 0
## When [method ResonanceServer.uses_custom_ray_tracer] is true: microseconds for the last [method _physics_process] block (viewport + tick + flush). See also split fields below. Otherwise 0.
var runtime_physics_tick_usec: int = 0
## Last [method _physics_process] (Custom tracer): viewport sync only.
var runtime_physics_viewport_usec: int = 0
## Last [method _physics_process] (Custom tracer): [method ResonanceServer.tick].
var runtime_physics_server_tick_usec: int = 0
## Last [method _physics_process] (Custom tracer): [method ResonanceServer.flush_pending_source_updates] if present.
var runtime_physics_flush_usec: int = 0

## Audio-thread aggregates across all living [ResonancePlayer] nodes. Refreshed from
## [method _sample_audio_aggregates] every [code]AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC[/code]; queried by the
## audio-dropout custom monitors. Cumulative counters are per-player-lifetime (calling
## [method ResonancePlayer.reset_audio_instrumentation] resets them).
var _audio_agg_output_underruns_total: int = 0
var _audio_agg_late_mix_total: int = 0
var _audio_agg_max_block_time_us: int = 0
var _audio_agg_voice_count: int = 0
var _audio_agg_last_sample_sec: float = -1.0
const AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC: float = 0.25

## Cached viewport sync: skip redundant [code]set_physics_world[/code] / exclude RIDs / camera [code]update_listener[/code] when unchanged.
var _vp_sync_cache_valid: bool = false
var _vp_sync_last_world_rid: RID = RID()
var _vp_sync_last_exclude_ids: PackedInt64Array = PackedInt64Array()
var _vp_sync_last_cam_xform: Transform3D = Transform3D.IDENTITY
## True after a frame had nodes in [code]resonance_listener[/code]; cleared when using camera fallback so removal triggers a sync.
var _vp_sync_last_had_listener_nodes: bool = false


func _reset_viewport_sync_cache() -> void:
	_vp_sync_cache_valid = false
	_vp_sync_last_world_rid = RID()
	_vp_sync_last_exclude_ids = PackedInt64Array()
	_vp_sync_last_cam_xform = Transform3D.IDENTITY
	_vp_sync_last_had_listener_nodes = false


func _sorted_rid_int_ids(rids: Array) -> PackedInt64Array:
	var ids: Array = []
	for r in rids:
		if r is RID and r.is_valid():
			ids.append(r.get_id())
	ids.sort()
	var out := PackedInt64Array()
	out.resize(ids.size())
	for i in ids.size():
		out[i] = ids[i]
	return out


func _camera_listener_xform_changed(cam: Camera3D) -> bool:
	var xf := cam.global_transform
	return (
		not xf.origin.is_equal_approx(_vp_sync_last_cam_xform.origin)
		or not xf.basis.is_equal_approx(_vp_sync_last_cam_xform.basis)
	)


## Returns bake params from first Probe Volume with bake_config, or default. Used before init so pathing visibility params are set.
func _get_bake_params_for_runtime() -> Dictionary:
	if not is_inside_tree():
		return ResonanceBakeConfig.create_default().get_bake_params()
	var tree = get_tree()
	if not tree:
		return ResonanceBakeConfig.create_default().get_bake_params()
	var volumes = tree.get_nodes_in_group("resonance_probe_volume")
	for vol in volumes:
		var bc = vol.get("bake_config") if "bake_config" in vol else null
		if bc and bc.has_method("get_bake_params"):
			return bc.get_bake_params()
	return ResonanceBakeConfig.create_default().get_bake_params()


## Returns effective bus for Direct + Pathing. Public API for debug overlay and external use.
func get_bus_effective() -> StringName:
	return _get_bus_effective()


## Re-applies [ResonanceRuntimeBus] routing ([code]set_bus[/code] / [code]set_reverb_split_output[/code]) to every
## node in group [code]resonance_player[/code]. [ResonanceRuntime] runs this once at startup; spawned
## [ResonancePlayer] nodes must call this (or you call it once after spawning) so config mix levels and
## buses match editor-placed sources instead of falling back to plain [AudioStreamPlayer3D] behavior.
func refresh_player_bus_routing() -> void:
	_apply_bus_to_players()


## Returns the FMOD bridge when fmod_bridge_enabled. Used by ResonanceFmodEventEmitter.
func get_fmod_bridge() -> RefCounted:
	return _fmod_bridge


## Returns config dict for init_audio_engine. Merges runtime resource with [member debug_occlusion] and Steam Audio context debug fields from this node.
func get_config_dict() -> Dictionary:
	var cfg: Dictionary = _runtime.get_config() if _runtime else {}
	cfg["debug_occlusion"] = _player_overlay_visible
	cfg["context_simd_level"] = context_simd_level
	cfg["context_validation"] = context_validation
	return cfg


func _ready() -> void:
	if not _runtime:
		_runtime = ResonanceRuntimeConfig.create_default()
	add_to_group("resonance_runtime")
	set_process_priority(100)
	set_physics_process_priority(100)
	_connect_runtime_signals()
	if Engine.is_editor_hint():
		call_deferred("_notify_volumes_runtime_config_changed")
	if not get_node_or_null("DebugOverlay"):
		const DebugOverlayScript = preload("res://addons/nexus_resonance/scripts/debug_overlay.gd")
		var overlay = DebugOverlayScript.new()
		overlay.name = "DebugOverlay"
		add_child(overlay)
	_initialize_server()
	if not Engine.is_editor_hint():
		_setup_activator()
		call_deferred("_apply_bus_to_players")
		# Node.tree_exiting (not SceneTree): fires before this node leaves the tree (quit or scene change).
		if not tree_exiting.is_connected(_on_scene_tree_exiting):
			tree_exiting.connect(_on_scene_tree_exiting)
	_update_debug_overlay_visibility()
	if not Engine.is_editor_hint():
		_refresh_performance_custom_monitors_if_ready()


func _on_scene_tree_exiting() -> void:
	_cleanup_reverb_activator()


func _cleanup_reverb_activator() -> void:
	if _reverb_activator:
		_reverb_activator.cleanup()
		_reverb_activator = null


func _exit_tree() -> void:
	_reset_viewport_sync_cache()
	if tree_exiting.is_connected(_on_scene_tree_exiting):
		tree_exiting.disconnect(_on_scene_tree_exiting)
	_unregister_nexus_performance_monitors()
	_cleanup_reverb_activator()
	var perf_overlay = get_node_or_null("PerformanceOverlay")
	if perf_overlay:
		perf_overlay.visible = false
		perf_overlay.process_mode = Node.PROCESS_MODE_DISABLED
	# Always call native shutdown when the singleton exists; C++ tears down only if a context is live.
	var srv_shutdown: Variant = ResonanceServerAccess.get_server()
	if srv_shutdown != null and srv_shutdown.has_method("shutdown"):
		srv_shutdown.shutdown()
	if _fmod_bridge:
		_fmod_bridge.shutdown_bridge()
		_fmod_bridge = null


func _input(event: InputEvent) -> void:
	if Engine.is_editor_hint():
		return
	if not _enable_debug:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == debug_overlay_toggle_key:
			_toggle_debug_overlay()
			get_viewport().set_input_as_handled()
		elif event.keycode == performance_overlay_toggle_key:
			_toggle_performance_overlay()
			get_viewport().set_input_as_handled()
		elif event.keycode == player_overlay_toggle_key:
			_toggle_player_overlay()
			get_viewport().set_input_as_handled()


func _init_fmod_bridge() -> void:
	_fmod_bridge = ResonanceFMODBridgeScript.new()
	if _fmod_bridge.init_bridge():
		pass  # Success; bridge is ready
	else:
		push_warning(
			"Nexus Resonance: FMOD bridge init failed. Ensure phonon_fmod plugin is in FMOD path."
		)


func _toggle_debug_overlay() -> void:
	_debug_overlay_visible = not _debug_overlay_visible
	_update_debug_overlay_visibility()


func _create_performance_overlay() -> void:
	if Engine.is_editor_hint() or get_node_or_null("PerformanceOverlay"):
		return
	const PerformanceOverlayScript = preload(
		"res://addons/nexus_resonance/scripts/performance_overlay.gd"
	)
	var perf_overlay = PerformanceOverlayScript.new()
	perf_overlay.name = "PerformanceOverlay"
	perf_overlay.visible = false
	add_child(perf_overlay)


func _toggle_performance_overlay() -> void:
	_performance_overlay_visible = not _performance_overlay_visible
	var overlay = get_node_or_null("PerformanceOverlay")
	if overlay == null and _performance_overlay_visible:
		_create_performance_overlay()
		overlay = get_node_or_null("PerformanceOverlay")
	if overlay:
		overlay.visible = _performance_overlay_visible
		if _performance_overlay_visible:
			overlay.process_mode = Node.PROCESS_MODE_INHERIT


func _toggle_player_overlay() -> void:
	_player_overlay_visible = not _player_overlay_visible
	_apply_debug_flags()
	_refresh_resonance_geometry_for_debug_viz()


func _disable_runtime_debug_ui() -> void:
	_debug_overlay_visible = false
	_performance_overlay_visible = false
	_player_overlay_visible = false
	_update_debug_overlay_visibility()
	var perf := get_node_or_null("PerformanceOverlay")
	if perf:
		perf.visible = false
		perf.process_mode = Node.PROCESS_MODE_DISABLED
	_apply_debug_flags()
	_refresh_resonance_geometry_for_debug_viz()


func _refresh_resonance_geometry_for_debug_viz() -> void:
	if not is_inside_tree():
		return
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return
	# sync_reflection_debug_viz updates RayTraceDebugContext only. Full refresh_geometry() rebuilds
	# IPL InstancedMesh/sub-scenes and breaks Embree dynamic occlusion when toggling F3.
	get_tree().call_group_flags(
		SceneTree.GROUP_CALL_DEFERRED, "resonance_geometry", "sync_reflection_debug_viz"
	)


## CollisionObject3D RIDs along the active camera parent chain plus [code]resonance_listener[/code] group nodes.
## Used with Custom scene type so occlusion/reflection rays do not hit the listener body.
func _collect_listener_physics_exclude_rids(vp: Viewport) -> Array[RID]:
	var out: Array[RID] = []
	var cam = vp.get_camera_3d()
	if cam:
		var n: Node = cam
		while n:
			if n is CollisionObject3D:
				out.append(n.get_rid())
			n = n.get_parent()
	var tree = get_tree()
	if tree:
		var listeners = tree.get_nodes_in_group("resonance_listener")
		for node in listeners:
			if node is CollisionObject3D:
				out.append(node.get_rid())
	return out


## Pushes active viewport world, optional Custom-tracer exclude RIDs, and default-camera listener into [ResonanceServer].
func _apply_resonance_viewport_to_server(vp: Viewport) -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return
	if srv.has_method("set_physics_world"):
		var w3 = vp.get_world_3d()
		var wrid: RID = w3.get_rid() if w3 else RID()
		if not _vp_sync_cache_valid or wrid != _vp_sync_last_world_rid:
			srv.set_physics_world(w3)
			_vp_sync_last_world_rid = wrid
	if srv.uses_custom_ray_tracer() and srv.has_method("set_listener_physics_ray_exclude_rids"):
		var collected: Array = _collect_listener_physics_exclude_rids(vp)
		var ex_ids := _sorted_rid_int_ids(collected)
		if not _vp_sync_cache_valid or ex_ids != _vp_sync_last_exclude_ids:
			srv.set_listener_physics_ray_exclude_rids(collected)
			_vp_sync_last_exclude_ids = ex_ids
	var cam = vp.get_camera_3d()
	if cam and is_inside_tree():
		var listeners = get_tree().get_nodes_in_group("resonance_listener")
		if listeners.is_empty():
			if (
				not _vp_sync_cache_valid
				or _vp_sync_last_had_listener_nodes
				or _camera_listener_xform_changed(cam)
			):
				srv.update_listener(
					cam.global_position, -cam.global_transform.basis.z, cam.global_transform.basis.y
				)
				_vp_sync_last_cam_xform = cam.global_transform
			_vp_sync_last_had_listener_nodes = false
		else:
			_vp_sync_last_had_listener_nodes = true
	_vp_sync_cache_valid = true


## Custom (Godot Physics) scene type runs [method ResonanceServer.tick] in [method _physics_process] so simulation
## stays aligned with the physics step and the active [code]World3D[/code]. Godot still serves
## [code]PhysicsDirectSpaceState3D[/code] only on the main thread when 3D physics uses
## [code]physics/3d/run_on_separate_thread[/code]; in that mode [method _physics_process] runs on the physics
## thread, so ray queries fail — turn off separate-thread 3D physics for Custom or use a non-Custom scene type.
func _sync_physics_process_for_custom_tracer() -> void:
	if Engine.is_editor_hint():
		set_physics_process(false)
		return
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or not srv.is_initialized():
		set_physics_process(false)
		return
	set_physics_process(srv.uses_custom_ray_tracer())


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	main_thread_activator_usec = 0
	main_thread_reinit_usec = 0
	main_thread_viewport_usec = 0
	main_thread_tick_usec = 0
	main_thread_flush_usec = 0
	var t0_usec := Time.get_ticks_usec()
	var t_a0 := Time.get_ticks_usec()
	_fill_activator_buffer()
	var da := Time.get_ticks_usec() - t_a0
	main_thread_activator_usec = da if da > 0 else 0
	var srv: Variant = ResonanceServerAccess.get_server()
	var srv_ready: bool = srv != null and srv.is_initialized()
	var custom_tracer: bool = srv_ready and srv.uses_custom_ray_tracer()
	if not srv_ready or not custom_tracer:
		runtime_physics_tick_usec = 0
		runtime_physics_viewport_usec = 0
		runtime_physics_server_tick_usec = 0
		runtime_physics_flush_usec = 0
	if srv_ready:
		# Runtime frame_size detection: reverb bus may have requested reinit with actual Godot frame_count
		var pending: int = srv.consume_pending_reinit_frame_size()
		if pending > 0:
			var t_r0 := Time.get_ticks_usec()
			var cfg := get_config_dict()
			cfg["audio_frame_size"] = pending
			_prepare_geometry_before_reinit()
			srv.reinit_audio_engine(cfg)
			_reset_viewport_sync_cache()
			call_deferred("_reload_after_reinit")
			var dr := Time.get_ticks_usec() - t_r0
			main_thread_reinit_usec = dr if dr > 0 else 0
		var vp = get_viewport()
		if vp and not custom_tracer:
			if _nexus_perf_uses_full_frame_timing():
				var t_v0 := Time.get_ticks_usec()
				_apply_resonance_viewport_to_server(vp)
				var dv := Time.get_ticks_usec() - t_v0
				main_thread_viewport_usec = dv if dv > 0 else 0
			else:
				_apply_resonance_viewport_to_server(vp)
			if _nexus_perf_uses_full_frame_timing():
				var t_t0 := Time.get_ticks_usec()
				srv.tick(delta)
				var dt := Time.get_ticks_usec() - t_t0
				main_thread_tick_usec = dt if dt > 0 else 0
			else:
				srv.tick(delta)
			if srv.has_method("flush_pending_source_updates"):
				if _nexus_perf_uses_full_frame_timing():
					var t_f0 := Time.get_ticks_usec()
					srv.flush_pending_source_updates()
					var df := Time.get_ticks_usec() - t_f0
					main_thread_flush_usec = df if df > 0 else 0
				else:
					srv.flush_pending_source_updates()
	var dt_usec := Time.get_ticks_usec() - t0_usec
	main_thread_last_tick_usec = dt_usec if dt_usec > 0 else 0
	_sample_audio_aggregates_if_due()


func _physics_process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or not srv.is_initialized() or not srv.uses_custom_ray_tracer():
		return
	runtime_physics_viewport_usec = 0
	runtime_physics_server_tick_usec = 0
	runtime_physics_flush_usec = 0
	var t0_usec := Time.get_ticks_usec()
	var vp = get_viewport()
	if vp:
		if _nexus_perf_uses_full_frame_timing():
			var t_v0 := Time.get_ticks_usec()
			_apply_resonance_viewport_to_server(vp)
			var dv := Time.get_ticks_usec() - t_v0
			runtime_physics_viewport_usec = dv if dv > 0 else 0
		else:
			_apply_resonance_viewport_to_server(vp)
	if _nexus_perf_uses_full_frame_timing():
		var t_t0 := Time.get_ticks_usec()
		srv.tick(delta)
		var dt := Time.get_ticks_usec() - t_t0
		runtime_physics_server_tick_usec = dt if dt > 0 else 0
	else:
		srv.tick(delta)
	if srv.has_method("flush_pending_source_updates"):
		if _nexus_perf_uses_full_frame_timing():
			var t_f0 := Time.get_ticks_usec()
			srv.flush_pending_source_updates()
			var df := Time.get_ticks_usec() - t_f0
			runtime_physics_flush_usec = df if df > 0 else 0
		else:
			srv.flush_pending_source_updates()
	var dt_usec := Time.get_ticks_usec() - t0_usec
	runtime_physics_tick_usec = dt_usec if dt_usec > 0 else 0


## Public: rebuilds the IPL static-scene set from the current scene tree. Use after a
## [member ResonanceStaticScene.static_scene_asset] is swapped at runtime. Debounced to one
## reload per frame so multiple simultaneous swaps (e.g. level streaming) batch into a single
## [code]clear_static_scenes[/code] + [code]add_static_scene_from_asset[/code] pass.
func request_static_scene_reload() -> void:
	if not is_inside_tree():
		return
	if _static_scene_reload_pending:
		return
	_static_scene_reload_pending = true
	call_deferred("_perform_deferred_static_scene_reload")


func _perform_deferred_static_scene_reload() -> void:
	_static_scene_reload_pending = false
	if not is_inside_tree():
		return
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return
	_reload_static_scenes_from_tree(srv, get_tree().get_root())


## Loads every [ResonanceStaticScene] under [param tree_root] into the server (additive API), or legacy single-scene load.
func _reload_static_scenes_from_tree(srv: Variant, tree_root: Node) -> void:
	if srv == null or tree_root == null:
		return
	var static_scenes: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_static_scenes(tree_root, static_scenes)
	if static_scenes.is_empty():
		return
	if srv.has_method("clear_static_scenes") and srv.has_method("add_static_scene_from_asset"):
		srv.clear_static_scenes()
		for ss in static_scenes:
			if ss.static_scene_asset and ss.has_valid_asset():
				srv.add_static_scene_from_asset(ss.static_scene_asset, ss.get_global_transform())
	elif srv.has_method("load_static_scene_from_asset"):
		if static_scenes.size() == 1:
			var only = static_scenes[0]
			if only.static_scene_asset and only.has_valid_asset():
				srv.load_static_scene_from_asset(
					only.static_scene_asset, only.get_global_transform()
				)
		else:
			push_error(
				"Nexus Resonance: Multiple ResonanceStaticScene nodes require clear_static_scenes and add_static_scene_from_asset on ResonanceServer; this build only supports load_static_scene_from_asset (single scene)."
			)


func _initialize_server() -> void:
	# In editor, probe_toolbar inits when needed for bake. Avoid Steam Audio init on scene load (can crash).
	if Engine.is_editor_hint():
		return
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null:
		push_error("Nexus Resonance: GDExtension not loaded.")
		return

	var cfg = get_config_dict()
	if cfg.is_empty():
		push_error("Nexus Resonance: Failed to build config.")
		return
	if srv.is_initialized() and srv.has_method("reinit_audio_engine"):
		_prepare_geometry_before_reinit()
		srv.reinit_audio_engine(cfg)
		_reset_viewport_sync_cache()
		call_deferred("_reload_after_reinit")
		_sync_physics_process_for_custom_tracer()
		return
	if srv.is_initialized():
		_sync_physics_process_for_custom_tracer()
		return
	# Set bake params before init so pathing visibility params (from bake_config) are available.
	var bake_params = _get_bake_params_for_runtime()
	srv.set_bake_params(bake_params)
	srv.init_audio_engine(cfg)
	_reset_viewport_sync_cache()
	# FMOD Bridge: Connect Steam Audio to FMOD when enabled
	if fmod_bridge_enabled:
		_init_fmod_bridge()
	# Unity-style: load static scene(s) from ResonanceStaticScene nodes. Additive: one per scene.
	if is_inside_tree():
		_reload_static_scenes_from_tree(srv, get_tree().get_root())
	# add_static_scene_from_asset / load_static_scene_from_asset commit the IPL scene and set scene_dirty;
	# the simulation worker runs iplSceneCommit + RunDirect on the next tick. Deferred refresh re-runs
	# ResonanceGeometry::_create_meshes for nodes that missed server init during _ready (same as reinit path).
	if is_inside_tree():
		var tree = get_tree()
		if tree:
			tree.call_group_flags(
				SceneTree.GROUP_CALL_DEFERRED, "resonance_geometry", "refresh_geometry"
			)
	_apply_debug_flags()
	_apply_perspective_correction()
	# Mute spatialized player output until several worker RunDirect ticks after scene/geometry settle (see kSpatialAudioWarmupWorkerPasses).
	call_deferred("_deferred_reset_spatial_audio_warmup_passes")
	_sync_physics_process_for_custom_tracer()


func _deferred_reset_spatial_audio_warmup_passes() -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null and srv.has_method("reset_spatial_audio_warmup_passes"):
		srv.reset_spatial_audio_warmup_passes()


func _apply_debug_flags() -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null:
		srv.set_debug_occlusion(_player_overlay_visible)
		if srv.has_method("set_debug_reflections"):
			srv.set_debug_reflections(_player_overlay_visible)


func _apply_perspective_correction() -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null:
		srv.set_perspective_correction_enabled(runtime.perspective_correction_enabled)
		srv.set_perspective_correction_factor(runtime.perspective_correction_factor)
		srv.set_reverb_transmission_amount(runtime.reverb_transmission_amount)
		srv.set_apply_occlusion_to_baked_reflections(runtime.apply_occlusion_to_baked_reflections)
		srv.set_apply_distance_curve_to_reflections(runtime.apply_distance_curve_to_reflections)


func _get_bus_effective() -> StringName:
	if _runtime:
		return _runtime.get_bus_effective()
	return &"Master"


func _get_reverb_bus_name() -> StringName:
	return _runtime.get_reverb_bus_name_effective() if _runtime else &"ResonanceReverb"


func _get_reverb_bus_send() -> StringName:
	## Reverb bus send follows runtime dry bus (same as Steam Audio Unity indirect routing model).
	return _get_bus_effective()


func _apply_bus_to_players() -> void:
	if not is_inside_tree() or _runtime_bus == null:
		return
	var tree = get_tree()
	if not tree:
		return
	_runtime_bus.apply_bus_to_players(tree)


func _setup_activator() -> void:
	_runtime_bus = ResonanceRuntimeBus.new(
		Callable(self, "_get_bus_effective"),
		Callable(self, "_get_reverb_bus_name"),
		Callable(self, "_get_reverb_bus_send")
	)
	_reverb_activator = ResonanceReverbActivator.new()
	_reverb_activator.setup(self, _runtime_bus)


func _fill_activator_buffer() -> void:
	## Keeps the Reverb Bus active so Godot does not disable it after silence.
	## The activator feeds a low-level signal so the bus stays active; the effect
	## overwrites output with mixer content.
	if _reverb_activator == null or _runtime_bus == null:
		return
	_reverb_activator.fill_buffer(_runtime_bus)


func _prepare_geometry_before_reinit() -> void:
	if is_inside_tree():
		get_tree().call_group("resonance_geometry", "discard_meshes_before_scene_release")


func _reload_after_reinit() -> void:
	if not is_inside_tree():
		return
	var tree = get_tree()
	var srv: Variant = ResonanceServerAccess.get_server()
	_reload_static_scenes_from_tree(srv, tree.get_root())
	# Same frame: re-register probe batches after reinit (deferred was one frame late). Baked-only needs batches
	# before RunReflections so parametric/hybrid/pathing outputs are valid (see load_probe_batch heavy request).
	for n in tree.get_nodes_in_group("resonance_probe_volume"):
		if n.has_method("reload_probe_batch"):
			n.reload_probe_batch()
	tree.call_group_flags(SceneTree.GROUP_CALL_DEFERRED, "resonance_geometry", "refresh_geometry")
	call_deferred("_deferred_reset_spatial_audio_warmup_passes")
	_sync_physics_process_for_custom_tracer()


## Connects to `ResonanceRuntimeConfig` change signals so live tweaks (settings menus,
## debug overlays, GDScript scripting, etc.) trigger an audio engine reinit. The connect
## path runs in BOTH editor and play mode; `_disconnect_runtime_signals` below is also
## play-mode-active, so symmetry is required to avoid stuck connections after reinit.
func _connect_runtime_signals() -> void:
	if not _runtime:
		return
	if (
		_runtime.has_signal("reflection_type_changed")
		and not _runtime.reflection_type_changed.is_connected(_on_reflection_type_changed)
	):
		_runtime.reflection_type_changed.connect(_on_reflection_type_changed)
	if (
		_runtime.has_signal("pathing_enabled_changed")
		and not _runtime.pathing_enabled_changed.is_connected(_on_runtime_affecting_probes_changed)
	):
		_runtime.pathing_enabled_changed.connect(_on_runtime_affecting_probes_changed)
	if (
		_runtime.has_signal("audio_frame_size_changed")
		and not _runtime.audio_frame_size_changed.is_connected(_on_audio_frame_size_changed)
	):
		_runtime.audio_frame_size_changed.connect(_on_audio_frame_size_changed)


func _disconnect_runtime_signals() -> void:
	if not _runtime:
		return
	if (
		_runtime.has_signal("reflection_type_changed")
		and _runtime.reflection_type_changed.is_connected(_on_reflection_type_changed)
	):
		_runtime.reflection_type_changed.disconnect(_on_reflection_type_changed)
	if (
		_runtime.has_signal("pathing_enabled_changed")
		and _runtime.pathing_enabled_changed.is_connected(_on_runtime_affecting_probes_changed)
	):
		_runtime.pathing_enabled_changed.disconnect(_on_runtime_affecting_probes_changed)
	if (
		_runtime.has_signal("audio_frame_size_changed")
		and _runtime.audio_frame_size_changed.is_connected(_on_audio_frame_size_changed)
	):
		_runtime.audio_frame_size_changed.disconnect(_on_audio_frame_size_changed)


func _on_reflection_type_changed(_arg = null) -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return
	var cfg = get_config_dict()
	if cfg.is_empty():
		return
	_prepare_geometry_before_reinit()
	srv.reinit_audio_engine(cfg)
	call_deferred("_reload_after_reinit")
	_notify_volumes_runtime_config_changed()


func _on_audio_frame_size_changed(_arg = null) -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null:
		return
	var cfg = get_config_dict()
	if cfg.is_empty():
		return
	_prepare_geometry_before_reinit()
	srv.reinit_audio_engine(cfg)
	call_deferred("_reload_after_reinit")
	_notify_volumes_runtime_config_changed()


func _on_runtime_affecting_probes_changed(_arg = null) -> void:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null:
		srv.set_pathing_enabled(_runtime.pathing_enabled)
		var removed = srv.revalidate_probe_batches_with_config()
		if removed > 0 and is_inside_tree():
			get_tree().call_group_flags(
				SceneTree.GROUP_CALL_DEFERRED, "resonance_probe_volume", "reload_probe_batch"
			)
	_notify_volumes_runtime_config_changed()


func _notify_volumes_runtime_config_changed() -> void:
	if not is_inside_tree() or not _runtime:
		return
	var tree = get_tree()
	var refl: int = _runtime.reflection_type
	var pathing: bool = _runtime.pathing_enabled
	tree.call_group_flags(
		SceneTree.GROUP_CALL_DEFERRED,
		"resonance_probe_volume",
		"notify_runtime_config_changed",
		refl,
		pathing
	)


func _warn_restart_if_needed() -> void:
	if Engine.is_editor_hint():
		return
	if ResonanceServerAccess.get_server_if_initialized() != null:
		print_rich(
			"[color=yellow][Nexus Resonance] Change requires game restart to take effect.[/color]"
		)


func _nexus_perf_uses_full_frame_timing() -> bool:
	return _performance_custom_monitors == ResonanceRuntimePerfMonitors.PERF_MONITORS_FULL


func _refresh_performance_custom_monitors_if_ready() -> void:
	if Engine.is_editor_hint():
		return
	if not is_inside_tree():
		return
	_unregister_nexus_performance_monitors()
	if _performance_custom_monitors != ResonanceRuntimePerfMonitors.PERF_MONITORS_OFF:
		ResonanceRuntimePerfMonitors.register(self, _performance_custom_monitors)


func _unregister_nexus_performance_monitors() -> void:
	ResonanceRuntimePerfMonitors.unregister_all()


func _nexus_perf_read_main_usec() -> int:
	return main_thread_last_tick_usec


func _nexus_perf_read_physics_tick_usec() -> int:
	return runtime_physics_tick_usec


func _nexus_perf_read_main_viewport_usec() -> int:
	return main_thread_viewport_usec


func _nexus_perf_read_main_tick_usec() -> int:
	return main_thread_tick_usec


func _nexus_perf_read_main_flush_usec() -> int:
	return main_thread_flush_usec


func _nexus_perf_read_physics_viewport_usec() -> int:
	return runtime_physics_viewport_usec


func _nexus_perf_read_physics_server_tick_usec() -> int:
	return runtime_physics_server_tick_usec


func _nexus_perf_read_physics_flush_usec() -> int:
	return runtime_physics_flush_usec


func _nexus_perf_read_worker_sum() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_simulation_worker_timing"):
		return 0
	var w: Dictionary = srv.get_simulation_worker_timing()
	return ResonanceRuntimePerfMonitors.simulation_worker_timing_sum(w)


func _nexus_perf_read_worker_timing_field(field: String) -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_simulation_worker_timing"):
		return 0
	var w: Dictionary = srv.get_simulation_worker_timing()
	var v: Variant = w.get(field, 0)
	if v is bool:
		return 1 if v else 0
	return int(v)


func _nexus_perf_read_pathing_ran_tick() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_pathing_instrumentation"):
		return 0
	var p: Dictionary = srv.get_pathing_instrumentation()
	return 1 if p.get("pathing_ran_this_tick", false) else 0


func _nexus_perf_read_convolution_apply_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_reflection_apply_last", 0))


func _nexus_perf_read_convolution_reverb_bus_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_reverb_bus_last", 0))


func _nexus_perf_read_mixer_sanitize_ambi_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_mixer_sanitize_ambi_last", 0))


func _nexus_perf_read_mixer_sanitize_stereo_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_mixer_sanitize_stereo_last", 0))


## Re-scan all living [ResonancePlayer]s and refresh the cached audio-thread aggregates. Throttled to at most
## [constant AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC] to avoid iterating the scene every frame — the aggregates feed
## ~Hz-rate Performance monitors, so higher update rates have no UI benefit.
func _sample_audio_aggregates_if_due() -> void:
	var now_sec := float(Time.get_ticks_msec()) * 0.001
	if (
		_audio_agg_last_sample_sec >= 0.0
		and (now_sec - _audio_agg_last_sample_sec) < AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC
	):
		return
	_audio_agg_last_sample_sec = now_sec
	var tree := get_tree()
	if tree == null:
		_audio_agg_output_underruns_total = 0
		_audio_agg_late_mix_total = 0
		_audio_agg_max_block_time_us = 0
		_audio_agg_voice_count = 0
		return
	var sum_underrun: int = 0
	var sum_late_mix: int = 0
	var max_block: int = 0
	var sum_voices: int = 0
	for p in tree.get_nodes_in_group("resonance_player"):
		if p == null or not p.has_method("get_audio_instrumentation"):
			continue
		var d: Dictionary = p.get_audio_instrumentation()
		if d.is_empty():
			continue
		sum_underrun += int(d.get("output_underrun", 0))
		sum_late_mix += int(d.get("late_mix_count", 0))
		var mb: int = int(d.get("max_block_time_us", 0))
		if mb > max_block:
			max_block = mb
		sum_voices += int(d.get("polyphony_voice_count", 0))
	_audio_agg_output_underruns_total = sum_underrun
	_audio_agg_late_mix_total = sum_late_mix
	_audio_agg_max_block_time_us = max_block
	_audio_agg_voice_count = sum_voices


func _nexus_perf_read_audio_output_underruns_total() -> int:
	return _audio_agg_output_underruns_total


func _nexus_perf_read_audio_late_mix_total() -> int:
	return _audio_agg_late_mix_total


func _nexus_perf_read_audio_max_block_time_us() -> int:
	return _audio_agg_max_block_time_us


func _nexus_perf_read_audio_active_voice_count() -> int:
	return _audio_agg_voice_count


func _nexus_perf_read_server_active_source_count() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_active_source_count"):
		return 0
	return int(srv.get_active_source_count())


func _nexus_perf_read_server_active_probe_batch_count() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_active_probe_batch_count"):
		return 0
	return int(srv.get_active_probe_batch_count())


func _update_debug_overlay_visibility() -> void:
	var overlay = get_node_or_null("DebugOverlay")
	if overlay:
		overlay.visible = _debug_overlay_visible
