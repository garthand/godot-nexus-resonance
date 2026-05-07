extends SceneTree

# Headless diagnostic: loads hallway_test/main.tscn, attaches AudioEffectCapture to Master (final mix)
# and ResonanceReverb (wet bus), logs logical WAV via ResonancePlayer.get_stream(), prints RMS/peak,
# and ASCII sparklines to correlate EOS/tail behaviour with zero_input_count / blocks_processed.
#
# Run (PowerShell):
#   godot --path audio_resonance_tool --headless -s res://test/run_hallway_reverb_capture.gd
#
# Note: `player.stream` may reference internal ResonanceStream; use call("get_stream") for the
# logical AudioStream (e.g. footstep_short.WAV) path.

const HALLWAY_SCENE := "res://hallway_test/main.tscn"
const REVERB_BUS_NAME := "ResonanceReverb"
const MASTER_BUS_NAME := "Master"

const CAPTURE_MSEC := 9000
const SETTLE_MSEC := 500
const CAPTURE_CHUNK_FRAMES_MAX := 4096
const PRINT_INTERVAL_MSEC := 100

## Write user://hallway_reverb_capture.csv (columns: time_ms, clip, rms_master, rms_reverb, zi, blocks).
## rms_* = max chunk RMS per poll (reduces false zeros vs. mean when the capture buffer refills in bursts).
const WRITE_CSV := true


func _clip_label_from_stream(stream: Variant) -> String:
	if stream == null:
		return "(null)"
	var p := ""
	if stream is Resource:
		p = String((stream as Resource).resource_path)
	if not p.is_empty():
		var base := p.get_file()
		if base.contains("short") or base.contains("SHORT"):
			return "short (%s)" % base
		if base.contains("long") or base.contains("LONG"):
			return "long (%s)" % base
		return base
	var short := str(stream)
	var bracket := short.find("[")
	if bracket > 0:
		short = short.substr(0, bracket)
	return short


func _rms_peak_packed(pv: PackedVector2Array) -> Vector3:
	if pv.is_empty():
		return Vector3.ZERO
	var acc := 0.0
	var peak := 0.0
	for i in range(pv.size()):
		var s: Vector2 = pv[i]
		var m := 0.5 * (s.x * s.x + s.y * s.y)
		acc += m
		var ax := absf(s.x)
		var ay := absf(s.y)
		if ax > peak:
			peak = ax
		if ay > peak:
			peak = ay
	var rms := sqrt(acc / float(pv.size()))
	return Vector3(rms, peak, float(pv.size()))


func _drain_capture(cap: AudioEffectCapture) -> Vector3:
	# Returns Vector3(envelope_rms, peak_sample, total_frames). envelope_rms is the max RMS among
	# drained chunks this poll (better than energy-weighted mean when Godot delivers bursts between
	# empty polls — avoids rows of false 0.0 that look like dropouts in CSV).
	var den := 0.0
	var peak_max := 0.0
	var max_chunk_rms := 0.0
	while cap != null and cap.get_frames_available() > 0:
		var nf: int = mini(cap.get_frames_available(), CAPTURE_CHUNK_FRAMES_MAX)
		if nf <= 0:
			break
		if not cap.can_get_buffer(nf):
			break
		var pv: PackedVector2Array = cap.get_buffer(nf)
		if pv.is_empty():
			break
		var rp := _rms_peak_packed(pv)
		var chunk_rms: float = rp.x
		var chunk_peak: float = rp.y
		var zn: float = rp.z
		den += zn
		if chunk_rms > max_chunk_rms:
			max_chunk_rms = chunk_rms
		if chunk_peak > peak_max:
			peak_max = chunk_peak
	if den <= 0.0:
		return Vector3(0.0, peak_max, 0.0)
	return Vector3(max_chunk_rms, peak_max, den)


func _find_resonance_player(root: Node) -> Node:
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n.get_class() == "ResonancePlayer":
			return n
		for c in n.get_children():
			if c is Node:
				stack.push_back(c)
	return null


func _read_instr(player: Node) -> Dictionary:
	if player == null or not is_instance_valid(player):
		return {}
	if not player.has_method("get_audio_instrumentation"):
		return {}
	var d: Variant = player.call("get_audio_instrumentation")
	return d if d is Dictionary else {}


func _sparkline(values: Array[float], width: int) -> String:
	if values.is_empty() or width <= 0:
		return ""
	var mn := values[0]
	var mx := values[0]
	for v in values:
		if v < mn:
			mn = v
		if v > mx:
			mx = v
	var span := mx - mn
	if span < 1e-12:
		return "|" + "-".repeat(width) + "|"
	var chars := " .:-=+*#%@"
	var out := "|"
	for i in range(width):
		var t := float(i) / float(max(1, width - 1))
		var idx_v := int(round(t * float(values.size() - 1)))
		idx_v = clampi(idx_v, 0, values.size() - 1)
		var v: float = values[idx_v]
		var norm := (v - mn) / span
		var ci := int(norm * float(chars.length() - 1))
		ci = clampi(ci, 0, chars.length() - 1)
		out += chars[ci]
	out += "|"
	return out


