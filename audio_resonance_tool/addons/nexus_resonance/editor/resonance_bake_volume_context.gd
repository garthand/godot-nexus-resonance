extends RefCounted
class_name VolumeBakeContext

## Per-volume bake state for one pipeline step (reflections, pathing, static source/listener).

const _Self = preload("res://addons/nexus_resonance/editor/resonance_bake_volume_context.gd")
const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const _BakeHashes = preload("res://addons/nexus_resonance/editor/resonance_bake_hashes.gd")

var root: Node
var vol: Node
var probe_data: Resource
var need_reflections: bool
var need_pathing: bool
var need_static_source: bool
var need_static_listener: bool
var add_flags: Dictionary
var player_pos: Vector3
var player_radius: float
var listener_pos: Vector3
var listener_radius: float
## Every outdoor emitter listed in [member ResonanceProbeVolume.bake_sources]. Each entry is
## [code]{pos: Vector3, radius: float, node: Node3D}[/code]. The pipeline iterates this list and
## issues one [code]bake_static_source[/code] pass per entry so multiple fixed sources (e.g. rain,
## thunder) produce position-dependent baked IRs instead of a single listener-only REVERB IR.
var static_source_entries: Array = []
## As [member static_source_entries] but for [member ResonanceProbeVolume.bake_listeners].
var static_listener_entries: Array = []
var bc: Resource
var vol_info: String
var static_asset = null  # ResonanceGeometryAsset used for bake; for static_scene_params_hash


static func build(
	vol: Node,
	root: Node,
	vol_index: int,
	total: int,
	static_asset,
	get_bake_config_for_volume: Callable,
	default_influence_radius: float
):
	var ctx = _Self.new()
	ctx.root = root
	ctx.vol = vol
	ctx.static_asset = static_asset
	ctx.vol_info = " (volume %d of %d)" % [vol_index, total] if total > 1 else ""
	ctx.bc = get_bake_config_for_volume.call(vol) if get_bake_config_for_volume.is_valid() else null
	if ctx.bc == null:
		ctx.bc = ResonanceBakeConfig.create_default()
	ctx.add_flags = {
		"static_source": ctx.bc.static_source_enabled, "static_listener": ctx.bc.static_listener_enabled
	}
	ctx.player_pos = Vector3.ZERO
	ctx.player_radius = (
		vol.get("bake_influence_radius")
		if "bake_influence_radius" in vol
		else default_influence_radius
	)
	ctx.listener_pos = Vector3.ZERO
	ctx.listener_radius = (
		vol.get("bake_influence_radius")
		if "bake_influence_radius" in vol
		else default_influence_radius
	)
	if ctx.add_flags.static_source:
		var src_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
			vol, root, "bake_sources", "ResonancePlayer"
		)
		for n in src_nodes:
			if n is Node3D:
				ctx.static_source_entries.append(
					{"pos": n.global_position, "radius": ctx.player_radius, "node": n}
				)
		# Keep legacy single-value fields populated for status UI / debug that still read them.
		if ctx.static_source_entries.size() > 0:
			ctx.player_pos = ctx.static_source_entries[0].pos
	if ctx.add_flags.static_listener:
		var lst_nodes: Array = _BakeDiscovery.resolve_bake_nodes_for_volume(
			vol, root, "bake_listeners", "ResonanceListener"
		)
		for n in lst_nodes:
			if n is Node3D:
				ctx.static_listener_entries.append(
					{"pos": n.global_position, "radius": ctx.listener_radius, "node": n}
				)
		if ctx.static_listener_entries.size() > 0:
			ctx.listener_pos = ctx.static_listener_entries[0].pos
	var want_path = ctx.bc.pathing_enabled
	var path_hash = _BakeHashes.compute_pathing_hash(ctx.bc) if want_path else 0
	var probe_data = vol.get_probe_data() if vol.has_method("get_probe_data") else null
	if not probe_data:
		probe_data = ClassDB.instantiate("ResonanceProbeData")
		vol.set_probe_data(probe_data)
	ctx.probe_data = probe_data
	var ph = (
		probe_data.get_pathing_params_hash()
		if probe_data.has_method("get_pathing_params_hash")
		else 0
	)
	var has_data = probe_data.get_data().size() > 0
	var desired_refl = ctx.bc.reflection_type
	var refl_matches = (
		probe_data.get_baked_reflection_type() == desired_refl
		if probe_data.has_method("get_baked_reflection_type")
		else false
	)
	var hash_matches = (
		probe_data.get_bake_params_hash() == vol.get_bake_params_hash()
		if vol.has_method("get_bake_params_hash")
		else false
	)
	ctx.need_reflections = not has_data or not hash_matches or not refl_matches
	var union_static_hash: int = _BakeHashes.compute_all_resonance_static_scenes_params_hash(root)
	if union_static_hash != 0 and probe_data.has_method("get_static_scene_params_hash"):
		var stored_union: int = probe_data.get_static_scene_params_hash()
		if stored_union == 0 or stored_union != union_static_hash:
			ctx.need_reflections = true
	if not want_path and ph > 0:
		ctx.need_reflections = true
	ctx.need_pathing = want_path and (ph == 0 or ph != path_hash)
	if want_path and ctx.need_pathing and (not has_data or not refl_matches):
		ctx.need_reflections = true
	ctx.need_static_source = false
	ctx.need_static_listener = false
	if ctx.add_flags.static_source:
		var sh := 0
		if ctx.static_source_entries.size() > 1:
			sh = _BakeHashes.compute_position_radius_list_hash(ctx.static_source_entries)
		else:
			sh = _BakeHashes.compute_position_radius_hash(ctx.player_pos, ctx.player_radius)
		var ssh = (
			probe_data.get_static_source_params_hash()
			if probe_data.has_method("get_static_source_params_hash")
			else 0
		)
		ctx.need_static_source = ssh == 0 or ssh != sh
	if ctx.add_flags.static_listener:
		var lh := 0
		if ctx.static_listener_entries.size() > 1:
			lh = _BakeHashes.compute_position_radius_list_hash(ctx.static_listener_entries)
		else:
			lh = _BakeHashes.compute_position_radius_hash(ctx.listener_pos, ctx.listener_radius)
		var lsh = (
			probe_data.get_static_listener_params_hash()
			if probe_data.has_method("get_static_listener_params_hash")
			else 0
		)
		ctx.need_static_listener = lsh == 0 or lsh != lh
	return ctx
