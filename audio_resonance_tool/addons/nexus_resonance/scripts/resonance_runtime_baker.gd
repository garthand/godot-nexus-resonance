extends RefCounted
class_name ResonanceRuntimeBaker

## Public API for baking Nexus Resonance acoustics at runtime.
## This class is designed to be used in exported games, procedural generation,
## or dynamic level pipelines where the Godot Editor UI is not available.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")

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


## Releases the native probe batches for the given volumes from the Steam Audio simulator.
## Leaves the probe_data resource on each volume untouched; call this when you want to free
## simulator memory but keep the data around for a possible later reload_probe_batch().
## Volumes without [code]release_probe_batch[/code] (e.g. plain Nodes) are silently skipped.
func flush_volumes(volumes: Array[Node]) -> void:
	for vol in volumes:
		if vol and vol.has_method("release_probe_batch"):
			vol.release_probe_batch()


## Walks the scene under [code]scene_root[/code] and flushes every ResonanceProbeVolume.
## Convenience for level teardown / streaming / "start fresh before re-bake".
func flush_all_runtime_bakes(scene_root: Node) -> void:
	if not scene_root:
		return
	var collected: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(scene_root, collected)
	flush_volumes(collected)


## Reloads (re-registers) probe batches for the given volumes by calling [code]reload_probe_batch[/code].
## Useful after [code]flush_volumes[/code], or when you updated probe_data in RAM and want the native
## simulator registration refreshed. Volumes without [code]reload_probe_batch[/code] are skipped.
func reload_volumes(volumes: Array[Node]) -> void:
	for vol in volumes:
		if vol and vol.has_method("reload_probe_batch"):
			vol.reload_probe_batch()


## Walks the scene under [code]scene_root[/code] and calls [code]reload_probe_batch[/code] on every
## ResonanceProbeVolume found. Convenience for \"re-register everything\" flows.
func reload_all_runtime_bakes(scene_root: Node) -> void:
	if not scene_root:
		return
	var collected: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(scene_root, collected)
	reload_volumes(collected)


## Cleans up the runner and safely breaks reference cycles.
func shutdown() -> void:
	if _runner and _runner.has_method("shutdown"):
		_runner.shutdown()
	_runner = null