func _analyze_tail_dip(
	label: String, rms_series: Array[float], times_ms: Array[float], zero_at_ms: float
) -> void:
	if zero_at_ms < 0.0 or rms_series.is_empty():
		print("[hallway_reverb_capture] dip_scan(%s): skipped." % label)
		return
	var window_ms := 350.0
	var i0 := -1
	var i1 := -1
	for i in range(times_ms.size()):
		var t := times_ms[i]
		if t >= zero_at_ms and i0 < 0:
			i0 = i
		if t <= zero_at_ms + window_ms:
			i1 = i
	if i0 < 0 or i1 < 0 or i1 <= i0:
		print("[hallway_reverb_capture] dip_scan(%s): no samples in window after zero_input." % label)
		return
	var seg: Array[float] = []
	for i in range(i0, i1 + 1):
		seg.append(rms_series[i])
	var trough_v := seg[0]
	var trough_idx := 0
	var peak_v := seg[0]
	for i in range(seg.size()):
		if seg[i] > peak_v:
			peak_v = seg[i]
		if seg[i] < trough_v:
			trough_v = seg[i]
			trough_idx = i
	var median_approx := seg[int(seg.size() / 2)]
	print(
		(
			"[hallway_reverb_capture] dip_scan(%s): after zero_input t=%.1fms window=%.0fms samples=%d"
			% [label, zero_at_ms, window_ms, seg.size()]
		)
	)
	print(
		(
			"[hallway_reverb_capture] dip_scan(%s): min=%.6f med=%.6f max=%.6f (min_idx_rel=%d)"
			% [label, trough_v, median_approx, peak_v, trough_idx]
		)
	)


class _CapSlot:
	var name: String
	var cap: AudioEffectCapture
	var bus_idx: int = -1
	var eff_idx: int = -1


var _slots: Array[_CapSlot] = []


func _attach_capture_slot(bus_name: StringName, tag: String) -> bool:
	var idx := AudioServer.get_bus_index(bus_name)
	if idx < 0:
		push_warning("[hallway_reverb_capture] Bus not found: %s" % String(bus_name))
		return false
	var cap := AudioEffectCapture.new()
	cap.buffer_length = 4.0
	var slot := AudioServer.get_bus_effect_count(idx)
	AudioServer.add_bus_effect(idx, cap, slot)
	var s := _CapSlot.new()
	s.name = tag
	s.cap = cap
	s.bus_idx = idx
	s.eff_idx = slot
	_slots.append(s)
	print(
		(
			"[hallway_reverb_capture] Capture '%s' on bus '%s' idx=%d slot=%d"
			% [tag, String(bus_name), idx, slot]
		)
	)
	return true


func _detach_all_captures() -> void:
	for i in range(_slots.size() - 1, -1, -1):
		var s: _CapSlot = _slots[i]
		if s.bus_idx >= 0 and s.eff_idx >= 0:
			AudioServer.remove_bus_effect(s.bus_idx, s.eff_idx)
			print("[hallway_reverb_capture] Removed capture '%s' bus_idx=%d" % [s.name, s.bus_idx])
	_slots.clear()


func _logical_clip_label(player: Node) -> String:
	return _clip_label_from_stream(player.call("get_stream"))


func _initialize() -> void:
	if not ClassDB.class_exists("ResonancePlayer"):
		push_error("[hallway_reverb_capture] ResonancePlayer missing — GDExtension not loaded.")
		quit(1)
		return
	if not ResourceLoader.exists(HALLWAY_SCENE):
		push_error("[hallway_reverb_capture] Missing scene %s" % HALLWAY_SCENE)
		quit(1)
		return
	call_deferred("_run")


