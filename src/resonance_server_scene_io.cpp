#include "resonance_constants.h"
#include "resonance_geometry.h"
#include "resonance_geometry_asset.h"
#include "resonance_scene_manager.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <chrono>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// scene_dirty: set when triangle count changes or geometry_update_throttle fires (notify_geometry_changed_assume_locked
// for transform-only updates), after the worker drains dynamic instanced-mesh transform queue, or explicitly via
// mark_scene_commit_pending_assume_locked. The worker runs iplSceneCommit only when scene_dirty is true.

namespace {

void refresh_geometry_recursive(Node* node) {
    if (!node)
        return;
    if (ResonanceGeometry* geom = Object::cast_to<ResonanceGeometry>(node))
        geom->refresh_geometry();
    for (int i = 0; i < node->get_child_count(); ++i)
        refresh_geometry_recursive(node->get_child(i));
}

} // namespace

void ResonanceServer::notify_geometry_changed_assume_locked(int triangle_delta) {
    if (!_ctx())
        return;
    bool should_mark_dirty = (triangle_delta != 0) || _should_run_throttled(geometry_update_throttle_counter, geometry_update_throttle);
    global_triangle_count += triangle_delta;
    if (should_mark_dirty)
        scene_dirty = true;
}

void ResonanceServer::notify_geometry_changed(int triangle_delta) {
    if (!_ctx())
        return;
    std::lock_guard<std::mutex> lock(simulation_mutex);
    notify_geometry_changed_assume_locked(triangle_delta);
}

void ResonanceServer::mark_scene_commit_pending_assume_locked() {
    if (!_ctx())
        return;
    scene_dirty.store(true, std::memory_order_release);
}

void ResonanceServer::mark_scene_commit_pending() {
    if (!_ctx())
        return;
    scene_dirty.store(true, std::memory_order_release);
}

void ResonanceServer::save_scene_data(String filename) {
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: save_scene_data is not supported when scene_type is Custom (no Phonon mesh data).");
        return;
    }
    std::lock_guard<std::mutex> lock(simulation_mutex);
    scene_manager_.save_scene_data(_ctx(), scene, filename);
}

void ResonanceServer::save_scene_obj(String file_base_name) {
    std::lock_guard<std::mutex> lock(simulation_mutex);
    if (!scene) {
        UtilityFunctions::push_warning("Nexus Resonance: No scene to export (save_scene_obj).");
        return;
    }
    String abs_path = file_base_name;
    if (file_base_name.begins_with("res://") || file_base_name.begins_with("user://")) {
        ProjectSettings* ps = ProjectSettings::get_singleton();
        if (ps)
            abs_path = ps->globalize_path(file_base_name);
    }
    // Steam Audio dumpObj writes the OBJ file to the given path as-is (no .obj appended).
    if (!abs_path.ends_with(".obj")) {
        abs_path = abs_path + ".obj";
    }
    Error write_err = ResonanceSceneManager::save_phonon_scene_obj_atomic(scene, abs_path);
    if (write_err != OK)
        UtilityFunctions::push_warning("Nexus Resonance: save_scene_obj failed (error " + String::num_int64(write_err) + ").");
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Scene exported to OBJ (base: " + file_base_name + ").");
    }
}

void ResonanceServer::load_scene_data(String filename) {
    if (!_ctx() || !simulator)
        return;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: load_scene_data is not supported when scene_type is Custom (Godot Physics).");
        return;
    }
    bool loaded = false;
    {
        std::lock_guard<std::mutex> lock(simulation_mutex);
        loaded = scene_manager_.load_scene_data(_ctx(), &scene, simulator, _tracer_type_for_mesh_operations(), _embree(), _radeon(), filename,
                                                &global_triangle_count);
    }
    if (loaded)
        call_deferred("_deferred_refresh_all_geometry_after_scene_load");
}

