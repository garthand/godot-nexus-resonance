extends GutTest

## Unit tests for [ResonanceBakeDiscovery] static helpers (no [EditorInterface]).

const BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const RuntimeStubScript = preload("res://test/unit/bake_discovery_runtime_stub.gd")


## Native subclass so [code]property in vol[/code] matches editor volumes (script-only nodes can fail [code]in[/code]).
class _VolWithBakePaths:
	extends Node
	@export var test_bake_nodepaths: Array = [NodePath("Target")]


func _attach_to_main_scene_tree(n: Node) -> void:
	var st := Engine.get_main_loop() as SceneTree
	assert_not_null(st, "GUT should run under a SceneTree")
	st.root.add_child(n)


func test_find_resonance_runtime_null_node_returns_null():
	var found = BakeDiscovery.find_resonance_runtime(null, RuntimeStubScript)
	assert_null(found)


func test_find_resonance_runtime_finds_direct_child_with_matching_script():
	var root = Node.new()
	var match_node = Node.new()
	match_node.set_script(RuntimeStubScript)
	root.add_child(match_node)
	var found = BakeDiscovery.find_resonance_runtime(root, RuntimeStubScript)
	assert_eq(found, match_node)
	root.free()


func test_find_resonance_runtime_finds_nested_matching_script():
	var root = Node.new()
	var mid = Node.new()
	var leaf = Node.new()
	leaf.set_script(RuntimeStubScript)
	mid.add_child(leaf)
	root.add_child(mid)
	var found = BakeDiscovery.find_resonance_runtime(root, RuntimeStubScript)
	assert_eq(found, leaf)
	root.free()


func test_find_resonance_runtime_no_match_returns_null():
	var root = Node.new()
	var other = Node.new()
	root.add_child(other)
	var found = BakeDiscovery.find_resonance_runtime(root, RuntimeStubScript)
	assert_null(found)
	root.free()


func test_resolve_bake_node_for_volume_finds_child_via_path_when_in_tree():
	# Orphan nodes are not is_inside_tree(); discovery uses vol.get_node_or_null only when in tree.
	var holder = Node.new()
	_attach_to_main_scene_tree(holder)
	var root = Node.new()
	var vol = _VolWithBakePaths.new()
	var target = Node.new()
	target.name = "Target"
	vol.add_child(target)
	root.add_child(vol)
	holder.add_child(root)
	assert_true(vol.is_inside_tree())
	var resolved = BakeDiscovery.resolve_bake_node_for_volume(
		vol, root, "test_bake_nodepaths", "Node"
	)
	assert_eq(resolved, target)
	holder.queue_free()


func test_resolve_bake_node_for_volume_falls_back_to_root_when_vol_not_in_tree():
	var root = Node.new()
	var vol = _VolWithBakePaths.new()
	vol.test_bake_nodepaths = [NodePath("Target")]
	var target = Node.new()
	target.name = "Target"
	root.add_child(target)
	# vol not in tree: get_node_or_null on vol fails; root should resolve path
	var resolved = BakeDiscovery.resolve_bake_node_for_volume(
		vol, root, "test_bake_nodepaths", "Node"
	)
	assert_eq(resolved, target)
	vol.free()
	root.free()


func test_resolve_bake_node_for_volume_wrong_class_returns_null():
	var root = Node.new()
	var vol = _VolWithBakePaths.new()
	vol.test_bake_nodepaths = [NodePath("Target")]
	var target = Node.new()
	target.name = "Target"
	root.add_child(target)
	var resolved = BakeDiscovery.resolve_bake_node_for_volume(
		vol, root, "test_bake_nodepaths", "MeshInstance3D"
	)
	assert_null(resolved)
	vol.free()
	root.free()


func test_resolve_bake_node_for_volume_missing_property_returns_null():
	var vol = Node.new()
	var root = Node.new()
	var resolved = BakeDiscovery.resolve_bake_node_for_volume(
		vol, root, "test_bake_nodepaths", "Node"
	)
	assert_null(resolved)
	vol.free()
	root.free()
