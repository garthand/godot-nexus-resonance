@tool
extends Node3D
class_name ResonanceFmodEventEmitter

## Optional wrapper for FmodEventEmitter3D when using Nexus Resonance + fmod-gdextension.
## When attached as child of FmodEventEmitter3D, creates Resonance source and syncs position.
##
## Requires: fmod-gdextension, ResonanceFMODBridge initialized.
## Add this as child of FmodEventEmitter3D and assign the event_path.
##
## Limitation: The Steam Audio "Simulation Outputs Handle" DSP parameter cannot yet be
## passed to FMOD events. Requires fmod-gdextension API for DSP parameters
## (e.g. event_instance.setParameterByIndex). Until then: position sync and source creation
## work; spatialization may fall back to FMOD default 3D positioning. See _try_set_fmod_parameter.

@export var event_path: String = "event:/"
@export var auto_play: bool = true

const _RESONANCE_SYNC_POS_EPS := 1e-4

var _resonance_handle: int = -1
var _fmod_handle: int = -1
var _bridge: RefCounted = null
var _fmod_emitter: Node = null
var _event_instance: Object = null  # FMOD EventInstance when available
## Skip [code]update_source[/code] when position unchanged (reduces [code]TIME_PROCESS[/code] with many emitters).
var _last_resonance_sync_pos: Vector3 = Vector3(INF, INF, INF)


func _ready() -> void:
	_fmod_emitter = get_parent()
	if not _is_fmod_emitter(_fmod_emitter):
		push_warning(
			(
				"ResonanceFmodEventEmitter: Parent must be FmodEventEmitter3D. Found: %s"
				% _fmod_emitter.get_class()
			)
		)
		return
	call_deferred("_setup_bridge")
	if Engine.is_editor_hint():
		return
	if auto_play and _fmod_emitter.has_method("play"):
		call_deferred("_on_play")


func _exit_tree() -> void:
	_release_handles()


func _process(_delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if _resonance_handle >= 0 and _bridge and ResonanceServerAccess.has_server():
		var pos := global_position
		if (
			(pos - _last_resonance_sync_pos).length_squared()
			< _RESONANCE_SYNC_POS_EPS * _RESONANCE_SYNC_POS_EPS
		):
			return
		_last_resonance_sync_pos = pos
		var srv = ResonanceServerAccess.get_server()
		srv.update_source(_resonance_handle, pos, 1.0)


func _is_fmod_emitter(node: Node) -> bool:
	if not node:
		return false
	return node.get_class() == "FmodEventEmitter3D"


func _setup_bridge() -> void:
	var tree = get_tree()
	if not tree:
		return
	var runtimes = tree.get_nodes_in_group("resonance_runtime")
	for rt in runtimes:
		if rt.has_method("get_fmod_bridge"):
			var candidate = rt.get_fmod_bridge()
			if candidate and candidate.is_bridge_loaded():
				_bridge = candidate
				return
	if not _bridge:
		push_warning(
			"ResonanceFmodEventEmitter: No ResonanceRuntime with FMOD bridge. Enable fmod_bridge_enabled on ResonanceRuntime."
		)


func _on_play() -> void:
	if not _bridge or not _bridge.is_bridge_loaded():
		return
	if not ResonanceServerAccess.has_server():
		return
	var srv = ResonanceServerAccess.get_server()
	_resonance_handle = srv.create_source_handle(global_position, 1.0)
	if _resonance_handle < 0:
		return
	_last_resonance_sync_pos = global_position
	_fmod_handle = _bridge.add_fmod_source(_resonance_handle)
	if _fmod_handle < 0:
		srv.destroy_source_handle(_resonance_handle)
		_resonance_handle = -1
		_last_resonance_sync_pos = Vector3(INF, INF, INF)
		return
	# Try to pass handle to FMOD event if API allows
	_try_set_fmod_parameter(_fmod_handle)


## TODO: Implement when fmod-gdextension API for DSP parameters is available.
## Pass handle to FMOD event's Steam Audio Spatializer "Simulation Outputs Handle" parameter.
## API may vary; check FMOD Studio docs. Example: event_instance.setParameterByIndex(index, float(handle))
func _try_set_fmod_parameter(_handle: int) -> void:
	pass


func _release_handles() -> void:
	if _bridge and _fmod_handle >= 0:
		_bridge.remove_fmod_source(_fmod_handle)
		_fmod_handle = -1
	if _resonance_handle >= 0 and ResonanceServerAccess.has_server():
		ResonanceServerAccess.get_server().destroy_source_handle(_resonance_handle)
		_resonance_handle = -1
	_last_resonance_sync_pos = Vector3(INF, INF, INF)
