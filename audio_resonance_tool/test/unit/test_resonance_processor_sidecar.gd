extends GutTest

## Sidecar mesh dedupe for Nexus Resonance import (resonance_processor.gd).

const _ResProc := preload("res://addons/nexus_importer/processors/resonance_processor.gd")
const _RID_META := "nexus_resonance_mesh_rid_to_path"

## Writable folder under the project for ResourceSaver during GUT (user:// can fail in some headless setups).
var _test_dir := "res://test/unit/_gut_reso_sidecar"


func _clear_test_dir() -> void:
	var abs_base := ProjectSettings.globalize_path(_test_dir)
	DirAccess.make_dir_recursive_absolute(abs_base)
	var d := DirAccess.open(_test_dir)
	if d == null:
		return
	d.list_dir_begin()
	var n := d.get_next()
	while n != "":
		if n != "." and n != ".." and not d.current_is_dir():
			d.remove(n)
		n = d.get_next()
	d.list_dir_end()


func _triangle_mesh() -> ArrayMesh:
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(
		[
			Vector3(0, 0, 0),
			Vector3(1, 0, 0),
			Vector3(0, 1, 0),
		]
	)
	var m := ArrayMesh.new()
	m.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return m


func test_two_mesh_instances_same_mesh_reuse_one_sidecar_path() -> void:
	_clear_test_dir()
	var root := Node.new()
	root.set_meta("_nexus_gltf_path", _test_dir.path_join("asset.gltf"))
	root.set_meta("nexus_resonance_paths_used", [])
	root.set_meta(_RID_META, {})
	root.set_meta("nexus_resonance_nodes", [])

	var shared := _triangle_mesh()
	var meta := {"nexus_mesh_collision_shape": "RESONANCE_STATIC"}
	var stats := {"resonance": 0}

	var mi1 := MeshInstance3D.new()
	mi1.name = "Wall_block"
	mi1.mesh = shared
	root.add_child(mi1)

	var proc: Object = _ResProc.new()
	assert_true(proc.process(mi1, meta, {}, root, stats), "first process should handle node")

	var mi2 := MeshInstance3D.new()
	mi2.name = "Other_wall"
	mi2.mesh = shared
	root.add_child(mi2)
	assert_true(proc.process(mi2, meta, {}, root, stats), "second process should handle node")

	var nodes: Array = root.get_meta("nexus_resonance_nodes", [])
	assert_eq(nodes.size(), 2, "two resonance entries")
	var p0: String = str(nodes[0].get("mesh_path", ""))
	var p1: String = str(nodes[1].get("mesh_path", ""))
	assert_eq(p0, p1, "same Mesh RID should reuse one sidecar path")
	assert_eq(p0.get_file(), "asset_Wall_block.res", "first node wins base filename")

	var used: Array = root.get_meta("nexus_resonance_paths_used", [])
	assert_eq(used.size(), 1, "paths_used should list the file once")

	root.free()


func test_two_different_meshes_same_short_name_get_distinct_paths() -> void:
	_clear_test_dir()
	var root := Node.new()
	root.set_meta("_nexus_gltf_path", _test_dir.path_join("prop.gltf"))
	root.set_meta("nexus_resonance_paths_used", [])
	root.set_meta(_RID_META, {})
	root.set_meta("nexus_resonance_nodes", [])

	var mesh_a := _triangle_mesh()
	var mesh_b := _triangle_mesh()
	var meta := {"nexus_mesh_collision_shape": "RESONANCE_STATIC"}
	var stats := {"resonance": 0}
	var proc: Object = _ResProc.new()

	var mi1 := MeshInstance3D.new()
	mi1.name = "SameCollision"
	mi1.mesh = mesh_a
	root.add_child(mi1)
	assert_true(proc.process(mi1, meta, {}, root, stats))

	var mi2 := MeshInstance3D.new()
	mi2.name = "SameCollision"
	mi2.mesh = mesh_b
	root.add_child(mi2)
	assert_true(proc.process(mi2, meta, {}, root, stats))

	var nodes: Array = root.get_meta("nexus_resonance_nodes", [])
	assert_eq(nodes.size(), 2)
	var p0: String = str(nodes[0].get("mesh_path", ""))
	var p1: String = str(nodes[1].get("mesh_path", ""))
	assert_ne(p0, p1, "different mesh RIDs must not share path")
	assert_true(p0.ends_with(".res") and p1.ends_with(".res"))
	assert_true(
		p1.contains("_m") or p1.get_file().contains("_"),
		"collision path should use _m<RID> or numeric suffix"
	)

	var used: Array = root.get_meta("nexus_resonance_paths_used", [])
	assert_eq(used.size(), 2)

	root.free()
