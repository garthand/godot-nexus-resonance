extends SceneTree

# Same as run_animation_audio_resonance_smoke.gd but native AudioStreamPlayer3D (no ResonanceStream wrap).

const ANIM_NAME := &"anim_audio_test"


func _initialize() -> void:
	var root := Node3D.new()
	get_root().add_child(root)

	var player := AudioStreamPlayer3D.new()
	player.name = "AudioPlayer3D"

	var stream: Resource = load("res://Examples/audio/Phone.mp3")
	if stream == null:
		push_error("Failed to load test stream")
		quit(1)
		return

	root.add_child(player)

	var ap := AnimationPlayer.new()
	root.add_child(ap)

	var anim := Animation.new()
	anim.length = 2.0
	var track_i := anim.add_track(Animation.TYPE_AUDIO)
	anim.track_set_path(track_i, NodePath("AudioPlayer3D"))
	anim.audio_track_insert_key(track_i, 0.05, stream, 0.0, 0.0)

	var lib := AnimationLibrary.new()
	lib.add_animation(ANIM_NAME, anim)
	ap.add_animation_library(&"", lib)

	call_deferred("_run", ap)


func _run(ap: AnimationPlayer) -> void:
	for _i in range(5):
		await process_frame
	ap.play(ANIM_NAME)
	for _i in range(60):
		await process_frame
	ap.stop()
	quit(0)
