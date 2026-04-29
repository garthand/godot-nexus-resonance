@tool
extends RefCounted
class_name ResonanceBakeBackup

## Backup and restore logic for probe data before bake. Extracted for SRP.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)

var _backup_paths: Dictionary = {}  # probe_data resource_path -> backup file path


func create_backups(volumes: Array[Node]) -> void:
	_backup_paths.clear()
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if pd and pd.resource_path and pd.resource_path.get_file().length() > 0:
			var backup_path = pd.resource_path + ".bak"
			var err = ResourceSaver.save(pd, backup_path)
			if err == OK:
				_backup_paths[pd.resource_path] = backup_path


func has_backups() -> bool:
	return not _backup_paths.is_empty()


func restore(
	volumes: Array[Node],
	editor_interface: EditorInterface,
	on_reload: Callable,
	on_complete: Callable
) -> void:
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if not pd or not pd.resource_path:
			continue
		var backup_path = _backup_paths.get(pd.resource_path, "")
		if backup_path.is_empty() or not FileAccess.file_exists(backup_path):
			continue
		var backup = load(backup_path) as Resource
		if backup:
			if pd.has_method("copy_from"):
				pd.copy_from(backup)
			else:
				_copy_probe_data_properties(pd, backup)
			ResourceSaver.save(pd, pd.resource_path)
			on_reload.call(pd, volumes)
	ResonanceEditorDialogs.show_success_toast(editor_interface, UIStrings.INFO_BACKUP_RESTORED)
	_backup_paths.clear()
	on_complete.call()


func _copy_probe_data_properties(dst: Resource, src: Resource) -> void:
	if dst.has_method("set_data") and src.has_method("get_data"):
		dst.set_data(src.get_data())
	# Fallback when copy_from unavailable; extend list if ResonanceProbeData gains new hash properties
	for prop in [
		"pathing_params_hash",
		"static_source_params_hash",
		"static_listener_params_hash",
		"bake_params_hash"
	]:
		if prop in src and prop in dst:
			dst.set(prop, src.get(prop))


func shutdown() -> void:
	_backup_paths.clear()
