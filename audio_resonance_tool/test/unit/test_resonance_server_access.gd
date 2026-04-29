extends GutTest

## Unit tests for [ResonanceServerAccess] (Engine singleton wrapper).


func test_singleton_name_matches_engine_key() -> void:
	assert_eq(ResonanceServerAccess.SINGLETON_NAME, &"ResonanceServer")


func test_has_server_consistent_with_classdb() -> void:
	# When the GDExtension class is registered, the engine singleton is normally present.
	var class_registered: bool = ClassDB.class_exists("ResonanceServer")
	if not class_registered:
		assert_false(
			ResonanceServerAccess.has_server(), "no ResonanceServer class implies no singleton"
		)
	else:
		# In CI or headless runs without the native library, class can exist without singleton.
		var has: bool = ResonanceServerAccess.has_server()
		if has:
			var s: Variant = ResonanceServerAccess.get_server()
			assert_not_null(s, "has_server() true implies get_server() non-null")


func test_get_server_null_when_missing() -> void:
	if ResonanceServerAccess.has_server():
		assert_not_null(ResonanceServerAccess.get_server())
	else:
		assert_null(ResonanceServerAccess.get_server())


func test_get_server_if_initialized_only_when_ready() -> void:
	var s: Variant = ResonanceServerAccess.get_server()
	if s == null:
		assert_null(ResonanceServerAccess.get_server_if_initialized())
		return
	if s.has_method("is_initialized") and s.is_initialized():
		var server_if_ready: Variant = ResonanceServerAccess.get_server_if_initialized()
		assert_not_null(server_if_ready)
		assert_eq(server_if_ready, s)
	else:
		assert_null(ResonanceServerAccess.get_server_if_initialized())


func test_resonance_server_exposes_shutdown() -> void:
	if not ClassDB.class_exists("ResonanceServer"):
		pass_test("ResonanceServer not available (GDExtension not loaded)")
		return
	assert_true(
		ClassDB.class_has_method("ResonanceServer", "shutdown"),
		"ResonanceServer.shutdown should be callable from GDScript for ordered exit teardown"
	)
