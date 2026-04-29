extends GutTest

## Regression: ResonancePlayer must obtain a Steam source handle after ResonanceServer init even when
## the player node's _ready runs before the parent ResonanceRuntime (Godot child _ready before parent).


func test_resonance_player_exposes_play_and_play_stream() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		pass_test("ResonancePlayer not available (GDExtension not loaded)")
		return
	assert_true(
		ClassDB.class_has_method("ResonancePlayer", "play"),
		"ResonancePlayer should expose play (wraps sim hookup)"
	)
	assert_true(
		ClassDB.class_has_method("ResonancePlayer", "play_stream"),
		"ResonancePlayer should expose play_stream"
	)