func _run() -> void:
	var packed: PackedScene = load(HALLWAY_SCENE)
	if packed == null:
		push_error("[hallway_reverb_capture] Failed to load hallway scene.")
		quit(1)
		return
	var root: Node = packed.instantiate()
	if root == null:
		push_error("[hallway_reverb_capture] Failed to instantiate.")
		quit(1)
		return
	get_root().add_child(root)

	await _await_msec(SETTLE_MSEC)

	var foot := _find_resonance_player(root)
	if foot == null:
		push_error("[hallway_reverb_capture] No ResonancePlayer in scene.")
		root.queue_free()
		quit(1)
		return

	if foot.has_method("reset_audio_instrumentation"):
		foot.call("reset_audio_instrumentation")

	if not _attach_capture_slot(MASTER_BUS_NAME, "master"):
		push_error("[hallway_reverb_capture] Failed to attach Master capture.")
		root.queue_free()
		quit(1)
		return
	if not _attach_capture_slot(REVERB_BUS_NAME, "reverb"):
		push_warning("[hallway_reverb_capture] ResonanceReverb capture failed — continuing with Master only.")

	print("[hallway_reverb_capture] --- begin capture (%d ms) ---" % CAPTURE_MSEC)
	print("[hallway_reverb_capture] Sample rate (mix rate) = %d Hz" % AudioServer.get_mix_rate())

	var rms_m: Array[float] = []
	var rms_r: Array[float] = []
	var time_ms_samples: Array[float] = []
	var clips_log: Array[String] = []
	var zi_log: Array[int] = []
	var blk_log: Array[int] = []

	var t_start_usec := Time.get_ticks_usec()
	var next_print_usec := t_start_usec
	var zero_input_seen_ms := -1.0
	var last_clip := ""
	var prev_zi := -1
	var seen_audio_blocks := false

	while (Time.get_ticks_usec() - t_start_usec) / 1000 < CAPTURE_MSEC:
		await process_frame
		OS.delay_msec(2)

		var wall_ms := float(Time.get_ticks_usec() - t_start_usec) / 1000.0

		var clip_now := _logical_clip_label(foot)
		if clip_now != last_clip:
			print("[hallway_reverb_capture] STREAM_CHANGE wall_ms=%.1f clip=%s" % [wall_ms, clip_now])
			last_clip = clip_now

		var instr := _read_instr(foot)
		var zi := int(instr.get("zero_input_count", 0))
		var blk := int(instr.get("blocks_processed", 0))
		if blk > 0:
			seen_audio_blocks = true
		if prev_zi < 0:
			prev_zi = zi
		else:
			if seen_audio_blocks and prev_zi == 0 and zi > 0:
				if zero_input_seen_ms < 0.0:
					zero_input_seen_ms = wall_ms
				print(
					(
						"[hallway_reverb_capture] ZERO_INPUT_EDGE wall_ms=%.1f zero_input_count=%d blocks=%d mix_calls=%d"
						% [
							wall_ms,
							zi,
							blk,
							int(instr.get("mix_calls", 0)),
						]
					)
				)
			prev_zi = zi

		var r_block := 0.0
		var m_block := 0.0
		var peak_m := 0.0
		var peak_r := 0.0
		if _slots.size() > 0:
			var d0 := _drain_capture(_slots[0].cap)
			if d0.z > 0.0:
				m_block = d0.x
			peak_m = d0.y
		if _slots.size() > 1:
			var d1 := _drain_capture(_slots[1].cap)
			if d1.z > 0.0:
				r_block = d1.x
			peak_r = d1.y

		rms_m.append(m_block)
		rms_r.append(r_block)
		time_ms_samples.append(wall_ms)
		clips_log.append(clip_now)
		zi_log.append(zi)
		blk_log.append(blk)

		if Time.get_ticks_usec() >= next_print_usec:
			next_print_usec += PRINT_INTERVAL_MSEC * 1000
			var pv2 := _read_instr(foot)
			print(
				(
					"[hallway_reverb_capture] t=%6.0f ms clip=%-28s rms_master=%.5f rms_reverb=%.5f pk_m=%.5f pk_r=%.5f zi=%d blocks=%d"
					% [
						wall_ms,
						clip_now,
						m_block,
						r_block,
						peak_m,
						peak_r,
						int(pv2.get("zero_input_count", 0)),
						int(pv2.get("blocks_processed", 0)),
					]
				)
			)

	print("[hallway_reverb_capture] --- RMS Master (final mix, relative sparkline w=96) ---")
	print(_sparkline(rms_m, 96))
	if rms_r.size() > 0:
		print("[hallway_reverb_capture] --- RMS ResonanceReverb bus (wet chain output) ---")
		print(_sparkline(rms_r, 96))

	_analyze_tail_dip("master", rms_m, time_ms_samples, zero_input_seen_ms)
	_analyze_tail_dip("reverb_bus", rms_r, time_ms_samples, zero_input_seen_ms)

	if WRITE_CSV:
		var csv_path := "user://hallway_reverb_capture.csv"
		var fcsv := FileAccess.open(csv_path, FileAccess.WRITE)
		if fcsv:
			fcsv.store_line("time_ms,clip,rms_master,rms_reverb,zero_input,blocks_processed")
			for i in range(time_ms_samples.size()):
				var clip_esc := clips_log[i].replace(",", ";")
				fcsv.store_line(
					(
						"%s,%s,%s,%s,%s,%s"
						% [
						str(time_ms_samples[i]),
						clip_esc,
						str(rms_m[i]),
						str(rms_r[i]),
						str(zi_log[i]),
						str(blk_log[i]),
					]
					)
				)
			fcsv.close()
			var abs_csv := ProjectSettings.globalize_path(csv_path)
			print("[hallway_reverb_capture] CSV written: %s" % abs_csv)
		else:
			push_warning("[hallway_reverb_capture] Could not open %s for write." % csv_path)

	_detach_all_captures()
	root.queue_free()
	await _await_frames(3)
	print("[hallway_reverb_capture] Done.")
	quit(0)


func _await_frames(n: int) -> void:
	for _i in range(max(1, n)):
		await process_frame


func _await_msec(msec: int) -> void:
	var deadline_us: int = Time.get_ticks_usec() + max(0, msec) * 1000
	while Time.get_ticks_usec() < deadline_us:
		await process_frame
		OS.delay_msec(2)
