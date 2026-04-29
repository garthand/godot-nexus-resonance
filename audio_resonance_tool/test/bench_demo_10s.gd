extends SceneTree

## Headless/main-loop bench: loads demo.tscn, runs ~10s wall clock, prints perf snapshot to stdout.
## Run: godot --path audio_resonance_tool -s res://test/bench_demo_10s.gd

var _timer_started: bool = false
var _samples: int = 0
var _sum_process_s: float = 0.0
var _sum_physics_s: float = 0.0
var _start_msec: int = 0


func _init() -> void:
	call_deferred("_boot")


func _boot() -> void:
	var err: Error = change_scene_to_file("res://Examples/scenes/demo.tscn")
	if err != OK:
		push_error("bench_demo_10s: change_scene_to_file failed: %s" % str(err))
		quit(1)
		return
	process_frame.connect(_on_process_frame)


func _on_process_frame() -> void:
	if not _timer_started:
		_timer_started = true
		_start_msec = Time.get_ticks_msec()
		var t: SceneTreeTimer = create_timer(10.0)
		t.timeout.connect(_on_bench_done)
	_samples += 1
	_sum_process_s += Performance.get_monitor(Performance.TIME_PROCESS)
	_sum_physics_s += Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS)


func _on_bench_done() -> void:
	process_frame.disconnect(_on_process_frame)
	var wall_ms: int = Time.get_ticks_msec() - _start_msec
	var wall_s: float = wall_ms / 1000.0
	var n: int = maxi(_samples, 1)
	print("=== bench_demo_10s: demo.tscn wall=%.2fs frames=%d ===" % [wall_s, _samples])
	print(
		(
			"avg TIME_PROCESS=%.3f ms  TIME_PHYSICS_PROCESS=%.3f ms  approx_FPS=%.1f"
			% [
				(_sum_process_s / float(n)) * 1000.0,
				(_sum_physics_s / float(n)) * 1000.0,
				float(_samples) / maxf(wall_s, 0.001),
			]
		)
	)
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null and srv.has_method("get_simulation_worker_timing"):
		var w: Dictionary = srv.get_simulation_worker_timing()
		print("worker_last_tick_us: ", w)
	var rt: Node = get_first_node_in_group("resonance_runtime")
	if rt != null:
		var mtu: Variant = rt.get("main_thread_last_tick_usec")
		print(
			(
				"main_thread_us: total=%s act=%s reinit=%s vp=%s tick=%s flush=%s"
				% [
					str(mtu),
					str(rt.get("main_thread_activator_usec")),
					str(rt.get("main_thread_reinit_usec")),
					str(rt.get("main_thread_viewport_usec")),
					str(rt.get("main_thread_tick_usec")),
					str(rt.get("main_thread_flush_usec")),
				]
			)
		)
	# Defer quit so native/audio teardown does not race the last simulation tick (avoids SIGSEGV on some builds).
	call_deferred("quit", 0)
