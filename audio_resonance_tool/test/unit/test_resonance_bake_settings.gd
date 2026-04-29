extends GutTest

## Unit tests for ResonanceBakeConfig (per-volume bake resource).


func test_get_bake_params_has_expected_keys():
	var bc = ResonanceBakeConfig.create_default()
	var params = bc.get_bake_params()
	var expected = [
		"bake_ambisonics_order",
		"bake_num_rays",
		"bake_num_bounces",
		"bake_num_threads",
		"bake_reflection_type",
		"bake_pathing_vis_range",
		"bake_pathing_path_range",
		"bake_pathing_num_samples",
		"bake_pathing_radius",
		"bake_pathing_threshold"
	]
	for key in expected:
		assert_has(params, key, "get_bake_params missing: " + key)
	assert_eq(params.size(), expected.size(), "get_bake_params should have exactly expected keys")


func test_bake_defaults_are_sensible():
	var bc = ResonanceBakeConfig.create_default()
	assert_gte(bc.bake_num_rays, 256, "bake_num_rays should be >= 256")
	assert_lte(bc.bake_num_rays, 16384, "bake_num_rays should be <= 16384")
	assert_gte(bc.bake_num_bounces, 1, "bake_num_bounces should be >= 1")
	assert_lte(bc.bake_num_bounces, 32, "bake_num_bounces should be <= 32")
	assert_gte(bc.reflection_type, 0, "reflection_type should be 0-2")
	assert_lte(bc.reflection_type, 2, "reflection_type should be 0-2")
	assert_gte(bc.bake_ambisonics_order, 1, "bake_ambisonics_order should be >= 1")
	assert_lte(bc.bake_ambisonics_order, 3, "bake_ambisonics_order should be <= 3")


func test_get_bake_params_applies_properties():
	var bc = ResonanceBakeConfig.create_default()
	bc.bake_num_rays = 8192
	bc.bake_ambisonics_order = 2
	bc.pathing_enabled = true
	bc.bake_pathing_radius = 0.8
	var params = bc.get_bake_params()
	assert_eq(params.get("bake_num_rays", -1), 8192, "bake_num_rays should be applied")
	assert_eq(params.get("bake_ambisonics_order", -1), 2, "bake_ambisonics_order should be applied")
	assert_eq(params.get("bake_pathing_radius", -1.0), 0.8, "bake_pathing_radius should be applied")


func test_create_default_returns_valid_config():
	var bc = ResonanceBakeConfig.create_default()
	assert_not_null(bc, "create_default should return non-null")
	assert_true(bc is ResonanceBakeConfig, "create_default should return ResonanceBakeConfig")


# --- pathing_params_hash: must match C++ ResonanceBaker::bake_pathing and GDScript _compute_pathing_hash ---


static func _pathing_hash_from_params(params: Dictionary) -> int:
	return hash(
		var_to_str(
			{
				"vis_range": params.get("bake_pathing_vis_range", 500),
				"path_range": params.get("bake_pathing_path_range", 100),
				"num_samples": params.get("bake_pathing_num_samples", 16),
				"radius": params.get("bake_pathing_radius", 0.5),
				"threshold": params.get("bake_pathing_threshold", 0.1)
			}
		)
	)


func test_pathing_hash_deterministic():
	var bc = ResonanceBakeConfig.create_default()
	var params = bc.get_bake_params()
	var h1 = _pathing_hash_from_params(params)
	var h2 = _pathing_hash_from_params(params)
	assert_eq(h1, h2, "pathing hash should be deterministic")
	assert_ne(h1, 0, "pathing hash for default params should be non-zero")


func test_pathing_hash_changes_with_params():
	var bc = ResonanceBakeConfig.create_default()
	bc.pathing_enabled = true
	var params = bc.get_bake_params()
	var h_default = _pathing_hash_from_params(params)
	bc.bake_pathing_vis_range = 600.0
	var params_modified = bc.get_bake_params()
	var h_modified = _pathing_hash_from_params(params_modified)
	assert_ne(h_default, h_modified, "different pathing params should produce different hash")


## Ensures ResonanceBakeHashes.compute_pathing_hash matches the dict format used by C++ ResonanceBaker::bake_pathing.
## Both must use hash(var_to_str({vis_range, path_range, num_samples, radius, threshold})) for consistency.
func test_pathing_hash_bake_runner_matches_cpp_format():
	const BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")
	var bc = ResonanceBakeConfig.create_default()
	bc.pathing_enabled = true
	var runner_hash = BakeHashes.compute_pathing_hash(bc)
	var manual_hash = _pathing_hash_from_params(bc.get_bake_params())
	assert_eq(
		runner_hash,
		manual_hash,
		"BakeHashes must match C++ dict format (vis_range, path_range, num_samples, radius, threshold)"
	)
	bc.bake_pathing_threshold = 0.05
	runner_hash = BakeHashes.compute_pathing_hash(bc)
	manual_hash = _pathing_hash_from_params(bc.get_bake_params())
	assert_eq(runner_hash, manual_hash, "BakeHashes must match for modified params too")
