extends RefCounted
class_name ResonanceRuntimeBaker

## Public API for baking Nexus Resonance acoustics at runtime.
## This class is designed to be used in exported games, procedural generation,
## or dynamic level pipelines where the Godot Editor UI is not available.

signal bake_progress_updated(status_message: String)
signal bake_finished

var _runner: ResonanceBakeRunner


func _init() -> void:
	# Instantiate the runner with a null EditorInterface to trigger Headless Mode
	_runner = ResonanceBakeRunner.new(null)

	# Route the internal runner signals up to this public API
	_runner.bake_progress_updated.connect(func(msg: String): bake_progress_updated.emit(msg))
	_runner.bake_finished.connect(func(): bake_finished.emit())


## Bakes the provided volumes and injects the resulting acoustic data directly into RAM.
## This bypasses the ResourceSaver entirely, preventing disk write errors in exported builds.
func bake_volumes_to_ram(volumes: Array[Node], scene_root: Node) -> void:
	if volumes.is_empty() or not scene_root:
		push_error("RUNTIME BAKE ERROR: Missing volumes or scene root.")
		bake_finished.emit()
		return

	# The third parameter (false) tells the Runner to skip saving to the hard drive!
	_runner.run_bake(volumes, scene_root, false)


## Cleans up the runner and safely breaks reference cycles.
func shutdown() -> void:
	if _runner and _runner.has_method("shutdown"):
		_runner.shutdown()
	_runner = null
