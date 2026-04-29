extends Node

## Nexus Resonance Logger with thematic category filtering.
## Access via Engine.get_singleton("ResonanceLogger"). Do not use class_name (conflicts with autoload).
## Use from GDScript: ResonanceLogger.log("reflections", "message", {...})
## C++ calls via Engine.get_singleton("ResonanceLogger").log(...)
##
## Categories: reflections, realtime_rays, source_volume, pathing, occlusion, init, bake, validation
##
## File output uses a mutex-protected queue for thread-safe writes when log() is called from
## multiple threads (e.g. C++). Entries are flushed to disk in _process() on the main thread.

const DEFAULT_BUFFER_SIZE := 128
const PROJECT_PREFIX := "nexus/resonance/logger/"

## Standard categories for filtering
const CATEGORY_REFLECTIONS := &"reflections"
const CATEGORY_REALTIME_RAYS := &"realtime_rays"
const CATEGORY_SOURCE_VOLUME := &"source_volume"
const CATEGORY_PATHING := &"pathing"
const CATEGORY_OCCLUSION := &"occlusion"
const CATEGORY_INIT := &"init"
const CATEGORY_BAKE := &"bake"
const CATEGORY_VALIDATION := &"validation"

const ALL_CATEGORIES: Array[StringName] = [
	CATEGORY_REFLECTIONS,
	CATEGORY_REALTIME_RAYS,
	CATEGORY_SOURCE_VOLUME,
	CATEGORY_PATHING,
	CATEGORY_OCCLUSION,
	CATEGORY_INIT,
	CATEGORY_BAKE,
	CATEGORY_VALIDATION
]


## Default Project Settings value for `logger/categories_enabled` (all categories on).
static func get_default_categories_enabled_dict() -> Dictionary:
	var d := {}
	for c in ALL_CATEGORIES:
		d[str(c)] = true
	return d


## Emitted when a log entry is added (after category filter). Args: category, message, data
signal log_entry_added(category: StringName, message: String, data: Dictionary)

var _buffer: Array[Dictionary] = []
var _buffer_size: int = DEFAULT_BUFFER_SIZE
var _categories_enabled: Dictionary = {}
var _output_to_debug: bool = true
var _output_to_file: bool = false
var _file_path: String = "user://nexus_resonance_log.ndjson"
var _file_write_queue: Array = []
var _file_write_mutex: Mutex = Mutex.new()


## Rejects absolute system paths and path traversal. Only user:// and res:// are allowed.
static func _is_safe_log_path(path: String) -> bool:
	if path.is_empty():
		return false
	if not path.begins_with("user://") and not path.begins_with("res://"):
		return false
	if ".." in path or "/../" in path or path.ends_with("/.."):
		return false
	return true


func _init() -> void:
	_load_category_defaults()
	for cat in ALL_CATEGORIES:
		if not _categories_enabled.has(cat):
			_categories_enabled[cat] = true


func _ready() -> void:
	# ProjectSettings and plugin-registered keys are guaranteed after entering the tree.
	_load_category_defaults()


func _process(_delta: float) -> void:
	if not _output_to_file:
		return
	_file_write_mutex.lock()
	var to_write: Array = _file_write_queue.duplicate()
	_file_write_queue.clear()
	_file_write_mutex.unlock()
	for entry in to_write:
		_write_to_file(entry)


func _load_category_defaults() -> void:
	if ProjectSettings.has_setting(PROJECT_PREFIX + "categories_enabled"):
		var v = ProjectSettings.get_setting(PROJECT_PREFIX + "categories_enabled")
		if v is Dictionary:
			if v.is_empty():
				for cat in ALL_CATEGORIES:
					_categories_enabled[cat] = true
			else:
				for k in v:
					_categories_enabled[StringName(str(k))] = bool(v[k])
	if ProjectSettings.has_setting(PROJECT_PREFIX + "output_to_debug"):
		_output_to_debug = ProjectSettings.get_setting(PROJECT_PREFIX + "output_to_debug")
	if ProjectSettings.has_setting(PROJECT_PREFIX + "output_to_file"):
		_output_to_file = ProjectSettings.get_setting(PROJECT_PREFIX + "output_to_file")
	if ProjectSettings.has_setting(PROJECT_PREFIX + "file_path"):
		var configured: String = ProjectSettings.get_setting(PROJECT_PREFIX + "file_path")
		if _is_safe_log_path(configured):
			_file_path = configured
		else:
			push_warning(
				"Nexus Resonance: Logger file_path must be user:// or res:// (no path traversal). Using default."
			)


