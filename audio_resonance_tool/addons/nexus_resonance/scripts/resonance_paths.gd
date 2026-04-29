extends RefCounted
class_name ResonancePaths

## Central path constants for Nexus Resonance. Use for consistency and i18n-prep.
## Use get_audio_data_dir() for the audio output directory (respects Project Settings).

const PATH_AUDIO_DATA := "res://audio_data/"
const PATH_RESONANCE_MESHES := "res://resonance_meshes/"

const _OUTPUT_DIR_SETTING := "nexus/resonance/bake/default_output_directory"
const _STATIC_SCENE_FORMAT_SETTING := "nexus/resonance/export/static_scene_asset_format"
const _PROBE_DATA_FORMAT_SETTING := "nexus/resonance/export/probe_data_format"


static func _export_setting_use_res(setting_key: String) -> bool:
	var v: Variant = ProjectSettings.get_setting(setting_key, 0)
	if v == null:
		return false
	var t := typeof(v)
	if t == TYPE_INT:
		return int(v) == 1
	if t == TYPE_FLOAT:
		return clampi(int(round(v)), 0, 1) == 1
	return false


## Returns the audio data output directory (bake, static export). Reads from Project Settings
## (`default_output_directory`; legacy `bake/output_dir` supported until migrated).
## Falls back to PATH_AUDIO_DATA if not configured. Always returns a path ending with "/".
static func get_audio_data_dir() -> String:
	const LEGACY := "nexus/resonance/bake/output_dir"
	var key := _OUTPUT_DIR_SETTING
	if not ProjectSettings.has_setting(key) and ProjectSettings.has_setting(LEGACY):
		key = LEGACY
	if ProjectSettings.has_setting(key):
		var dir: String = ProjectSettings.get_setting(key, PATH_AUDIO_DATA)
		if not dir.is_empty():
			return dir if dir.ends_with("/") else dir + "/"
	return PATH_AUDIO_DATA


## File extension for exported static scene [ResonanceGeometryAsset] ([code]Export Active Scene[/code]).
## Project setting [code]nexus/resonance/export/static_scene_asset_format[/code]: [code]0[/code] = [code].tres[/code], [code]1[/code] = [code].res[/code].
static func get_static_scene_asset_extension() -> String:
	return "res" if _export_setting_use_res(_STATIC_SCENE_FORMAT_SETTING) else "tres"


## Full [code]res://…[/code] path: [code]audio_data_dir + scene_basename + "_static." + extension[/code].
static func static_scene_asset_save_path(scene_basename: String) -> String:
	return get_audio_data_dir() + scene_basename + "_static." + get_static_scene_asset_extension()


## File extension for baked [ResonanceProbeData] (addon custom [.tres] saver or engine [.res]).
## Project setting [code]nexus/resonance/export/probe_data_format[/code]: [code]0[/code] = [code].tres[/code], [code]1[/code] = [code].res[/code].
static func get_probe_data_asset_extension() -> String:
	return "res" if _export_setting_use_res(_PROBE_DATA_FORMAT_SETTING) else "tres"


## [code]audio_data_dir + scene_basename + "_" + node_key + "_batch." + extension[/code] (matches bake pipeline and C++ editor bake path).
static func probe_data_save_path(scene_basename: String, node_key: String) -> String:
	return (
		get_audio_data_dir()
		+ scene_basename
		+ "_"
		+ node_key
		+ "_batch."
		+ get_probe_data_asset_extension()
	)
