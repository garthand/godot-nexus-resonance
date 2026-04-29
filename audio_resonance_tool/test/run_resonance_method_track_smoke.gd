extends SceneTree

# Headless: METHOD track calling play_animation_audio_clip on ResonancePlayer (Steam path, no polyphonic wrapper).

const ANIM_NAME := &"method_audio_test"

func _initialize() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		push_error("ResonancePlayer missing")
		quit(1)
		return

	var root := Node3D.new()
	get_root().add_child(root)

	var player: Node = ClassDB.instantiate("ResonancePlayer")
	player.name = "ResonancePlayer"
	var cfg_script: GDScript = load("res://addons/nexus_resonance/scripts/resonance_player_config.gd") as GDScript
	player.player_config = cfg_script.call("create_default")

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
	var track_i := anim.add_track(Animation.TYPE_METHOD)
	anim.track_set_path(track_i, NodePath("ResonancePlayer"))
	var method_key := {"method": &"play_animation_audio_clip", "args": [stream, 0.0]}
	anim.track_insert_key(track_i, 0.05, method_key)

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
