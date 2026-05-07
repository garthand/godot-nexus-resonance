# Nexus Resonance Unit Tests

## GDScript (GUT)

Tests use the [GUT](https://github.com/bitwes/Gut) framework. Unit tests live in `test/unit/`.

## Running Tests

### GUT Panel (Editor)

1. Open the project in Godot.
2. Open the GUT panel (bottom or via **Project > Tools > GUT**).
3. Set **Directories** to `res://test/unit` (or use `.gutconfig.json` defaults).
4. Click **Run All** to run all tests.

### Command Line (CI)

GUT requires the project's global script class cache to be populated before running tests. Run import once (or after adding GUT):

```powershell
& "C:\Godot\Godot_v4.6-stable_win64\Godot_v4.6-stable_win64.exe" --headless --import
```

Then run the tests:

```powershell
& "C:\Godot\Godot_v4.6-stable_win64\Godot_v4.6-stable_win64.exe" -s addons/gut/gut_cmdln.gd -gdir=res://test/unit -gexit
```

Or use the helper script (set `$env:GODOT_PATH` if godot is not in PATH):

```powershell
.\run_tests.ps1
```

- `--headless --import` – populate `global_script_class_cache.cfg` (required before first CLI run)
- `-gdir=res://test/unit` – directory containing test scripts
- `-gexit` – exit with code 0 when all pass, non‑zero on failure (for CI)

### Test Files

| Script | Covers |
|--------|--------|
| `test/unit/test_resonance_config.gd` | ResonanceConfig (defaults, get_config, sample rate, HRTF, effective realtime rays) |
| `test/unit/test_probe_data_loader.gd` | ResonanceProbeDataLoader (_parse_tres_data, extensions, handles_type, recognize_path) |
| `test/unit/test_probe_data_saver.gd` | ResonanceProbeDataSaver (_recognize, extensions, _save) |
| `test/unit/test_resonance_bake_settings.gd` | ResonanceBakeConfig (get_bake_params, pathing hash vs C++ format) |
| `test/unit/test_resonance_bake_runner.gd` | ResonanceBakeRunner / ResonanceBakeEstimates (probe count, bake time estimates) |
| `test/unit/test_resonance_bake_discovery.gd` | ResonanceBakeDiscovery (find_resonance_runtime, resolve_bake_node_for_volume) |
| `test/unit/test_resonance_bake_hashes.gd` | ResonanceBakeHashes (hash_dict, pathing hash, position/radius hash) |
| `test/unit/test_resonance_paths.gd` | ResonancePaths (audio_data_dir, static scene + probe batch path / extension) |
| `test/unit/test_resonance_fs_paths.gd` | ResonanceFsPaths (globalize, open/read/exists, probe needles, scene text match) |
| `test/unit/test_resonance_fs_paths_fixtures.gd` | Disk fixtures under `test/fixtures/` + probe reference detection |
| `test/unit/test_resonance_export_handler.gd` | ResonanceExportHandler.collect_scene_paths_for_obj, server null guard |
| `test/unit/test_resonance_scene_utils.gd` | ResonanceSceneUtils (static scene, volumes, exportable content) |
| `test/unit/test_resonance_server_access.gd` | ResonanceServerAccess singleton helpers |
| `test/unit/test_resonance_server_geometry_refresh.gd` | GDExtension geometry API surface (has_method checks) |
| `test/unit/test_resonance_runtime_perf_monitors.gd` | ResonanceRuntimePerfMonitors timing sums |
| `test/unit/test_resonance_player_config.gd` | ResonancePlayerConfig |
| `test/unit/test_resonance_player_init_retry.gd` | ResonancePlayer play API |
| `test/unit/test_resonance_player_polyphony.gd` | ResonancePlayer max_polyphony / instrumentation |

Support scripts (not listed as GUT suites by name): `test/unit/bake_discovery_runtime_stub.gd` (runtime script match tests).

### Headless smoke (AnimationPlayer + ResonancePlayer)

Godot **TYPE_AUDIO** uses `AudioStreamPolyphonic`; **ResonancePlayer** must use native polyphonic playback for that type (wrapper crashes on Windows). Use **`convert_animation_audio_tracks_at_runtime`**, method tracks with `play_animation_audio_clip`, or editor converters for Steam.

| Script | Purpose |
|--------|---------|
| `test/run_lightning_smoke.gd` | Loads `Examples/scenes/demo.tscn` and plays the `lightning` animation repeatedly (expects method-track audio on `ResonancePlayer_Thunder`). |
| `test/run_resonance_method_track_smoke.gd` | Minimal scene: METHOD track → `play_animation_audio_clip`. |
| `test/run_animation_audio_plain_smoke.gd` | Same as minimal but **AudioStreamPlayer3D** (native polyphonic reference). |
| `test/run_reverb_tail_smoke.gd` | Verifies the v0.9.13 fix that lets the reverb / pathing tail decay after a one-shot stream ends and after explicit `ResonancePlayer.stop()` (asserts `mix_calls` keep advancing and `zero_input_count > 0` during the tail window). |
| `test/run_hallway_reverb_capture.gd` | Headless diagnostic: loads `hallway_test/main.tscn`, captures **Master** and **ResonanceReverb** with `AudioEffectCapture`, logs logical clip via `get_stream()`, prints dual RMS/peak, sparklines, `zero_input` edge after blocks ran; writes `user://hallway_reverb_capture.csv` (rms columns = max chunk RMS per poll). Compare builds: `python scripts/compare_hallway_reverb_csv.py before.csv after.csv`. |

Example:

```powershell
& godot --path audio_resonance_tool --headless -s res://test/run_lightning_smoke.gd
```

### Fixtures

`test/fixtures/` holds minimal `.tscn` files for substring and PackedScene tests (probe path lines, `minimal_root_for_collect.tscn` for scene path collection).

## C++ Unit Tests

C++ tests live in `src/test/` and use [Catch2](https://github.com/catchorg/Catch2). From the repository root:

```bash
scons build_tests=1
./build/tests/nexus_resonance_tests   # Linux/macOS
.\build\tests\nexus_resonance_tests.exe  # Windows
```
