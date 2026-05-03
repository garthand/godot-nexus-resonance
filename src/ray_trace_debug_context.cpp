#include "ray_trace_debug_context.h"
#include "ray_trace_debug_intersect.h"
#include "resonance_debug_log.h"
#include "resonance_utils.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

void RayTraceDebugContext::set_debug_log_path(const char* path) {
    resonance::set_debug_log_path(path);
}

namespace {
static inline IPLVector3 transform_point(const IPLMatrix4x4& m, const IPLVector3& p) {
    IPLVector3 out;
    out.x = m.elements[0][0] * p.x + m.elements[0][1] * p.y + m.elements[0][2] * p.z + m.elements[0][3];
    out.y = m.elements[1][0] * p.x + m.elements[1][1] * p.y + m.elements[1][2] * p.z + m.elements[1][3];
    out.z = m.elements[2][0] * p.x + m.elements[2][1] * p.y + m.elements[2][2] * p.z + m.elements[2][3];
    return out;
}

static IPLMatrix4x4 identity_matrix() {
    IPLMatrix4x4 m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m.elements[i][j] = (i == j) ? 1.0f : 0.0f;
    return m;
}
} // namespace

RayTraceDebugContext::RayTraceDebugContext() {}

RayTraceDebugContext::~RayTraceDebugContext() {
    clear();
}

void RayTraceDebugContext::clear() {
    // Lock order: geometry_mutex_ first, then swap_mutex_ (see ray_trace_debug_context.h)
    std::lock_guard<std::mutex> lock(geometry_mutex_);
    std::lock_guard<std::mutex> lock2(swap_mutex_);
    triangles_.clear();
    materials_.clear();
    mesh_id_to_mat_offset_.clear();
    segment_buffers_[0].clear();
    segment_buffers_[1].clear();
    write_buffer_index_.store(0);
    next_mesh_id_ = 1;
}

int RayTraceDebugContext::register_mesh(const std::vector<IPLVector3>& vertices,
                                        const std::vector<IPLTriangle>& triangles,
                                        const IPLint32* material_indices,
                                        const IPLMatrix4x4* transform,
                                        const IPLMaterial* material) {
    (void)material_indices; // Per-triangle materials ignored; debug context uses one material per mesh.
    std::lock_guard<std::mutex> lock(geometry_mutex_);

    IPLMatrix4x4 xform = transform ? *transform : identity_matrix();
    int mat_offset = (int)materials_.size();
    if (material) {
        materials_.push_back(*material);
    } else {
        IPLMaterial def{};
        def.absorption[0] = def.absorption[1] = def.absorption[2] = 0.1f;
        def.scattering = 0.5f;
        def.transmission[0] = def.transmission[1] = def.transmission[2] = 0.1f;
        materials_.push_back(def);
    }

    const int mesh_id = next_mesh_id_++;
    mesh_id_to_mat_offset_[mesh_id] = mat_offset;

    for (size_t i = 0; i < triangles.size(); i++) {
        const IPLTriangle& t = triangles[i];
        TriangleData td;
        td.v0 = transform_point(xform, vertices[t.indices[0]]);
        td.v1 = transform_point(xform, vertices[t.indices[1]]);
        td.v2 = transform_point(xform, vertices[t.indices[2]]);

        IPLVector3 e1 = {td.v1.x - td.v0.x, td.v1.y - td.v0.y, td.v1.z - td.v0.z};
        IPLVector3 e2 = {td.v2.x - td.v0.x, td.v2.y - td.v0.y, td.v2.z - td.v0.z};
        float nx = e1.y * e2.z - e1.z * e2.y;
        float ny = e1.z * e2.x - e1.x * e2.z;
        float nz = e1.x * e2.y - e1.y * e2.x;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > kNormalLengthEpsilon) {
            td.normal.x = nx / len;
            td.normal.y = ny / len;
            td.normal.z = nz / len;
        } else {
            td.normal.x = td.normal.y = 0.0f;
            td.normal.z = 1.0f;
        }
        td.material_index = mat_offset;
        triangles_.push_back(td);
    }
    return mesh_id;
}

