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
