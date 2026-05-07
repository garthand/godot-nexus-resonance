extends GutTest

## Unit tests for [ResonanceBakeBackup].
##
## Regression: previously [code]create_backups[/code] used [code]ResourceSaver.save(pd, "...bak")[/code],
## which the custom saver handled by calling [code]take_over_path[/code] on the resource. That bent
## [code]pd.resource_path[/code] to the .bak file, and the next bake would then create
## [code].bak.bak[/code], [code].bak.bak.bak[/code] and so on. These tests pin the new behavior:
## the resource_path stays stable and at most one .bak file ever exists per resource.

const BackupScript = preload("res://addons/nexus_resonance/editor/resonance_bake_backup.gd")

const TEST_RES_PATH := "user://gut_test_bake_backup.tres"
const TEST_BAK_PATH := "user://gut_test_bake_backup.tres.bak"
const TEST_DOUBLE_BAK_PATH := "user://gut_test_bake_backup.tres.bak.bak"


class MockVolume:
	extends Node
	var _probe_data: Resource

	func get_probe_data() -> Resource:
		return _probe_data


func _instantiate_probe_data() -> Resource:
	if not ClassDB.class_exists("ResonanceProbeData"):
		return null
	var pd := ClassDB.instantiate("ResonanceProbeData") as Resource
	pd.set("data", PackedByteArray([1, 2, 3, 4]))
	return pd


func _make_volume_with_data(path: String, payload: PackedByteArray) -> MockVolume:
	var pd := _instantiate_probe_data()
	if not pd:
		return null
	pd.set("data", payload)
	# Save once so a real file exists on disk at [code]path[/code].
	# Saving via ResourceSaver also sets pd.resource_path via the custom saver's take_over_path.
	var err := ResourceSaver.save(pd, path)
	assert_eq(err, OK, "setup: initial save must succeed")
	var vol := MockVolume.new()
	vol._probe_data = pd
	return vol


func _cleanup_paths() -> void:
	for p in [
		TEST_RES_PATH,
		TEST_RES_PATH + ".tmp",
		TEST_RES_PATH + ".uid",
		TEST_BAK_PATH,
		TEST_BAK_PATH + ".uid",
		TEST_DOUBLE_BAK_PATH,
		TEST_DOUBLE_BAK_PATH + ".uid",
	]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(p)


func before_each() -> void:
	_cleanup_paths()


func after_each() -> void:
	_cleanup_paths()


func test_create_backups_does_not_modify_resource_path():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var vol := _make_volume_with_data(TEST_RES_PATH, PackedByteArray([10, 20, 30]))
	assert_not_null(vol, "setup must produce a volume")
	var pd_path_before: String = vol._probe_data.resource_path
	assert_eq(pd_path_before, TEST_RES_PATH, "setup: resource_path should equal save path")

	var backup := BackupScript.new()
	var volumes: Array[Node] = [vol]
	backup.create_backups(volumes)

	assert_eq(
		vol._probe_data.resource_path,
		pd_path_before,
		"resource_path must remain on the canonical path, not be bent to .bak"
	)
	assert_true(backup.has_backups(), "a backup should have been registered")
	assert_true(FileAccess.file_exists(TEST_BAK_PATH), ".bak must exist on disk")
	vol.free()


func test_repeated_create_backups_does_not_chain_bak_extensions():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var vol := _make_volume_with_data(TEST_RES_PATH, PackedByteArray([42]))
	var backup := BackupScript.new()
	var volumes: Array[Node] = [vol]

	for i in range(5):
		backup.create_backups(volumes)
		assert_eq(
			vol._probe_data.resource_path,
			TEST_RES_PATH,
			"iteration %d: resource_path must stay canonical" % i
		)
		assert_false(
			FileAccess.file_exists(TEST_DOUBLE_BAK_PATH),
			"iteration %d: no .bak.bak chain may be created" % i
		)

	assert_true(FileAccess.file_exists(TEST_BAK_PATH), "final .bak should exist after the loop")
	vol.free()


func test_discard_backups_removes_bak_file():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var vol := _make_volume_with_data(TEST_RES_PATH, PackedByteArray([7, 7, 7]))
	var backup := BackupScript.new()
	var volumes: Array[Node] = [vol]
	backup.create_backups(volumes)
	assert_true(FileAccess.file_exists(TEST_BAK_PATH), "precondition: .bak exists")

	backup.discard_backups()

	assert_false(FileAccess.file_exists(TEST_BAK_PATH), ".bak must be deleted")
	assert_true(FileAccess.file_exists(TEST_RES_PATH), "original .tres must NOT be deleted")
	assert_false(backup.has_backups(), "internal backup map should be empty")
	vol.free()


func test_create_backups_self_heals_bak_resource_path():
	# Simulates the legacy bug state: resource_path was bent to ".tres.bak" by an earlier
	# (pre-fix) bake. create_backups must restore it to the canonical path before backing up.
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var vol := _make_volume_with_data(TEST_RES_PATH, PackedByteArray([1, 2]))
	# Forcefully bend resource_path to a .bak file (mimicking the old buggy state).
	vol._probe_data.take_over_path(TEST_BAK_PATH)
	assert_eq(vol._probe_data.resource_path, TEST_BAK_PATH)

	var backup := BackupScript.new()
	var volumes: Array[Node] = [vol]
	backup.create_backups(volumes)

	assert_eq(
		vol._probe_data.resource_path,
		TEST_RES_PATH,
		"create_backups must self-heal a .bak resource_path back to the canonical path"
	)
	assert_false(
		FileAccess.file_exists(TEST_DOUBLE_BAK_PATH),
		"no .bak.bak should ever be created from a self-heal"
	)
	vol.free()


func test_restore_overwrites_resource_and_removes_bak():
	if not ClassDB.class_exists("ResonanceProbeData"):
		pass_test("ResonanceProbeData not available")
		return
	var original_payload := PackedByteArray([100, 101, 102])
	var vol := _make_volume_with_data(TEST_RES_PATH, original_payload)
	var backup := BackupScript.new()
	var volumes: Array[Node] = [vol]
	backup.create_backups(volumes)

	# Mutate the in-memory probe data after the backup, then restore.
	var mutated_payload := PackedByteArray([200, 201])
	vol._probe_data.set("data", mutated_payload)

	var reload_called := [false]
	var complete_called := [false]
	var on_reload := func(_pd: Resource, _vols: Array[Node]) -> void: reload_called[0] = true
	var on_complete := func() -> void: complete_called[0] = true

	backup.restore(volumes, null, on_reload, on_complete)

	assert_true(reload_called[0], "on_reload callback must be invoked")
	assert_true(complete_called[0], "on_complete callback must be invoked")
	assert_eq(
		vol._probe_data.get("data"),
		original_payload,
		"probe data must be restored to the pre-backup payload"
	)
	assert_false(FileAccess.file_exists(TEST_BAK_PATH), ".bak must be removed after restore")
	assert_true(FileAccess.file_exists(TEST_RES_PATH), "canonical file must still exist")
	assert_false(backup.has_backups(), "internal backup map should be empty after restore")
	vol.free()
