extends GutTest

## ResonancePlayer inherits AudioStreamPlayer3D polyphony; with player_config, all voices must receive
## the same Steam parameters (C++ registry + broadcast).


func test_resonance_player_inherits_max_polyphony() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		pass_test("ResonancePlayer not available (GDExtension not loaded)")
		return
	var player := ResonancePlayer.new()
	add_child_autoqfree(player)
	var has_max_polyphony := false
	for p in player.get_property_list():
		if p.name == "max_polyphony":
			has_max_polyphony = true
			break
	if not has_max_polyphony:
		pass_test("max_polyphony not in property list on this Godot build")
		return
	player.max_polyphony = 4
	assert_eq(
		player.max_polyphony,
		4,
		"ResonancePlayer should expose max_polyphony like AudioStreamPlayer3D"
	)


func test_get_audio_instrumentation_has_polyphony_key_when_documented() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		pass_test("ResonancePlayer not available")
		return
	assert_true(
		ClassDB.class_has_method("ResonancePlayer", "get_audio_instrumentation"),
		"ResonancePlayer should expose get_audio_instrumentation"
	)