## Log a message. Category determines filtering. Data is optional extra context.
func log(category: StringName, message: String, data: Dictionary = {}) -> void:
	if not _is_category_enabled(category):
		return

	var entry := {
		"timestamp": Time.get_ticks_msec(),
		"category": String(category),
		"message": message,
		"data": data.duplicate()
	}

	_add_to_buffer(entry)

	if _output_to_debug:
		_output_to_debug_console(category, message, data)

	if _output_to_file:
		_file_write_mutex.lock()
		_file_write_queue.append(entry)
		_file_write_mutex.unlock()

	log_entry_added.emit(category, message, data)


func _is_category_enabled(category: StringName) -> bool:
	if _categories_enabled.has(category):
		return _categories_enabled[category]
	return true


func _add_to_buffer(entry: Dictionary) -> void:
	_buffer.append(entry)
	while _buffer.size() > _buffer_size:
		_buffer.pop_front()


func _output_to_debug_console(category: StringName, message: String, data: Dictionary) -> void:
	var data_suffix := "" if data.is_empty() else " " + str(data)
	# Use print_rich so the line appears as normal log output in the Output dock; category is visible without BBCode too.
	print_rich(
		"[color=cyan][Nexus Resonance][%s][/color] %s%s" % [String(category), message, data_suffix]
	)
	# Plain duplicate: some filters or remote runs only surface standard print() reliably.
	print("[Nexus Resonance][%s] %s%s" % [String(category), message, data_suffix])


func _write_to_file(entry: Dictionary) -> void:
	if not _is_safe_log_path(_file_path):
		return
	var path := _file_path
	if path.begins_with("user://") or path.begins_with("res://"):
		path = ProjectSettings.globalize_path(path)
	var dict := {
		"timestamp": entry.timestamp,
		"category": entry.category,
		"message": entry.message,
		"data": entry.data
	}
	var line := JSON.stringify(dict) + "\n"
	var f: FileAccess = null
	# READ_WRITE appends; multiple processes (e.g. editor + game) may conflict. Recommended: single writer.
	if FileAccess.file_exists(path):
		f = FileAccess.open(path, FileAccess.READ_WRITE)
	if f == null:
		f = FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_warning(
			(
				"Nexus Resonance: Cannot open log file for writing: %s (error %d)"
				% [path, FileAccess.get_open_error()]
			)
		)
		return
	f.seek_end()
	f.store_string(line)
	f.close()


## Enable or disable a category. Affects future log calls.
func set_category_enabled(category: StringName, enabled: bool) -> void:
	_categories_enabled[category] = enabled


## Get whether a category is enabled
func is_category_enabled(category: StringName) -> bool:
	return _is_category_enabled(category)


## Get the last N log entries (for overlay display)
func get_recent_entries(count: int = 32) -> Array[Dictionary]:
	var start := maxi(0, _buffer.size() - count)
	var result: Array[Dictionary] = []
	for i in range(start, _buffer.size()):
		result.append(_buffer[i])
	return result


## Clear the internal buffer
func clear_buffer() -> void:
	_buffer.clear()


func set_buffer_size(size: int) -> void:
	_buffer_size = maxi(16, size)
	while _buffer.size() > _buffer_size:
		_buffer.pop_front()


func set_output_to_debug(enabled: bool) -> void:
	_output_to_debug = enabled


func set_output_to_file(enabled: bool) -> void:
	_output_to_file = enabled


func set_file_path(path: String) -> void:
	if _is_safe_log_path(path):
		_file_path = path
	else:
		push_warning(
			"Nexus Resonance: Logger file_path must be user:// or res:// (no path traversal). Ignored."
		)


## Returns all category StringNames for UI (e.g. filter checkboxes)
func get_all_categories() -> Array[StringName]:
	return ALL_CATEGORIES.duplicate()