void RayTraceDebugContext::unregister_mesh(int mesh_id) {
    std::lock_guard<std::mutex> lock(geometry_mutex_);
    auto it = mesh_id_to_mat_offset_.find(mesh_id);
    if (it == mesh_id_to_mat_offset_.end())
        return;
    mesh_id_to_mat_offset_.erase(it);
    // Note: We don't remove triangles - that would require tracking which triangles
    // belong to which mesh. For debug, clearing all and re-registering on scene change
    // is simpler. Call clear() when scene is fully rebuilt.
}

bool RayTraceDebugContext::ray_triangle_intersect(const IPLRay& ray, float t_min, float t_max,
                                                  const TriangleData& tri, float& out_t, IPLVector3& out_normal) {
    const resonance::RayDebugTriangle rt{tri.v0, tri.v1, tri.v2, tri.normal};
    return resonance::ray_debug_ray_triangle_intersect(ray, t_min, t_max, rt, kRayTriangleEpsilon, out_t, out_normal);
}

void RayTraceDebugContext::trace_batch(IPLint32 num_rays, const IPLRay* rays,
                                       const IPLfloat32* min_distances, const IPLfloat32* max_distances,
                                       IPLHit* hits, int bounce_index) {
    std::lock_guard<std::mutex> lock(geometry_mutex_);
    for (IPLint32 i = 0; i < num_rays; i++) {
        const IPLRay& ray = rays[i];
        float t_min = min_distances ? min_distances[i] : 0.0f;
        float t_max = max_distances ? max_distances[i] : kDefaultMaxRayDistance;

        if (t_max <= t_min) {
            hits[i].distance = INFINITY;
            hits[i].triangleIndex = -1;
            hits[i].objectIndex = -1;
            hits[i].materialIndex = -1;
            hits[i].normal = {0, 0, 1};
            hits[i].material = nullptr;
            continue;
        }

        float best_t = t_max + 1.0f;
        int best_tri = -1;
        IPLVector3 best_normal = {0, 0, 1};

        for (size_t k = 0; k < triangles_.size(); k++) {
            float t;
            IPLVector3 n;
            if (ray_triangle_intersect(ray, t_min, t_max, triangles_[k], t, n) && t < best_t) {
                best_t = t;
                best_tri = (int)k;
                best_normal = n;
            }
        }

        if (best_tri >= 0) {
            int mat_idx = triangles_[best_tri].material_index;
            hits[i].distance = best_t;
            hits[i].triangleIndex = best_tri;
            hits[i].objectIndex = 0;
            hits[i].materialIndex = mat_idx;
            hits[i].normal = best_normal;
            hits[i].material = (mat_idx >= 0 && mat_idx < (int)materials_.size())
                                   ? &materials_[mat_idx]
                                   : (materials_.empty() ? nullptr : &materials_[0]);
        } else {
            hits[i].distance = INFINITY;
            hits[i].triangleIndex = -1;
            hits[i].objectIndex = -1;
            hits[i].materialIndex = -1;
            hits[i].normal = {0, 0, 1};
            hits[i].material = nullptr;
        }
    }
}

void RayTraceDebugContext::push_rays_for_viz(IPLint32 num_rays, const IPLRay* rays,
                                             const IPLHit* hits, int bounce_index) {
    int idx = write_buffer_index_.load(std::memory_order_relaxed);
    std::vector<RayDebugSegment>& buf = segment_buffers_[idx];
    for (IPLint32 i = 0; i < num_rays; i++) {
        if (buf.size() >= (size_t)resonance::kRayDebugMaxSegments)
            break;

        const IPLRay& ray = rays[i];
        const IPLHit& hit = hits[i];

        RayDebugSegment seg;
        seg.from_x = ray.origin.x;
        seg.from_y = ray.origin.y;
        seg.from_z = ray.origin.z;
        seg.bounce_index = bounce_index;

        if (hit.distance < INFINITY && hit.distance > 0.0f) {
            seg.to_x = ray.origin.x + ray.direction.x * hit.distance;
            seg.to_y = ray.origin.y + ray.direction.y * hit.distance;
            seg.to_z = ray.origin.z + ray.direction.z * hit.distance;
        } else {
            float far = resonance::kRayDebugDefaultMissRayLength;
            seg.to_x = ray.origin.x + ray.direction.x * far;
            seg.to_y = ray.origin.y + ray.direction.y * far;
            seg.to_z = ray.origin.z + ray.direction.z * far;
        }
        buf.push_back(seg);
    }
}

