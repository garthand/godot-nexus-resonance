extends GutTest

## Unit tests for ResonanceRuntimeBaker (instantiation, input handling, signals).
## Full baking flow requires the C++ ResonanceServer and a valid 3D SceneTree;
## tests focus on testable API logic and safe headless execution.

const RuntimeBakerScript = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_baker.gd"
)


func test_baker_instantiation():
	var baker = RuntimeBakerScript.new()
	assert_not_null(baker, "ResonanceRuntimeBaker should instantiate successfully.")
	baker.shutdown()


func test_baker_handles_empty_input_safely():
	var baker = RuntimeBakerScript.new()

	# Watch the signal to ensure it fires even when aborting due to empty input.
	watch_signals(baker)
	baker.bake_volumes_to_ram([], null)
	assert_push_error("Missing volumes or scene root")

	assert_signal_emitted(
		baker, "bake_finished", "Baker should emit bake_finished when aborting empty inputs."
	)
	baker.shutdown()


# --- Flush API tests --------------------------------------------------------
#
# The flush API routes through ResonanceProbeVolume.release_probe_batch(). MockProbeVolume
# counts release calls so the tests stay headless without the native ResonanceServer.
#
# Note: ResonanceSceneUtils.collect_resonance_probe_volumes uses node.is_class("ResonanceProbeVolume")
# to detect volumes. is_class is an engine-native check and cannot be overridden from GDScript,
# so flush_all_runtime_bakes is only smoke-tested here (no real volume traversal). The scene
# traversal itself is covered by test_resonance_scene_utils.gd. The flush_volumes call path
# (the substantive logic) is fully exercised below with mocks.


class MockProbeVolume:
	extends Node
	var _release_call_count: int = 0
	var _reload_call_count: int = 0

	func release_probe_batch() -> void:
		_release_call_count += 1

	func reload_probe_batch() -> void:
		_reload_call_count += 1


func test_flush_volumes_calls_release_on_each():
	var baker = RuntimeBakerScript.new()
	var v1 := MockProbeVolume.new()
	var v2 := MockProbeVolume.new()
	var volumes: Array[Node] = [v1, v2]

	baker.flush_volumes(volumes)

	assert_eq(v1._release_call_count, 1, "v1 should be flushed exactly once")
	assert_eq(v2._release_call_count, 1, "v2 should be flushed exactly once")

	v1.free()
	v2.free()
	baker.shutdown()


func test_flush_volumes_skips_volumes_without_method():
	var baker = RuntimeBakerScript.new()
	var v_real := MockProbeVolume.new()
	var v_plain := Node.new()  # no release_probe_batch method
	var mixed: Array[Node] = [v_plain, v_real, null]

	baker.flush_volumes(mixed)

	assert_eq(
		v_real._release_call_count,
		1,
		"plain Nodes / null entries must be skipped silently, real volumes still flushed"
	)

	v_real.free()
	v_plain.free()
	baker.shutdown()


func test_flush_volumes_is_idempotent():
	var baker = RuntimeBakerScript.new()
	var v := MockProbeVolume.new()
	var volumes: Array[Node] = [v]

	baker.flush_volumes(volumes)
	baker.flush_volumes(volumes)

	assert_eq(
		v._release_call_count,
		2,
		"flush_volumes should call release_probe_batch each time (release itself is the no-op safeguard)"
	)

	v.free()
	baker.shutdown()


func test_flush_all_runtime_bakes_handles_null_scene_root():
	var baker = RuntimeBakerScript.new()
	# Should not push errors or crash on a null scene root.
	baker.flush_all_runtime_bakes(null)
	pass_test("flush_all_runtime_bakes(null) returned without error")
	baker.shutdown()


func test_flush_all_runtime_bakes_on_empty_tree():
	var baker = RuntimeBakerScript.new()
	var root := Node.new()
	# A tree with zero ResonanceProbeVolume descendants must complete without errors.
	baker.flush_all_runtime_bakes(root)
	pass_test("flush_all_runtime_bakes on a volume-less tree returned without error")
	root.free()
	baker.shutdown()


# --- Reload helpers tests ---------------------------------------------------


func test_reload_volumes_calls_reload_on_each():
	var baker = RuntimeBakerScript.new()
	var v1 := MockProbeVolume.new()
	var v2 := MockProbeVolume.new()
	var volumes: Array[Node] = [v1, v2]

	baker.reload_volumes(volumes)

	assert_eq(v1._reload_call_count, 1, "v1 should be reloaded exactly once")
	assert_eq(v2._reload_call_count, 1, "v2 should be reloaded exactly once")

	v1.free()
	v2.free()
	baker.shutdown()


func test_reload_volumes_skips_volumes_without_method():
	var baker = RuntimeBakerScript.new()
	var v_real := MockProbeVolume.new()
	var v_plain := Node.new()  # no reload_probe_batch method
	var mixed: Array[Node] = [v_plain, v_real, null]

	baker.reload_volumes(mixed)

	assert_eq(
		v_real._reload_call_count,
		1,
		"plain Nodes / null entries must be skipped silently, real volumes still reloaded"
	)

	v_real.free()
	v_plain.free()
	baker.shutdown()


func test_reload_all_runtime_bakes_handles_null_scene_root():
	var baker = RuntimeBakerScript.new()
	baker.reload_all_runtime_bakes(null)
	pass_test("reload_all_runtime_bakes(null) returned without error")
	baker.shutdown()


func test_reload_all_runtime_bakes_on_empty_tree():
	var baker = RuntimeBakerScript.new()
	var root := Node.new()
	baker.reload_all_runtime_bakes(root)
	pass_test("reload_all_runtime_bakes on a volume-less tree returned without error")
	root.free()
	baker.shutdown()
