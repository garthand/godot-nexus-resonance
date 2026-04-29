extends GutTest

## Unit tests for ResonanceBakeRunner and ResonanceBakeEstimates (probe/time estimates).
## EditorInterface-dependent paths are not tested here.

const BakeConfigScript = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const BakeEstimates = preload("res://addons/nexus_resonance/editor/resonance_bake_estimates.gd")
const BakeRunnerScript = preload("res://addons/nexus_resonance/editor/resonance_bake_runner.gd")


class MockProbeVolume:
	extends Node
	var region_size := Vector3(10.0, 2.0, 10.0)
	var spacing := 1.0
	var generation_type := 1  # Floor


class MockProbeVolumeOctree:
	extends Node
	var region_size := Vector3(4.0, 4.0, 4.0)
	var spacing := 1.0
	var generation_type := 0  # Octree


func test_estimate_probe_count_null_returns_minus_one():
	var runner = BakeRunnerScript.new(null)
	var result = runner.estimate_probe_count(null)
	assert_eq(result, -1, "null volume should return -1")


func test_estimate_probe_count_floor_mode():
	var runner = BakeRunnerScript.new(null)
	var vol = MockProbeVolume.new()
	var result = runner.estimate_probe_count(vol)
	vol.free()
	# Floor: extents 5,5,5 -> ceil(10)*ceil(10) = 100 probes
	assert_gte(result, 1, "floor mode should return positive probe count")
	assert_eq(result, 100, "10x10 floor with spacing 1 should give 100 probes")


func test_estimate_probe_count_octree_mode():
	var runner = BakeRunnerScript.new(null)
	var vol = MockProbeVolumeOctree.new()
	var result = runner.estimate_probe_count(vol)
	vol.free()
	# Octree: extents 2,2,2 -> ceil(4)*ceil(4)*ceil(4) = 64
	assert_gte(result, 1, "octree mode should return positive probe count")
	assert_eq(result, 64, "4x4x4 octree with spacing 1 should give 64 probes")


func test_estimate_probe_count_zero_spacing_returns_minus_one():
	var runner = BakeRunnerScript.new(null)
	var vol = MockProbeVolume.new()
	vol.spacing = 0.0
	var result = runner.estimate_probe_count(vol)
	vol.free()
	assert_eq(result, -1, "zero spacing should return -1")


func test_estimate_probe_count_node_without_properties_returns_minus_one():
	var runner = BakeRunnerScript.new(null)
	var plain_node = Node.new()
	var result = runner.estimate_probe_count(plain_node)
	plain_node.free()
	assert_eq(result, -1, "node without region_size/spacing should return -1")


func test_estimate_bake_time_returns_non_empty_for_valid_volume():
	var runner = BakeRunnerScript.new(null)
	var vol = MockProbeVolume.new()
	var result = runner.estimate_bake_time(vol)
	vol.free()
	assert_false(
		result.is_empty(), "estimate_bake_time should return non-empty string for valid volume"
	)
	assert_true(result.begins_with("~"), "estimate should start with ~")


func test_estimate_bake_time_null_returns_empty():
	var runner = BakeRunnerScript.new(null)
	var result = runner.estimate_bake_time(null)
	assert_true(result.is_empty(), "null volume should return empty string")


func test_resonance_bake_estimates_static_matches_runner_probe_count():
	var vol = MockProbeVolume.new()
	var from_static = BakeEstimates.estimate_probe_count(vol)
	var runner = BakeRunnerScript.new(null)
	var from_runner = runner.estimate_probe_count(vol)
	vol.free()
	assert_eq(from_static, from_runner, "static estimates should match runner delegation")


func test_resonance_bake_estimates_bake_time_with_config():
	var vol = MockProbeVolume.new()
	var bc = BakeConfigScript.create_default()
	var s = BakeEstimates.estimate_bake_time(vol, bc)
	vol.free()
	assert_false(s.is_empty(), "BakeEstimates.estimate_bake_time should return text")
	assert_true(s.begins_with("~"), "estimate should start with ~")
