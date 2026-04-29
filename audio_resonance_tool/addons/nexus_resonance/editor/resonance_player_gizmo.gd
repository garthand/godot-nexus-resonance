@tool
extends EditorNode3DGizmoPlugin

## Nexus Resonance directivity gizmo for [code]ResonancePlayer[/code].
##
## Draws a wireframe polar curve (dipole) or unit sphere + forward arrow (omni / user-defined)
## derived from [code]ResonancePlayerConfig[/code]. Shape math comes from the native
## [code]ResonancePlayer.build_directivity_gizmo_lines()[/code] static method so editor and
## runtime drawer stay in sync.

const GIZMO_CLASS_NAME := "ResonancePlayer"
const GIZMO_SIZE_METERS := 1.0
const MAT_DIPOLE := "nexus_player_directivity_dipole"
const MAT_OMNI := "nexus_player_directivity_omni"

## Signals us when the editor's selection / scene changes so we can redraw gizmos per frame.
var _mat_dipole_name: String = ""
var _mat_omni_name: String = ""


func _init() -> void:
	var sid := str(get_instance_id())
	_mat_dipole_name = MAT_DIPOLE + "_" + sid
	_mat_omni_name = MAT_OMNI + "_" + sid
	create_material(_mat_dipole_name, Color(1.0, 0.75, 0.15))
	create_material(_mat_omni_name, Color(0.25, 0.85, 1.0))


func _get_gizmo_name() -> String:
	return GIZMO_CLASS_NAME


func _has_gizmo(node: Node) -> bool:
	return node != null and node.is_class(GIZMO_CLASS_NAME)


func _redraw(gizmo: EditorNode3DGizmo) -> void:
	gizmo.clear()
	var node := gizmo.get_node_3d()
	if node == null:
		return
	_ensure_live_connection(node)
	# [code]show_directivity_gizmo[/code] is the single source of truth for editor and runtime visibility.
	if not bool(node.get("show_directivity_gizmo")):
		return
	var cfg: Object = node.get("player_config") as Object
	var enabled := false
	var input_mode := 0
	var weight := 0.0
	var power := 1.0
	var user_value := 1.0
	if cfg != null:
		enabled = bool(cfg.get("directivity_enabled"))
		input_mode = int(cfg.get("directivity_input"))
		weight = float(cfg.get("directivity_weight"))
		power = float(cfg.get("directivity_power"))
		user_value = float(cfg.get("directivity_value"))

	# [method ResonancePlayer.build_directivity_gizmo_lines] is a static method bound from GDExtension;
	# calling it through the instance works in GDScript regardless of editor global-class registration timing.
	var lines: PackedVector3Array = node.build_directivity_gizmo_lines(
		enabled, input_mode, weight, power, user_value, GIZMO_SIZE_METERS
	)
	if lines.is_empty():
		return

	var is_dipole := enabled and input_mode == 0
	var mat_name := _mat_dipole_name if is_dipole else _mat_omni_name
	gizmo.add_lines(lines, get_material(mat_name, gizmo))


## Keeps the editor gizmo live while the user edits directivity parameters inline. GDExtension nodes
## do not run [code]_process[/code] in the editor, so the native [signal Resource.changed] hook on
## [code]ResonancePlayer[/code] does not repaint the editor viewport. We connect the signal from this
## [code]@tool[/code] plugin and forward it to [method Node3D.update_gizmos] — [code]is_connected[/code]
## with an identical [Callable] keeps the call idempotent across repeated [method _redraw] invocations.
func _ensure_live_connection(node: Node3D) -> void:
	var cfg: Resource = node.get("player_config") as Resource
	if cfg == null:
		return
	var refresh_cb := Callable(node, "update_gizmos")
	if not cfg.changed.is_connected(refresh_cb):
		cfg.changed.connect(refresh_cb)
