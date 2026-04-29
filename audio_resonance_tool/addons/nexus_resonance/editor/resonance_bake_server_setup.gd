extends RefCounted
class_name ResonanceBakeServerSetup

## Ensures [ResonanceServerAccess] is initialized for editor bakes and surfaces init errors.

const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")
const ResonanceRuntimeScript = preload("res://addons/nexus_resonance/scripts/resonance_runtime.gd")
const _BakeDiscovery = preload("res://addons/nexus_resonance/editor/resonance_bake_discovery.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

var _runner: Object


func _init(runner: Object) -> void:
	_runner = runner


func shutdown() -> void:
	# Break RefCounted reference cycles (runner <-> server setup).
	_runner = null


func log_and_show_error(
	message: String,
	solution: String = "",
	cause: String = "",
	volume_name: String = "",
	step: String = ""
) -> void:
	var data := {"error": true}
	if not volume_name.is_empty():
		data["volume"] = volume_name
	if not step.is_empty():
		data["step"] = step
	if Engine.has_singleton("ResonanceLogger"):
		Engine.get_singleton("ResonanceLogger").log(&"bake", "Bake error: " + message, data)
	# Untyped local avoids Godot 4.x OPCODE_ASSIGN_TYPED_NATIVE assert with EditorInterface + ternary + null.
	var ei = _runner.editor_interface if _runner else null
	ResonanceEditorDialogs.show_error_dialog(
		ei, tr(UIStrings.DIALOG_BAKE_FAILED_TITLE), message, cause, solution
	)


func ensure_resonance_server_initialized(volumes: Array[Node]) -> bool:
	if not ResonanceServerAccess.has_server():
		log_and_show_error(
			"GDExtension not loaded.", "Ensure the Nexus Resonance GDExtension is installed."
		)
		return false
	var srv = ResonanceServerAccess.get_server()
	if srv.is_initialized():
		return true
	var root: Node = _runner._get_edited_scene_root(volumes) if _runner else null
	var cfg_node = _BakeDiscovery.find_resonance_runtime(root, ResonanceRuntimeScript) if root else null
	var config := {}
	if cfg_node:
		if cfg_node.has_method("get_config_dict"):
			config = cfg_node.get_config_dict()
		else:
			var rt = cfg_node.get("runtime")
			if rt and rt.has_method("get_config"):
				config = rt.get_config()
				config["debug_occlusion"] = false
	if config.is_empty():
		var data := {"error": true}
		if Engine.has_singleton("ResonanceLogger"):
			Engine.get_singleton("ResonanceLogger").log(
				&"bake", "Bake error: No ResonanceRuntime in scene.", data
			)
		var ei = _runner.editor_interface if _runner else null
		var msg := tr(UIStrings.WARN_RUNTIME_REQUIRED_EDITOR)
		ResonanceEditorDialogs.show_warning(ei, msg)
		if ei and ei.has_method("get_editor_toaster"):
			var toaster = ei.get_editor_toaster()
			if toaster and toaster.has_method("push_toast"):
				push_warning(UIStrings.PREFIX + msg)
		return false
	var bake_params := ResonanceBakeConfig.create_default().get_bake_params()
	if volumes.size() > 0:
		var bc = _runner._get_bake_config_for_volume(volumes[0]) if _runner else null
		if bc:
			bake_params = bc.get_bake_params()
	srv.set_bake_params(bake_params)
	srv.init_audio_engine(config)
	if not srv.is_initialized():
		log_and_show_error(
			"Server init failed.",
			"Check Editor output and ResonanceRuntime config. Ensure Steam Audio is properly configured."
		)
		return false
	return true
