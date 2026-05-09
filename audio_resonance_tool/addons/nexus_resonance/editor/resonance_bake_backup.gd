@tool
extends RefCounted
class_name ResonanceBakeBackup

## Backup/restore for probe data before bake.
##
## Uses 1:1 disk copies via [DirAccess.copy_absolute] to avoid triggering the custom probe saver,
## which may call [code]take_over_path[/code] and accidentally redirect future saves to the .bak file.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)

var _backup_paths: Dictionary = {}  # probe_data resource_path -> backup file path


## Strips trailing .bak suffixes (e.g. self-heal a path like "foo.res.bak.bak" -> "foo.res").
static func _strip_bak_suffixes(path: String) -> String:
	var p := path
	while p.ends_with(".bak"):
		p = p.get_basename()
	return p


func create_backups(volumes: Array[Node]) -> void:
	_backup_paths.clear()
	for vol in volumes:
		var pd = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		if not pd or not pd.resource_path or pd.resource_path.get_file().length() == 0:
			continue
		var original_path: String = _strip_bak_suffixes(pd.resource_path)
		# Self-heal: if a previous (buggy) bake left resource_path pointing at a .bak file,
		# point it back to the canonical path before saving anything else.
		if original_path != pd.resource_path and pd.has_method("take_over_path"):
			pd.take_over_path(original_path)
		if not FileAccess.file_exists(original_path):
			continue
		var backup_path: String = original_path + ".bak"
		# DirAccess.copy_absolute overwrites if backup_path already exists.
		var err: int = DirAccess.copy_absolute(original_path, backup_path)
		if err == OK:
			_backup_paths[original_path] = backup_path
		else:
			push_warning(
				(
					"Nexus Resonance: Failed to create probe data backup at %s (error %d)."
					% [backup_path, err]
				)
			)


func has_backups() -> bool:
	return not _backup_paths.is_empty()


func discard_backups() -> void:
	for backup_path in _backup_paths.values():
		_remove_backup_file(backup_path)
	_backup_paths.clear()


static func _remove_backup_file(backup_path: String) -> void:
	if FileAccess.file_exists(backup_path):
		DirAccess.remove_absolute(backup_path)
	# Companion .uid files may exist from legacy ResourceSaver-based backups.
	var uid_path := backup_path + ".uid"
	if FileAccess.file_exists(uid_path):
		DirAccess.remove_absolute(uid_path)


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
		var lookup_path: String = _strip_bak_suffixes(pd.resource_path)
		var backup_path: String = _backup_paths.get(lookup_path, "")
		if backup_path.is_empty() or not FileAccess.file_exists(backup_path):
			continue
		var backup = load(backup_path) as Resource
		if backup:
			if pd.has_method("copy_from"):
				pd.copy_from(backup)
			else:
				_copy_probe_data_properties(pd, backup)
			# Make sure resource_path is the canonical path before saving.
			if pd.resource_path != lookup_path and pd.has_method("take_over_path"):
				pd.take_over_path(lookup_path)
			ResourceSaver.save(pd, lookup_path)
			on_reload.call(pd, volumes)
	ResonanceEditorDialogs.show_success_toast(editor_interface, UIStrings.INFO_BACKUP_RESTORED)
	# After successful restore, the .bak files are no longer needed.
	for backup_path in _backup_paths.values():
		_remove_backup_file(backup_path)
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