void RayTraceDebugContext::get_segments_for_godot(Array& out_segments) {
    std::unique_lock<std::mutex> lock(swap_mutex_, std::try_to_lock);
    if (!lock.owns_lock())
        return;

    int write_idx = write_buffer_index_.load(std::memory_order_relaxed);
    int read_idx = 1 - write_idx;
    std::vector<RayDebugSegment>& read_buf = segment_buffers_[read_idx];

    auto is_finite_vec3 = [](float a, float b, float c) {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
    };
    out_segments.clear();
    for (const RayDebugSegment& seg : read_buf) {
        if (!is_finite_vec3(seg.from_x, seg.from_y, seg.from_z) || !is_finite_vec3(seg.to_x, seg.to_y, seg.to_z))
            continue;
        Dictionary d;
        d[StringName("from")] = Vector3(seg.from_x, seg.from_y, seg.from_z);
        d[StringName("to")] = Vector3(seg.to_x, seg.to_y, seg.to_z);
        d[StringName("bounce")] = seg.bounce_index;
        out_segments.push_back(d);
    }
    read_buf.clear();
    write_buffer_index_.store(read_idx, std::memory_order_release);
}

void RayTraceDebugContext::trace_reflection_rays_for_viz(const IPLVector3& origin, int num_rays,
                                                         float max_distance, Array& out_segments) {
    out_segments.clear();
    if (num_rays <= 0 || max_distance <= 0.0f)
        return;

    std::lock_guard<std::mutex> lock(geometry_mutex_);
    if (triangles_.empty())
        return;

    static constexpr float kPi = 3.14159265f;
    const float n = static_cast<float>(num_rays);
    for (int i = 0; i < num_rays && (int)out_segments.size() < resonance::kRayDebugMaxSegments; i++) {
        // Uniform on sphere: z uniform in [-1,1], azimuth phi uniform in [0, 2pi).
        const float z = 1.0f - (2.0f * (static_cast<float>(i) + 0.5f) / n);
        const float phi = 2.0f * kPi * static_cast<float>(i) / n;
        const float r_xy = std::sqrt(std::max(0.0f, 1.0f - z * z));
        IPLVector3 dir{r_xy * std::cos(phi), r_xy * std::sin(phi), z};
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len > 1e-8f) {
            dir.x /= len;
            dir.y /= len;
            dir.z /= len;
        } else {
            dir = IPLVector3{0.0f, 0.0f, 1.0f};
        }
        IPLRay ray = {origin, dir};
        float t_min = kMinRayT;
        float t_max = max_distance;

        float best_t = t_max + 1.0f;
        IPLVector3 best_normal = {0, 0, 1};
        for (size_t k = 0; k < triangles_.size(); k++) {
            float t;
            IPLVector3 hit_normal;
            if (ray_triangle_intersect(ray, t_min, t_max, triangles_[k], t, hit_normal) && t < best_t) {
                best_t = t;
                best_normal = hit_normal;
            }
        }

        if (best_t <= t_max) {
            float to_x = origin.x + dir.x * best_t;
            float to_y = origin.y + dir.y * best_t;
            float to_z = origin.z + dir.z * best_t;
            if (std::isfinite(to_x) && std::isfinite(to_y) && std::isfinite(to_z)) {
                Dictionary d;
                d[StringName("from")] = Vector3(origin.x, origin.y, origin.z);
                d[StringName("to")] = Vector3(to_x, to_y, to_z);
                d[StringName("bounce")] = 0;
                out_segments.push_back(d);
            }
        }
    }
}

} // namespace godot
