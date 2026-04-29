extends SceneTree

# Headless smoke test:
# - Loads the demo scene
# - Repeatedly plays the "lightning" animation (which includes an audio track targeting ResonancePlayer_Thunder)
# - Quits with non-zero exit code if the scene/animation can't be driven

const DEMO_SCENE := "res://Examples/scenes/demo.tscn"
const ANIM_NAME := "lightning"
const TRIGGERS := 10


func _initialize() -> void:
	var packed: PackedScene = load(DEMO_SCENE)
	if packed == null:
		push_error("Failed to load demo scene: %s" % DEMO_SCENE)
		quit(1)
		return

	var root: Node = packed.instantiate()
	if root == null:
		push_error("Failed to instantiate demo scene: %s" % DEMO_SCENE)
		quit(1)
		return

	get_root().add_child(root)

	# Find an AnimationPlayer that contains the lightning animation.
	var anim_player: AnimationPlayer = null
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		var ap := n as AnimationPlayer
		if ap != null and ap.has_animation(ANIM_NAME):
			anim_player = ap
			break
		for c in n.get_children():
			if c is Node:
				stack.push_back(c)

	if anim_player == null:
		push_error("No AnimationPlayer with animation '%s' found in demo scene." % ANIM_NAME)
		quit(1)
		return

	call_deferred("_run", anim_player)


func _run(anim_player: AnimationPlayer) -> void:
	# Let the scene settle for a moment.
	for _i in range(5):
		await process_frame

	for _t in range(TRIGGERS):
		anim_player.play(ANIM_NAME)
		# Allow a few frames so the audio track triggers reliably.
		for _i in range(8):
			await process_frame

	quit(0)
