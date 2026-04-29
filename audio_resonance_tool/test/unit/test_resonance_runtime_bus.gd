extends GutTest


func test_ensure_resonance_reverb_effect_on_bus_negative_index_noop() -> void:
	ResonanceRuntimeBus.ensure_resonance_reverb_effect_on_bus(-1)
	assert_true(true, "negative index should not crash")


func test_ensure_resonance_reverb_effect_on_bus_adds_when_missing() -> void:
	if not ClassDB.class_exists("ResonanceAudioEffect"):
		pending("ResonanceAudioEffect not registered (GDExtension not loaded in this run)")
		return
	var bus_name := &"__nexus_test_reverb_bus__"
	var idx := AudioServer.get_bus_index(bus_name)
	if idx < 0:
		AudioServer.add_bus()
		idx = AudioServer.bus_count - 1
		AudioServer.set_bus_name(idx, bus_name)
	# Strip any existing effects (fresh bus may inherit none)
	while AudioServer.get_bus_effect_count(idx) > 0:
		AudioServer.remove_bus_effect(idx, 0)
	ResonanceRuntimeBus.ensure_resonance_reverb_effect_on_bus(idx)
	var found := false
	for i in AudioServer.get_bus_effect_count(idx):
		var eff: AudioEffect = AudioServer.get_bus_effect(idx, i)
		if eff != null and eff.get_class() == "ResonanceAudioEffect":
			found = true
			break
	assert_true(found, "ResonanceAudioEffect should be added when missing")
	# Cleanup: remove test bus
	var cleanup_idx := AudioServer.get_bus_index(bus_name)
	if cleanup_idx >= 0:
		AudioServer.remove_bus(cleanup_idx)
