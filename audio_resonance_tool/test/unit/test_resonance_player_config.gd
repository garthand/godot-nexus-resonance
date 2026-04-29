extends GutTest

## Unit tests for ResonancePlayerConfig (per-source config resource).


func test_create_default_returns_valid_config():
	var pc = ResonancePlayerConfig.create_default()
	assert_not_null(pc, "create_default should return non-null")
	assert_true(pc is ResonancePlayerConfig, "create_default should return ResonancePlayerConfig")


func test_defaults_are_sensible():
	var pc = ResonancePlayerConfig.create_default()
	assert_gt(pc.min_distance, 0.0, "min_distance should be > 0")
	assert_gt(pc.max_distance, pc.min_distance, "max_distance should be > min_distance")
	assert_gte(pc.source_radius, 0.1, "source_radius should be >= 0.1")
	assert_lte(pc.source_radius, 10.0, "source_radius should be <= 10")


func test_attenuation_mode_valid_range():
	var pc = ResonancePlayerConfig.create_default()
	assert_gte(pc.attenuation_mode, 0, "attenuation_mode should be 0-3")
	assert_lte(pc.attenuation_mode, 3, "attenuation_mode should be 0-3")