void ResonanceServer::refresh_all_geometry_from_scene_tree() {
    Engine* eng = Engine::get_singleton();
    if (!eng)
        return;
    MainLoop* ml = eng->get_main_loop();
    SceneTree* tree = Object::cast_to<SceneTree>(ml);
    if (!tree)
        return;
    Window* win = tree->get_root();
    if (!win)
        return;
    // SceneTree root is Window (extends Viewport -> Node); use Node entry for traversal.
    Node* root = static_cast<Node*>(static_cast<Viewport*>(win));
    refresh_geometry_recursive(root);
}

void ResonanceServer::_deferred_refresh_all_geometry_after_scene_load() {
    refresh_all_geometry_from_scene_tree();
    reset_spatial_audio_warmup_passes();
}

Error ResonanceServer::export_static_scene_to_asset(Node* scene_root, const String& p_path) {
    return scene_manager_.export_static_scene_to_asset(scene_root, p_path);
}

Error ResonanceServer::export_static_scene_to_obj(Node* scene_root, const String& file_base_name) {
    return scene_manager_.export_static_scene_to_obj(scene_root, file_base_name);
}

int64_t ResonanceServer::get_static_scene_hash(Node* scene_root) {
    return scene_manager_.get_static_scene_hash(scene_root, [this](const PackedByteArray& pba) { return _hash_probe_data(pba); });
}

int64_t ResonanceServer::get_geometry_asset_hash(const Ref<ResonanceGeometryAsset>& p_asset) const {
    if (!p_asset.is_valid() || !p_asset->is_valid())
        return 0;
    PackedByteArray data = p_asset->get_mesh_data();
    return static_cast<int64_t>(_hash_probe_data(data));
}
uint64_t ResonanceServer::_hash_probe_data(const PackedByteArray& pba) {
    const uint8_t* ptr = pba.ptr();
    return resonance::fnv1a_hash(ptr, static_cast<size_t>(pba.size()));
}

void ResonanceServer::enqueue_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh, const IPLMatrix4x4& transform) {
    if (!mesh || !_ctx())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        dynamic_instanced_transform_queue_[mesh] = transform;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    instrumentation_main_us_last_dynamic_transform_enqueue_.store(us, std::memory_order_relaxed);
    instrumentation_main_us_dynamic_transform_enqueue_.fetch_add(us, std::memory_order_relaxed);
    instrumentation_dynamic_transform_enqueue_events_.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::cancel_pending_dynamic_instanced_mesh_transform(IPLInstancedMesh mesh) {
    if (!mesh)
        return;
    std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
    dynamic_instanced_transform_queue_.erase(mesh);
}

void ResonanceServer::_apply_queued_dynamic_instanced_mesh_transforms_assume_locked() {
    if (!_ctx() || !scene)
        return;

    std::unordered_map<IPLInstancedMesh, IPLMatrix4x4> local;
    {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        local.swap(dynamic_instanced_transform_queue_);
    }

    if (local.empty())
        return;

    const bool static_wants_commit = scene_dirty.load(std::memory_order_acquire);
    const auto now = std::chrono::steady_clock::now();
    bool apply_now = static_wants_commit || dynamic_scene_commit_min_interval_ <= 0.0f;
    if (!apply_now) {
        const float dt_sec = std::chrono::duration<float>(now - last_dynamic_scene_commit_time_).count();
        apply_now = (dt_sec >= dynamic_scene_commit_min_interval_);
    }

    if (!apply_now) {
        std::lock_guard<std::mutex> qlock(dynamic_instanced_transform_queue_mutex_);
        for (auto& kv : local)
            dynamic_instanced_transform_queue_[kv.first] = kv.second;
        return;
    }

    for (auto& kv : local) {
        if (kv.first)
            iplInstancedMeshUpdateTransform(kv.first, scene, kv.second);
    }
    last_dynamic_scene_commit_time_ = now;
    mark_scene_commit_pending_assume_locked();
}
