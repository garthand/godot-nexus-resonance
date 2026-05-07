extends GutTest

## Unit tests for ResonanceProbeDataSaver.
## Requires ResonanceProbeData (GDExtension) to be available.

var _temp_path: String = ""
const ProbeDataSaverScript = preload(
	"res://addons/nexus_resonance/editor/resonance_probe_data_saver.gd"
)


func after_each():
	if _temp_path != "":
		if FileAccess.file_exists(_temp_path):
			DirAccess.remove_absolute(_temp_path)
		if FileAccess.file_exists(_temp_path + ".tmp"):
			DirAccess.remove_absolute(_temp_path + ".tmp")
	_temp_path = ""


func test_recognize_resonance_probe_data():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available (GDExtension not loaded)")
		return
	var probe_data = ClassDB.instantiate("ResonanceProbeData") as Resource
	var saver = ProbeDataSaverScript.new()
	assert_true(saver._recognize(probe_data), "should recognize ResonanceProbeData")


func test_recognize_rejects_other_resources():
	var saver = ProbeDataSaverScript.new()
	var other = Resource.new()
	assert_false(saver._recognize(other), "should not recognize generic Resource")


func test_recognize_rejects_null():
	var saver = ProbeDataSaverScript.new()
	assert_false(saver._recognize(null), "should not recognize null")


func test_get_recognized_extensions():
	var saver = ProbeDataSaverScript.new()
	var exts = saver._get_recognized_extensions(null)
	assert_eq(exts.size(), 1, "should only recognize tres (bak is no longer a save target)")
	assert_true(exts.has("tres"), "should recognize tres")
	assert_false(exts.has("bak"), "must not recognize bak anymore")


func test_save_rejects_bak_path():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var probe_data = ClassDB.instantiate("ResonanceProbeData") as Resource
	probe_data.set("data", PackedByteArray([1, 2, 3]))
	var saver = ProbeDataSaverScript.new()
	var bak_path := "user://gut_test_probe_save_reject.tres.bak"
	var err = saver._save(probe_data, bak_path, 0)
	assert_eq(err, ERR_INVALID_PARAMETER, "saver must refuse .bak destinations")
	assert_false(FileAccess.file_exists(bak_path), "no file should be written for rejected save")


func test_save_writes_valid_tres():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var probe_data = ClassDB.instantiate("ResonanceProbeData") as Resource
	probe_data.set("data", PackedByteArray([1, 2, 3]))
	var saver = ProbeDataSaverScript.new()
	_temp_path = "user://gut_test_probe_save.tres"
	var err = saver._save(probe_data, _temp_path, 0)
	assert_eq(err, OK, "save should succeed")
	assert_file_exists(_temp_path)
	assert_false(
		FileAccess.file_exists(_temp_path + ".tmp"), "atomic save: .tmp file should not remain"
	)
	var content = FileAccess.get_file_as_string(_temp_path)
	assert_string_contains(
		content, '[gd_resource type="ResonanceProbeData"', "should have correct header"
	)
	assert_string_contains(content, "[resource]", "should have resource block")
	assert_string_contains(content, "data = PackedByteArray", "should have data line")
