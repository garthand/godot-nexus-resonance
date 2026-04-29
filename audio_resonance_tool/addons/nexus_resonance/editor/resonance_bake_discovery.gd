extends Object
class_name ResonanceBakeDiscovery

## Scene tree discovery for [ResonanceRuntime], [ResonanceStaticScene], and bake source/listener nodes.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")


static func find_resonance_runtime(node: Node, resonance_runtime_script: GDScript) -> Node:
	if not node:
		return null
	if node.get_script() == resonance_runtime_script:
		return node
	if node.is_class("ResonanceRuntime"):
		return node
	for c in node.get_children():
		var found = find_resonance_runtime(c, resonance_runtime_script)
		if found:
			return found
	return null


static func find_resonance_static_scene_for_bake(volumes: Array[Node], edited_root: Node) -> Node:
	if edited_root:
		var found = ResonanceSceneUtils.find_resonance_static_scene(edited_root)
		if found:
			return found
	if volumes.size() > 0:
		var vol: Node = volumes[0]
		var branch_root: Node = vol
		while vol.get_parent():
			branch_root = vol.get_parent()
			vol = branch_root
		var found2 = ResonanceSceneUtils.find_resonance_static_scene(branch_root)
		if found2:
			return found2
	var tree: SceneTree = null
	if edited_root:
		tree = edited_root.get_tree()
	if not tree and volumes.size() > 0:
		tree = volumes[0].get_tree() if volumes[0].is_inside_tree() else null
	if tree:
		var scene_root = tree.get_edited_scene_root() if "edited_scene_root" in tree else tree.root
		if scene_root:
			return ResonanceSceneUtils.find_resonance_static_scene(scene_root)
	return null


static func resolve_bake_node_for_volume(
	vol: Node, root: Node, property: String, target_class: String
) -> Node:
	var arr = vol.get(property) if property in vol else []
	if arr is Array and arr.size() > 0:
		var path_val = arr[0]
		var path = NodePath(str(path_val)) if path_val else NodePath()
		if not path.is_empty():
			var n = vol.get_node_or_null(path) if vol.is_inside_tree() else null
			if not n and root:
				n = root.get_node_or_null(path)
			if n and n.is_class(target_class):
				return n
	return null


## Resolve all NodePaths in [param vol].[param property] to live Node3D instances of [param target_class].
## Used by the bake pipeline to issue one STATICSOURCE/STATICLISTENER pass per outdoor emitter so that
## multiple fixed sources (rain, thunder, HVAC, ...) produce position-dependent baked IRs instead of a
## single listener-only REVERB IR.
static func resolve_bake_nodes_for_volume(
	vol: Node, root: Node, property: String, target_class: String
) -> Array:
	var out: Array = []
	var arr = vol.get(property) if property in vol else []
	if not (arr is Array):
		return out
	for path_val in arr:
		var path = NodePath(str(path_val)) if path_val else NodePath()
		if path.is_empty():
			continue
		var n = vol.get_node_or_null(path) if vol.is_inside_tree() else null
		if not n and root:
			n = root.get_node_or_null(path)
		if n and n.is_class(target_class):
			out.append(n)
	return out
