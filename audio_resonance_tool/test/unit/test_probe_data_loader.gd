extends GutTest

## Unit tests for ResonanceProbeDataLoader. Tests _parse_tres_data (returns Dictionary) and public API.

var _loader: ResourceFormatLoader
var _temp_tres_path: String = ""


func before_each():
	_loader = load("res://addons/nexus_resonance/editor/resonance_probe_data_loader.gd").new()


func after_each():
	if _temp_tres_path != "" and FileAccess.file_exists(_temp_tres_path):
		DirAccess.remove_absolute(_temp_tres_path)
	_temp_tres_path = ""


# --- _parse_tres_data tests (returns Dictionary with data, probe_positions, hashes) ---


func test_parse_tres_data_empty_resource_returns_dict_with_empty_data():
	var content = '[gd_resource type="ResonanceProbeData" format=3]\n\n[resource]\n'
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_true(result.data is PackedByteArray, "data should be PackedByteArray")
	assert_eq(result.data.size(), 0, "empty [resource] should have empty data")


func test_parse_tres_data_valid_data_line():
	var content = '[gd_resource type="ResonanceProbeData" format=3]\n\n[resource]\ndata = PackedByteArray(1, 2, 3)'
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_true(result.data is PackedByteArray, "data should be PackedByteArray")
	assert_eq(result.data.size(), 3, "data = PackedByteArray(1,2,3) should parse to 3 bytes")


func test_parse_tres_data_empty_packed_byte_array():
	var content = "[resource]\ndata = PackedByteArray()"
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_eq(result.data.size(), 0, "empty PackedByteArray should parse")


func test_parse_tres_data_resource_before_data_line():
	var content = "[gd_resource]\n[resource]\ndata = PackedByteArray(65, 66)"
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_eq(result.data.size(), 2, "should have 2 bytes")
	assert_eq(result.data[0], 65, "first byte should be 65")


# --- _parse_tres_data edge cases ---


func test_parse_tres_data_null_expr_returns_empty():
	var content = "[resource]\ndata = null"
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_true(result.data is PackedByteArray, "null expr should fallback to PackedByteArray")
	assert_eq(result.data.size(), 0, "null should yield empty array")


func test_parse_tres_data_multiple_resource_blocks_uses_first():
	var content = "[resource]\ndata = PackedByteArray(10)\n[resource]\ndata = PackedByteArray(20, 21)"
	var result = _loader._parse_tres_data(content)
	assert_eq(result.data.size(), 1, "first block should be used")
	assert_eq(result.data[0], 10, "first block data")


func test_parse_tres_data_includes_hash_fields():
	var content = "[resource]\ndata = PackedByteArray()\nbake_params_hash = 42\npathing_params_hash = 7"
	var result = _loader._parse_tres_data(content)
	assert_eq(result.bake_params_hash, 42, "bake_params_hash should parse")
	assert_eq(result.pathing_params_hash, 7, "pathing_params_hash should parse")


func test_parse_tres_data_probe_positions():
	# PackedVector3Array(0,0,0, 1,0,0) = 2 Vector3s
	var content = "[resource]\ndata = PackedByteArray()\nprobe_positions = PackedVector3Array(0, 0, 0, 1, 0, 0)"
	var result = _loader._parse_tres_data(content)
	assert_true(
		result.probe_positions is PackedVector3Array, "probe_positions should be PackedVector3Array"
	)
	assert_eq(result.probe_positions.size(), 2, "probe_positions should have 2 elements")


func test_parse_tres_data_invalid_data_type_fallback():
	var content = '[resource]\ndata = "not_a_byte_array"'
	var result = _loader._parse_tres_data(content)
	assert_true(result is Dictionary, "should return Dictionary")
	assert_true(
		result.data is PackedByteArray, "invalid type should fallback to empty PackedByteArray"
	)
	assert_eq(result.data.size(), 0, "invalid data should yield empty array")


# --- Public API tests ---


func test_get_recognized_extensions():
	var exts = _loader._get_recognized_extensions()
	assert_eq(exts.size(), 2, "should have tres + bak")
	assert_has(exts, "tres", "should recognize tres")
	assert_has(exts, "bak", "should recognize bak")


func test_handles_type_resonance_probe_data():
	assert_true(_loader._handles_type(&"ResonanceProbeData"), "should handle ResonanceProbeData")


func test_handles_type_resource():
	assert_false(_loader._handles_type(&"Resource"), "should not handle generic Resource")


func test_recognize_path_tres_requires_file():
	_temp_tres_path = "user://gut_test_resonance_probe_temp.tres"
	var f = FileAccess.open(_temp_tres_path, FileAccess.WRITE)
	assert_not_null(f, "temp file should be creatable")
	f.store_string(
		'[gd_resource type="ResonanceProbeData" format=3]\n\n[resource]\ndata = PackedByteArray()\n'
	)
	f.close()
	assert_true(
		_loader._recognize_path(_temp_tres_path, &""),
		"should recognize tres with ResonanceProbeData header"
	)


func test_get_resource_type_tres():
	_temp_tres_path = "user://gut_test_resonance_probe_type.tres"
	var f = FileAccess.open(_temp_tres_path, FileAccess.WRITE)
	f.store_string(
		'[gd_resource type="ResonanceProbeData" format=3]\n\n[resource]\ndata = PackedByteArray()\n'
	)
	f.close()
	assert_eq(
		_loader._get_resource_type(_temp_tres_path), "ResonanceProbeData", "tres with valid header"
	)
