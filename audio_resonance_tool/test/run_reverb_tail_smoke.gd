extends SceneTree

# Headless smoke test for the v0.9.13 fix that lets the reverb / pathing tail decay after a
# ResonancePlayer stream ends or is stopped explicitly. We reuse Examples/scenes/demo.tscn
# because it is the only headless-runnable scene with a fully-initialised ResonanceServer
# (geometry, probes, listener) - which is required for ResonancePlayer parameter pushes to
# fire. Two scenarios are checked:
#   1. Natural completion: trigger the lightning animation (one-shot via method-track), wait
#      until the dry signal ends, then verify mix_calls keep advancing during the tail window.
#   2. Explicit stop(): stop the same player mid-playback, then verify mix_calls keep advancing
#      and the tail-drain branch (zero_input_count) was hit.
#
# Both checks rely on ResonancePlayer.get_audio_instrumentation():
#   - blocks_processed / mix_calls confirm the audio thread is calling _mix.
#   - zero_input_count > 0 confirms we entered the samples_read==0 tail-drain path.

const DEMO_SCENE := "res://Examples/scenes/demo.tscn"
const ANIM_NAME := "lightning"
# Headless Godot does not pace process_frame at real time, but the audio mixer thread
# does run in real time. We therefore poll across a wall-clock budget and yield via
# process_frame between samples so SceneTree continues to dispatch.
const DRY_END_WAIT_MSEC := 8000  # Lightning wav is ~3.7s; 8s wall clock leaves headroom.
const TAIL_OBSERVATION_MSEC := 2000  # Watch tail-drain for ~2s after dry end / stop.

var _exit_code: int = 0


func _initialize() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		push_error("[reverb_tail_smoke] ResonancePlayer class missing - GDExtension not loaded.")
		quit(1)
		return
	if not ResourceLoader.exists(DEMO_SCENE):
		print(
			(
				"[reverb_tail_smoke] SKIP: demo scene missing (repo-safe smoke). Missing: %s"
				% DEMO_SCENE
			)
		)
		quit(0)
		return
	call_deferred("_run_all")


func _run_all() -> void:
	_exit_code = 0
	await _run_case()
	quit(_exit_code)


func _await_frames(n: int) -> void:
	for _i in range(max(1, n)):
		await process_frame


func _await_msec(msec: int) -> void:
	# Yield process_frame periodically while real time elapses, so the audio thread
	# (which runs in real time even headlessly) gets to fill its rings and the SceneTree
	# keeps dispatching frame callbacks.
	var deadline_us: int = Time.get_ticks_usec() + max(0, msec) * 1000
	while Time.get_ticks_usec() < deadline_us:
		await process_frame
		OS.delay_msec(2)


func _find_animation_player(root: Node) -> AnimationPlayer:
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		var ap := n as AnimationPlayer
		if ap != null and ap.has_animation(ANIM_NAME):
			return ap
		for c in n.get_children():
			if c is Node:
				stack.push_back(c)
	return null


func _find_resonance_player_sibling_of(ap: AnimationPlayer) -> Node:
	# Lightning animation tracks reference ResonancePlayer_Thunder via a relative NodePath,
	# so the player must live next to the AnimationPlayer in the scene tree.
	if ap == null or ap.get_parent() == null:
		return null
	for c in ap.get_parent().get_children():
		if c.get_class() == "ResonancePlayer":
			return c
	return null


func _read_instr(player: Node) -> Dictionary:
	if player == null or not is_instance_valid(player):
		return {}
	if not player.has_method("get_audio_instrumentation"):
		return {}
	var d: Variant = player.call("get_audio_instrumentation")
	return d if d is Dictionary else {}


func _wait_for_blocks(player: Node, msec_budget: int) -> bool:
	var deadline_us: int = Time.get_ticks_usec() + max(0, msec_budget) * 1000
	while Time.get_ticks_usec() < deadline_us:
		await process_frame
		OS.delay_msec(2)
		if int(_read_instr(player).get("blocks_processed", 0)) > 0:
			return true
	return false


func _wait_for_zero_input(player: Node, msec_budget: int) -> bool:
	var deadline_us: int = Time.get_ticks_usec() + max(0, msec_budget) * 1000
	while Time.get_ticks_usec() < deadline_us:
		await process_frame
		OS.delay_msec(2)
		if int(_read_instr(player).get("zero_input_count", 0)) > 0:
			return true
	return false


