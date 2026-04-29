extends Object
class_name ResonanceBakeHashes

## Hash helpers for bake params and static source/listener positions (shared by context + status UI).

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")


static func hash_dict(d: Dictionary) -> int:
	return hash(var_to_str(d))


static func compute_pathing_hash(bc: Resource) -> int:
	var params = bc.get_bake_params()
	return hash_dict(
		{
			"vis_range": params.get("bake_pathing_vis_range", 500),
			"path_range": params.get("bake_pathing_path_range", 100),
			"num_samples": params.get("bake_pathing_num_samples", 16),
			"radius": params.get("bake_pathing_radius", 0.5),
			"threshold": params.get("bake_pathing_threshold", 0.1)
		}
	)


static func compute_position_radius_hash(pos: Vector3, radius: float) -> int:
	return hash_dict({"pos": pos, "radius": radius})


## Combined hash for a list of (position, radius) tuples. Invalidates STATICSOURCE/STATICLISTENER
## bakes when the set of outdoor emitters changes (added/removed/moved or radius changed).
static func compute_position_radius_list_hash(entries: Array) -> int:
	if entries.is_empty():
		return 0
	var buckets: Array = []
	for e in entries:
		var pos: Vector3 = e.get("pos", Vector3.ZERO) if e is Dictionary else Vector3.ZERO
		var radius: float = e.get("radius", 0.0) if e is Dictionary else 0.0
		buckets.append({"pos": pos, "radius": radius})
	return hash_dict({"entries": buckets})


## Combined hash for all [ResonanceStaticScene] nodes under [param root] (geometry asset hash + global transform each).
## Order follows [method ResonanceSceneUtils.collect_resonance_static_scenes] (depth-first, first child first).
## Used so probe bake invalidation sees every static pack, not only the first scene.
static func compute_all_resonance_static_scenes_params_hash(root: Node) -> int:
	if root == null or not ResonanceServerAccess.has_server():
		return 0
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or not srv.has_method("get_geometry_asset_hash"):
		return 0
	var scenes: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_static_scenes(root, scenes)
	if scenes.is_empty():
		return 0
	var parts: Array = []
	for ss in scenes:
		var asset = ss.get("static_scene_asset") if "static_scene_asset" in ss else null
		if asset == null:
			continue
		if not (ss.has_method("has_valid_asset") and ss.has_valid_asset()):
			continue
		var ah: int = int(srv.get_geometry_asset_hash(asset))
		var xf: Transform3D = (
			(ss as Node3D).global_transform if ss is Node3D else Transform3D.IDENTITY
		)
		parts.append({"h": ah, "t": xf})
	if parts.is_empty():
		return 0
	return hash(var_to_str(parts))
