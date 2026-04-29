extends RefCounted
class_name ResonanceReverbActivator

## Keeps the reverb bus active via a low-level [AudioStreamGenerator] feed (see [ResonanceRuntime]).

var _player: AudioStreamPlayer
var _frames_pushed: int = 0
var _fill_calls: int = 0
## Cached reverb bus index; refreshed when [member _cached_reverb_bus_name] differs from runtime bus name.
var _cached_reverb_bus_index: int = -1
var _cached_reverb_bus_name: StringName = &""
## When true, [member instrumentation] holds the active success shape; keys updated in place to avoid per-frame Dictionary allocation.
var _instrumentation_shape_ok: bool = false
## Instrumentation for debug overlay ([member ResonanceRuntime.activator_instrumentation] delegates here).
var instrumentation: Dictionary = {}


func setup(parent: Node, bus: ResonanceRuntimeBus) -> bool:
	if not bus.ensure_reverb_bus_exists():
		return false
	var bus_name := bus.get_reverb_bus_name()
	_cached_reverb_bus_name = bus_name
	_cached_reverb_bus_index = AudioServer.get_bus_index(bus_name)
	_player = AudioStreamPlayer.new()
	_player.name = "ResonanceInternalActivator"
	_player.bus = bus_name
	var gen = AudioStreamGenerator.new()
	gen.buffer_length = 0.1
	_player.stream = gen
	parent.add_child(_player)
	_player.play()
	return true


func _invalidate_instrumentation_shape() -> void:
	_instrumentation_shape_ok = false


func _sync_reverb_bus_cache(bus: ResonanceRuntimeBus) -> int:
	var bn := bus.get_reverb_bus_name()
	if bn != _cached_reverb_bus_name:
		_cached_reverb_bus_name = bn
		_cached_reverb_bus_index = AudioServer.get_bus_index(bn)
	return _cached_reverb_bus_index


func fill_buffer(bus: ResonanceRuntimeBus) -> void:
	if not _player or not _player.playing:
		_invalidate_instrumentation_shape()
		instrumentation = {"active": false, "reason": "no_activator_or_not_playing"}
		return
	var playback = _player.get_stream_playback()
	if not playback:
		_invalidate_instrumentation_shape()
		instrumentation = {"active": false, "reason": "no_playback"}
		return
	if not playback is AudioStreamGeneratorPlayback:
		_invalidate_instrumentation_shape()
		instrumentation = {"active": false, "reason": "not_generator"}
		return
	var avail = playback.get_frames_available()
	if avail <= 0:
		# Reuse success dict in place when possible; do not set _instrumentation_shape_ok on a minimal dict
		# (would break the full-key update path on the next successful push).
		if _instrumentation_shape_ok:
			instrumentation["avail_zero_count"] = (
				int(instrumentation.get("avail_zero_count", 0)) + 1
			)
		else:
			var prev_z := 0
			if instrumentation.get("active", false) and instrumentation.has("avail_zero_count"):
				prev_z = int(instrumentation["avail_zero_count"])
			instrumentation = {"active": true, "avail_zero_count": prev_z + 1}
		return
	const AMP := 1e-5
	var to_push = min(avail, 512)
	for i in to_push:
		playback.push_frame(Vector2(AMP, AMP))
	_frames_pushed += to_push
	_fill_calls += 1
	var bus_idx := _sync_reverb_bus_cache(bus)
	var skips = playback.get_skips() if playback.has_method("get_skips") else -1
	var muted := AudioServer.is_bus_mute(bus_idx) if bus_idx >= 0 else true
	if not _instrumentation_shape_ok:
		instrumentation = {
			"active": true,
			"frames_pushed_total": _frames_pushed,
			"fill_calls": _fill_calls,
			"bus_index": bus_idx,
			"bus_muted": muted,
			"skips": skips,
		}
		_instrumentation_shape_ok = true
	else:
		instrumentation["active"] = true
		instrumentation["frames_pushed_total"] = _frames_pushed
		instrumentation["fill_calls"] = _fill_calls
		instrumentation["bus_index"] = bus_idx
		instrumentation["bus_muted"] = muted
		instrumentation["skips"] = skips


## Stops the generator player and frees it immediately. Use [method Node.free], not [method Node.queue_free]:
## on application quit deferred frees may never run, leaving [AudioStreamGeneratorPlayback] in ObjectDB.
func cleanup() -> void:
	if _player == null:
		return
	if _player.playing:
		_player.stop()
	_player.stream = null
	if _player.is_inside_tree():
		var par := _player.get_parent()
		if par:
			par.remove_child(_player)
	_player.free()
	_player = null
	_cached_reverb_bus_index = -1
	_cached_reverb_bus_name = &""
	_invalidate_instrumentation_shape()
	instrumentation = {"active": false, "reason": "cleaned_up"}