func _run_case() -> void:
	var packed: PackedScene = load(DEMO_SCENE)
	if packed == null:
		push_error("[reverb_tail_smoke] Failed to load demo scene.")
		_exit_code = 1
		return
	var root: Node = packed.instantiate()
	if root == null:
		push_error("[reverb_tail_smoke] Failed to instantiate demo scene.")
		_exit_code = 1
		return
	get_root().add_child(root)

	# Let the runtime / probe data settle before triggering audio.
	await _await_msec(500)

	var ap := _find_animation_player(root)
	if ap == null:
		push_error(
			"[reverb_tail_smoke] No AnimationPlayer with '%s' animation in demo." % ANIM_NAME
		)
		_exit_code = 1
		root.queue_free()
		return

	var thunder := _find_resonance_player_sibling_of(ap)
	if thunder == null:
		push_error("[reverb_tail_smoke] No ResonancePlayer sibling for lightning AnimationPlayer.")
		_exit_code = 1
		root.queue_free()
		return

	# --- Case 1: natural one-shot completion ---
	print("[reverb_tail_smoke] case 1: natural one-shot completion")
	if thunder.has_method("reset_audio_instrumentation"):
		thunder.call("reset_audio_instrumentation")
	ap.play(ANIM_NAME)

	if not await _wait_for_blocks(thunder, 4000):
		push_error(
			"[reverb_tail_smoke] case 1 FAILED: no audio blocks processed (audio thread idle)."
		)
		_exit_code = 1
		root.queue_free()
		return

	# Wait for the dry signal to finish (zero_input_count fires when samples_read==0).
	if not await _wait_for_zero_input(thunder, DRY_END_WAIT_MSEC):
		push_error(
			"[reverb_tail_smoke] case 1 FAILED: tail-drain branch never hit (zero_input_count=0 after dry end)."
		)
		_exit_code = 1
		root.queue_free()
		return

	var instr_at_dry_end := _read_instr(thunder)
	var mix_at_dry_end: int = int(instr_at_dry_end.get("mix_calls", 0))
	var blocks_at_dry_end: int = int(instr_at_dry_end.get("blocks_processed", 0))
	var zero_at_dry_end: int = int(instr_at_dry_end.get("zero_input_count", 0))

	# Tail observation window: mix_calls must keep advancing while the playback drains its tail.
	await _await_msec(TAIL_OBSERVATION_MSEC)
	var instr_after_tail := _read_instr(thunder)
	var mix_after_tail: int = int(instr_after_tail.get("mix_calls", 0))
	var blocks_after_tail: int = int(instr_after_tail.get("blocks_processed", 0))

	if mix_after_tail <= mix_at_dry_end:
		push_error(
			(
				"[reverb_tail_smoke] case 1 FAILED: mix_calls did not advance during tail (before=%d after=%d)."
				% [mix_at_dry_end, mix_after_tail]
			)
		)
		_exit_code = 1
	else:
		print(
			(
				"[reverb_tail_smoke] case 1 OK: zero_input=%d blocks=%d->%d mix_calls=%d->%d"
				% [
					zero_at_dry_end,
					blocks_at_dry_end,
					blocks_after_tail,
					mix_at_dry_end,
					mix_after_tail
				]
			)
		)

	# Wait until case 1's leftover playback voice has fully drained (voice_count drops to 0)
	# so case 2's metrics don't get mixed with the previous voice's residue. The convolution
	# IR can be ~3 s so we allow a generous budget.
	var drain_deadline_us: int = Time.get_ticks_usec() + 8_000_000
	while Time.get_ticks_usec() < drain_deadline_us:
		await process_frame
		OS.delay_msec(2)
		if int(_read_instr(thunder).get("polyphony_voice_count", 0)) == 0:
			break

	# --- Case 2: explicit stop() during playback ---
	# Bypass the AnimationPlayer (its RESET animation also writes to ResonancePlayer.playing,
	# which can issue a competing stop()). The lightning track has already populated the
	# stream property on thunder, so we can drive play() / stop() directly.
	print("[reverb_tail_smoke] case 2: explicit stop() during playback")
	if thunder.has_method("reset_audio_instrumentation"):
		thunder.call("reset_audio_instrumentation")
	ap.stop()
	await _await_msec(100)
	thunder.call("play", 0.0)

	if not await _wait_for_blocks(thunder, 4000):
		push_error("[reverb_tail_smoke] case 2 FAILED: no audio blocks before stop().")
		_exit_code = 1
		root.queue_free()
		return

	# Give the dry signal at least a few hundred ms before stopping so the soft-stop
	# transitions through the tail-drain branch with non-zero residue.
	await _await_msec(500)

	if not thunder.has_method("stop"):
		push_error("[reverb_tail_smoke] case 2 FAILED: ResonancePlayer.stop missing.")
		_exit_code = 1
		root.queue_free()
		return
	thunder.call("stop")

	await _await_msec(50)
	var instr_after_stop := _read_instr(thunder)
	var mix_after_stop: int = int(instr_after_stop.get("mix_calls", 0))

	await _await_msec(TAIL_OBSERVATION_MSEC)
	var instr_after_tail2 := _read_instr(thunder)
	var mix_after_tail2: int = int(instr_after_tail2.get("mix_calls", 0))
	var zero_after_tail2: int = int(instr_after_tail2.get("zero_input_count", 0))

	if mix_after_tail2 <= mix_after_stop:
		push_error(
			(
				"[reverb_tail_smoke] case 2 FAILED: mix_calls did not advance after stop() (before=%d after=%d)."
				% [mix_after_stop, mix_after_tail2]
			)
		)
		_exit_code = 1
	elif zero_after_tail2 <= 0:
		push_error("[reverb_tail_smoke] case 2 FAILED: tail-drain branch never hit after stop().")
		_exit_code = 1
	else:
		print(
			(
				"[reverb_tail_smoke] case 2 OK: mix_calls=%d->%d zero_input=%d"
				% [mix_after_stop, mix_after_tail2, zero_after_tail2]
			)
		)

	root.queue_free()
	await _await_frames(5)
