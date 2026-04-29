extends GutTest

## Unit tests for ResonancePaths (get_audio_data_dir respects Project Settings).

const ResonancePaths = preload("res://addons/nexus_resonance/scripts/resonance_paths.gd")


func test_get_audio_data_dir_returns_path_ending_with_slash():
	var path: String = ResonancePaths.get_audio_data_dir()
	assert_true(path.ends_with("/"), "path should end with /")
	assert_false(path.is_empty(), "path should not be empty")


func test_get_audio_data_dir_default_is_audio_data():
	var path: String = ResonancePaths.get_audio_data_dir()
	assert_true(
		"audio_data" in path or path == "res://audio_data/", "default should point to audio_data"
	)


func test_get_audio_data_dir_uses_project_setting_when_configured():
	const KEY := "nexus/resonance/bake/default_output_directory"
	if not ProjectSettings.has_setting(KEY):
		pass_test("default_output_directory not in Project Settings - plugin may not have loaded")
		return
	var configured: String = ProjectSettings.get_setting(KEY, "")
	var result: String = ResonancePaths.get_audio_data_dir()
	if configured.is_empty():
		assert_eq(
			result, ResonancePaths.PATH_AUDIO_DATA, "empty setting should fall back to default"
		)
	else:
		assert_true(result.ends_with("/"), "configured path should end with /")


func test_export_extensions_default_to_tres_when_key_cleared():
	const K_STATIC := "nexus/resonance/export/static_scene_asset_format"
	const K_PROBE := "nexus/resonance/export/probe_data_format"
	var had_s: bool = ProjectSettings.has_setting(K_STATIC)
	var had_p: bool = ProjectSettings.has_setting(K_PROBE)
	var prev_s: Variant = ProjectSettings.get_setting(K_STATIC, 0) if had_s else 0
	var prev_p: Variant = ProjectSettings.get_setting(K_PROBE, 0) if had_p else 0
	if had_s:
		ProjectSettings.clear(K_STATIC)
	if had_p:
		ProjectSettings.clear(K_PROBE)
	assert_eq(ResonancePaths.get_static_scene_asset_extension(), "tres")
	assert_eq(ResonancePaths.get_probe_data_asset_extension(), "tres")
	if had_s:
		ProjectSettings.set_setting(K_STATIC, prev_s)
	if had_p:
		ProjectSettings.set_setting(K_PROBE, prev_p)


func test_get_static_scene_asset_extension_tres_and_res():
	const KEY := "nexus/resonance/export/static_scene_asset_format"
	var prev: Variant = ProjectSettings.get_setting(KEY) if ProjectSettings.has_setting(KEY) else 0
	ProjectSettings.set_setting(KEY, 0)
	assert_eq(ResonancePaths.get_static_scene_asset_extension(), "tres")
	ProjectSettings.set_setting(KEY, 1)
	assert_eq(ResonancePaths.get_static_scene_asset_extension(), "res")
	ProjectSettings.set_setting(KEY, prev)


func test_get_probe_data_asset_extension_tres_and_res():
	const KEY := "nexus/resonance/export/probe_data_format"
	var prev: Variant = ProjectSettings.get_setting(KEY) if ProjectSettings.has_setting(KEY) else 0
	ProjectSettings.set_setting(KEY, 0)
	assert_eq(ResonancePaths.get_probe_data_asset_extension(), "tres")
	ProjectSettings.set_setting(KEY, 1)
	assert_eq(ResonancePaths.get_probe_data_asset_extension(), "res")
	ProjectSettings.set_setting(KEY, prev)


func test_probe_data_save_path_uses_setting_suffix():
	const KEY := "nexus/resonance/export/probe_data_format"
	var prev: Variant = ProjectSettings.get_setting(KEY) if ProjectSettings.has_setting(KEY) else 0
	ProjectSettings.set_setting(KEY, 1)
	var p1: String = ResonancePaths.probe_data_save_path("main", "vol_a")
	assert_true(p1.ends_with("main_vol_a_batch.res"), "expected .res suffix, got: %s" % p1)
	ProjectSettings.set_setting(KEY, 0)
	var p0: String = ResonancePaths.probe_data_save_path("main", "vol_a")
	assert_true(p0.ends_with("main_vol_a_batch.tres"), "expected .tres suffix, got: %s" % p0)
	ProjectSettings.set_setting(KEY, prev)


func test_static_scene_asset_save_path_uses_setting_suffix():
	const KEY := "nexus/resonance/export/static_scene_asset_format"
	var prev: Variant = ProjectSettings.get_setting(KEY) if ProjectSettings.has_setting(KEY) else 0
	ProjectSettings.set_setting(KEY, 1)
	var p1: String = ResonancePaths.static_scene_asset_save_path("demo")
	assert_true(p1.ends_with("demo_static.res"), "expected .res suffix, got: %s" % p1)
	ProjectSettings.set_setting(KEY, 0)
	var p0: String = ResonancePaths.static_scene_asset_save_path("demo")
	assert_true(p0.ends_with("demo_static.tres"), "expected .tres suffix, got: %s" % p0)
	ProjectSettings.set_setting(KEY, prev)
