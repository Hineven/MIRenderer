/*
 * Created: 2025/4/14
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include <random>
#include <array>
#include <algorithm>
#include <cmath>
#include "renderer/mi_static_mesh.h"

#include <gtest/internal/gtest-port.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_geometry.h"
#include "renderer/mi_material.h"
#include "renderer/mi_renderer_view.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_texture.h"
#include "rhi/rhi_as.h"
#include <renderer/r_light_cluster_hiearchy.h>

#include "shaders/shared/SharedLight.hlsl"

#include "rdg/rdg_ray_tracing_registry.h"

MI_NAMESPACE_BEGIN

RayTracedRenderableClassRegistrator<StaticMeshInstance> StaticMeshInstance::kClassRegistrator("StaticMesh", "StaticMesh");

CVar<int> CVar_MaxMeshLightsPerGeometry(
    "r.mesh_light.max_lights_per_geometry",
    "Maximum number of mesh lights generated for each emissive geometry. <= 0 means no explicit limit.",
    128
);

namespace {

constexpr float kOverlapEpsilon = 1e-6f;
constexpr uint32_t kMeshLightTriangleStream_Triangle = 0;
constexpr uint32_t kMeshLightTriangleStream_Hash = 1;
constexpr uint32_t kMeshLightTriangleStream_BakedData = 2;
constexpr uint32_t kMeshLightClusterStream_Header = 0;
constexpr uint32_t kMeshLightClusterStream_Node = 1;
constexpr uint32_t kMeshLightInstanceClusterStream_Header = 0;
constexpr uint32_t kMeshLightInstanceClusterStream_Node = 1;

template<typename T>
bool ReallocateUberBufferIfNeeded(
    RHICommandQueueGraphics & queue,
    TRef<DeviceUberBufferAllocation> & allocation,
    DeviceUberBufferInterface * uber_buffer,
    size_t required_count,
    const char * debug_name
) {
    size_t required_size = required_count * sizeof(T);
    if (required_size == 0) {
        if (allocation) {
            Helpers::Clear_Async(queue, allocation->GetRHI());
            allocation.SafeRelease();
        }
        return true;
    }

    bool need_reallocate = !allocation
        || allocation->GetSize() < required_size
        || allocation->GetSize() > required_size * 2;

    if (!need_reallocate) {
        return true;
    }

    if (allocation) {
        Helpers::Clear_Async(queue, allocation->GetRHI());
        allocation.SafeRelease();
    }

    auto alloc_res = uber_buffer->AllocateRefCounted<T>(required_count);
    if (!alloc_res) {
        MI_WARN("Failed to allocate {}.", debug_name);
        return false;
    }
    allocation = alloc_res;
    return true;
}

bool ReallocateUberBufferArrayIfNeeded(
    RHICommandQueueGraphics & queue,
    TRef<DeviceUberBufferArrayAllocation> & allocation,
    DeviceUberBufferArrayInterface * uber_buffer_array,
    size_t required_count,
    const char * debug_name
) {
    if (required_count == 0) {
        if (allocation) {
            for (uint32_t i = 0; i < uber_buffer_array->GetNumStreams(); ++i) {
                Helpers::Clear_Async(queue, allocation->GetRHI(i));
            }
            allocation.SafeRelease();
        }
        return true;
    }

    bool need_reallocate = !allocation
        || allocation->GetElementCount() < required_count
        || allocation->GetElementCount() > required_count * 2;

    if (!need_reallocate) {
        return true;
    }

    if (allocation) {
        for (uint32_t i = 0; i < uber_buffer_array->GetNumStreams(); ++i) {
            Helpers::Clear_Async(queue, allocation->GetRHI(i));
        }
        allocation.SafeRelease();
    }

    auto alloc_res = uber_buffer_array->AllocateRefCounted((uint32_t)required_count);
    if (!alloc_res.first) {
        MI_WARN("Failed to allocate {}.", debug_name);
        return false;
    }
    allocation = alloc_res.first;
    return true;
}

template<typename T>
bool UploadPackedVector(
    RHICommandQueueGraphics & queue,
    TRef<DeviceUberBufferAllocation> & allocation,
    DeviceUberBufferInterface * uber_buffer,
    const std::vector<T> & data,
    const char * debug_name
) {
    if (!ReallocateUberBufferIfNeeded<T>(queue, allocation, uber_buffer, data.size(), debug_name)) {
        return false;
    }
    if (!data.empty()) {
        Helpers::Upload_Async(queue, allocation->GetRHI(), data.data(), data.size() * sizeof(T));
    }
    return true;
}

void CollectMeshLightClusterDepthLevels(
    const MeshLightClusterHierarchy & hierarchy,
    std::vector<std::vector<uint32_t>> & out_levels
) {
    out_levels.clear();
    if (hierarchy.root_node.bIsLeaf() || hierarchy.nodes.empty()) {
        return;
    }

    std::vector<uint32_t> current_level {hierarchy.root_node.Index()};
    while (!current_level.empty()) {
        out_levels.push_back(current_level);
        std::vector<uint32_t> next_level;
        for (uint32_t cluster_idx : current_level) {
            mi_check(cluster_idx < hierarchy.nodes.size(), "Cluster index out of bounds while collecting hierarchy levels.");
            auto const & node = hierarchy.nodes[cluster_idx];
            if (!node.L.bIsLeaf()) {
                next_level.push_back(node.L.Index());
            }
            if (!node.R.bIsLeaf()) {
                next_level.push_back(node.R.Index());
            }
        }
        current_level = std::move(next_level);
    }
}

FORCEINLINE float Cross2D(glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    glm::vec2 ab = b - a;
    glm::vec2 ac = c - a;
    return ab.x * ac.y - ab.y * ac.x;
}

bool TryIntersectEdgeWithScanlineY(glm::vec2 p0, glm::vec2 p1, float scan_y, float & out_x) {
    if (std::abs(p0.y - p1.y) <= kOverlapEpsilon) {
        return false;
    }

    float edge_min_y = std::min(p0.y, p1.y);
    float edge_max_y = std::max(p0.y, p1.y);
    if (scan_y < edge_min_y || scan_y >= edge_max_y) {
        return false;
    }

    float t = (scan_y - p0.y) / (p1.y - p0.y);
    out_x = p0.x + t * (p1.x - p0.x);
    return true;
}

bool HasAnyEmissiveTexelInTriangle(Texture * emissive_map, glm::vec2 uv0, glm::vec2 uv1, glm::vec2 uv2) {
    if (!emissive_map) {
        return true;
    }

    uint32_t width = emissive_map->GetWidth();
    uint32_t height = emissive_map->GetHeight();
    if (width == 0 || height == 0) {
        return true;
    }
    if (emissive_map->GetBinary().empty()) {
        return true;
    }

    auto uv_in_01 = [](glm::vec2 uv) {
        return uv.x >= 0.f && uv.x <= 1.f && uv.y >= 0.f && uv.y <= 1.f;
    };
    if (!uv_in_01(uv0) || !uv_in_01(uv1) || !uv_in_01(uv2)) {
        return true;
    }

    glm::vec2 p0 = uv0 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);
    glm::vec2 p1 = uv1 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);
    glm::vec2 p2 = uv2 * glm::vec2((float)width, (float)height) - glm::vec2(0.5f);

    float tri_min_y_f = std::min({p0.y, p1.y, p2.y});
    float tri_max_y_f = std::max({p0.y, p1.y, p2.y});
    int32_t min_y = std::clamp((int32_t)std::ceil(tri_min_y_f), 0, (int32_t)height - 1);
    int32_t max_y = std::clamp((int32_t)std::floor(tri_max_y_f), 0, (int32_t)height - 1);

    if (min_y > max_y) {
        return false;
    }

    for (int32_t y = min_y; y <= max_y; ++y) {
        float scan_y = (float)y;
        std::array<float, 3> intersections {};
        int intersection_count = 0;

        float hit_x;
        if (TryIntersectEdgeWithScanlineY(p0, p1, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }
        if (TryIntersectEdgeWithScanlineY(p1, p2, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }
        if (TryIntersectEdgeWithScanlineY(p2, p0, scan_y, hit_x)) {
            intersections[intersection_count++] = hit_x;
        }

        if (intersection_count < 2) {
            continue;
        }

        float x_min = intersections[0];
        float x_max = intersections[1];
        if (x_min > x_max) {
            std::swap(x_min, x_max);
        }
        if (intersection_count == 3) {
            float x2 = intersections[2];
            if (x2 < x_min) {
                x_min = x2;
            } else if (x2 > x_max) {
                x_max = x2;
            }
        }

        int32_t x_start = std::clamp((int32_t)std::ceil(x_min), 0, (int32_t)width - 1);
        int32_t x_end = std::clamp((int32_t)std::floor(x_max), 0, (int32_t)width - 1);

        if (x_start > x_end) {
            continue;
        }

        for (int32_t x = x_start; x <= x_end; ++x) {
            auto emissive = emissive_map->Load((uint32_t)x, (uint32_t)y);
            if (emissive.x > 0.f || emissive.y > 0.f || emissive.z > 0.f) {
                return true;
            }
        }
    }
    return false;
}

}

DeviceStaticMesh::DeviceStaticMesh(DeviceBindlessResourceAllocator * in_allocator) {
    allocator_ = in_allocator;
    slot_ = allocator_->AllocateStaticMeshSlotKeeper();
}

DeviceStaticMesh::~DeviceStaticMesh() {
    // Slot is freed (delayed) by SlotKeeper.
}

void StaticMesh::AddMeshPrimitive(TRef<Geometry> geom, TRef<Material> mat) {
    assert(mat->GetDeviceMaterial() && "Material must have a device material. Call UpdateOnDevice() on the material first.");
    assert(geometries_.size() < kMaxNumGeometries && "Exceeded maximum number of geometries per static mesh.");
    geometries_.push_back(geom);
    materials_.push_back(mat);
    aabb_ = AABB::Merge(aabb_, geom->GetAABB());
    has_double_sided_material_ = has_double_sided_material_ || mat->IsDoubleSided();
    SetDirty(true);
}

void StaticMesh::ClearMeshPrimitives() {
    geometries_.clear();
    materials_.clear();
    device_static_mesh_ = {};
    aabb_ = {};
    light_hierarchy_records_.clear();
    mesh_light_triangles_.clear();
    mesh_light_triangle_hashes_.clear();
    mesh_light_triangle_baked_data_.clear();
    mesh_light_level_headers_.clear();
    mesh_lights_.clear();
    mesh_light_instance_template_.clear();
    light_triangle_streams_.SafeRelease();
    light_cluster_streams_.SafeRelease();
    light_level_headers_.SafeRelease();
    lights_.SafeRelease();
    has_double_sided_material_ = false;
    SetDirty(true);
}

void StaticMesh::UpdateOnDevice_Async (DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!IsDirty()) return;
    if (IsEmpty()) return;
    if (!device_static_mesh_) {
        device_static_mesh_ = new DeviceStaticMesh(alloc);
        mi_check(device_static_mesh_.IsValid(), "Failed to allocate static mesh slot. Maybe too many static meshes?");
    }

    // Update geometry - material pairs
    auto desired_num_pairs = (uint32_t)geometries_.size();
    if (!device_static_mesh_->geometry_material_indices_
        || device_static_mesh_->geometry_material_indices_->GetRHI().size / (sizeof(uint32_t) * 2) != desired_num_pairs) {
        device_static_mesh_->geometry_material_indices_ = alloc->GetStaticMeshDescriptionUberBuffer()->AllocateRefCounted(
            desired_num_pairs * sizeof(uint32_t) * 2
        ).first;
    }
    std::vector<uint32_t> data;
    data.reserve(geometries_.size() * 2);
    for (int i = 0; i < (int)geometries_.size(); i++) {
        auto geom = geometries_[i];
        auto mat = materials_[i];
        data.emplace_back(geom->GetDeviceGeometry()->GetIndex());
        data.emplace_back(mat->GetDeviceMaterial()->GetIndex());
    }
    Helpers::Upload_Async(queue,
        device_static_mesh_->geometry_material_indices_->GetRHI(), data.data(), sizeof(uint32_t) * data.size()
    );
    // Update header
    auto header = StaticMeshHeader {
        (uint32_t)(device_static_mesh_->geometry_material_indices_->GetRHI().offset / (2 * sizeof(uint32_t))),
        (uint32_t)geometries_.size(),
    };
    header_ = header;
    Helpers::Upload_Async(queue,
        alloc->GetStaticMeshHeaderBuffer(),
        sizeof(StaticMeshHeader) * device_static_mesh_->GetIndex(),
        header
    );
    // Update BLAS if needed
    if (!IsRayTraced()) {
        device_static_mesh_->BLAS_ = {};
    } else {
        // All materials must be deferred
        for (auto mat : materials_) {
            mi_assert(!mat->IsForward(), "All materials in a ray-traced static mesh must be deferred materials.");
        }
        if (geometries_.empty()) {
            // No geometries, release BLAS. NullDescriptorSet feature will take care of this case.
            device_static_mesh_->BLAS_ = {};
        } else {
            // Update acceleration structure for raytracing
            auto geometries = queue.Allocate<RHIASGeometry[]>(geometries_.size());
            RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
            build_flags = build_flags | (dynamic_ ? RHIAccelerationStructureBuildFlagBits::kAllowUpdate : RHIAccelerationStructureBuildFlagBits::kNone);
            for (const auto& [i, geometry] : std::views::enumerate(geometries_)) {
                auto material = materials_[i];
                RHIASGeometryFlags geometry_flags = material->IsOpaque() ? RHIASGeometryFlagBits::kOpaque : RHIASGeometryFlagBits::kNone;
                auto device_geom = geometry->GetDeviceGeometry();
                geometries[i] = RHIASGeometry{
                    RHIASGeometryType::kTriangles,
                    geometry_flags,
                    {
                        device_geom->GetDeviceVertexBuffer()->GetRHI(),
                        sizeof(DefaultStaticMeshVertex),
                        (uint32_t)device_geom->GetVertexCount(),
                        RHIVertexAttributeFormatType::k3xFp32,
                        RHIBufferSpan{
                            device_geom->GetDeviceIndexBuffer()->GetRHI().buffer,
                            device_geom->GetDeviceIndexBuffer()->GetRHI().offset,
                            device_geom->GetDeviceIndexBuffer()->GetRHI().size
                        },
                        (uint32_t)device_geom->GetIndexCount(),
                        RHIIndexType::kUint32
                    }
                };
            }
            if (!device_static_mesh_->BLAS_) {
                // Create
                device_static_mesh_->BLAS_ = RHI::Get().CreateAccelerationStructure(
                    RHIAccelerationStructureType::kBottomLevel
                );
            }
            auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                RHIAccelerationStructureType::kBottomLevel,
                build_flags,
                RHIAccelerationStructureBuildMode::kUpdate,
                device_static_mesh_->BLAS_.Raw(),
                device_static_mesh_->BLAS_.Raw(),
                {geometries, geometries_.size()}, {}, {}
            };
            auto sizes = device_static_mesh_->BLAS_->GetBuildSizes(build_info);
            bool updated = false;
            // Try to update
            if (dynamic_ && sizes.acceleration_structure_size <= device_static_mesh_->BLAS_->GetSize()) {
                queue.AccelerationStructureBarrier(
                    device_static_mesh_->BLAS_.Raw(),
                    RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                    RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                    RHIGPUAccessFlagBits::kAccelerationStructureRW,
                    RHIGPUAccessFlagBits::kAccelerationStructureRW
                );
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
                updated = true;
            }
            if (!updated) {
                // Need rebuild
                // Re-create
                device_static_mesh_->BLAS_->Create(sizes.acceleration_structure_size);
                // Build
                build_info.mode = RHIAccelerationStructureBuildMode::kBuild;
                build_info.src_acceleration_structure = {};
                build_info.dst_acceleration_structure = device_static_mesh_->BLAS_.Raw();
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
            }
            // Barrier
            queue.AccelerationStructureBarrier(
                device_static_mesh_->BLAS_.Raw(),
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kAccelerationStructureWrite,
                RHIGPUAccessFlagBits::kAccelerationStructureRW
            );
        }
    }
    SetDirty(false);
}

void StaticMesh::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("StaticMesh::UpdateOnDevice");
}

void StaticMesh::RebuildLightClusterHierarchy_CPU(const MeshLightClusterBuildConfig & config) {
    if (!light_hierarchy_dirty_) {
        return;
    }

    light_hierarchy_records_.clear();
    mesh_light_instance_template_.clear();

    if (IsEmpty()) {
        light_hierarchy_dirty_ = false;
        light_hierarchy_device_dirty_ = true;
        return;
    }

    light_hierarchy_records_.reserve(geometries_.size());
    for (uint32_t i = 0; i < (uint32_t)geometries_.size(); ++i) {
        auto * geom = geometries_[i].Raw();
        auto * mat = materials_[i].Raw();
        if (!geom || !mat || !mat->IsEmissive()) {
            continue;
        }

        auto hierarchy_opt = BuildMeshLightClusterHierarchy(*geom, *mat, config);
        if (!hierarchy_opt.has_value()) {
            MI_WARN("Failed to build light cluster hierarchy for static mesh geometry {}.", i);
            continue;
        }

        if (hierarchy_opt->Empty()) {
            continue;
        }

        std::vector<std::vector<uint32_t>> cluster_levels;
        CollectMeshLightClusterDepthLevels(*hierarchy_opt, cluster_levels);
        std::vector<uint32_t> depth_level_cluster_counts;
        depth_level_cluster_counts.reserve(cluster_levels.size());
        for (auto const & level : cluster_levels) {
            depth_level_cluster_counts.push_back((uint32_t)level.size());
        }

        light_hierarchy_records_.push_back(MeshLightHierarchyRecord{
            i,
            std::move(hierarchy_opt.value()),
            std::move(depth_level_cluster_counts)
        });
    }

    light_hierarchy_dirty_ = false;
    light_hierarchy_device_dirty_ = true;
}

void StaticMesh::UploadLightClusterHierarchy_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    RebuildLightClusterHierarchy_CPU();
    if (!light_hierarchy_device_dirty_) {
        return;
    }

    if (light_hierarchy_records_.empty()) {
        mesh_light_triangles_.clear();
        mesh_light_triangle_hashes_.clear();
        mesh_light_triangle_baked_data_.clear();
        mesh_light_level_headers_.clear();
        mesh_lights_.clear();
        mesh_light_instance_template_.clear();
        light_triangle_streams_.SafeRelease();
        light_cluster_streams_.SafeRelease();
        if (light_level_headers_) {
            Helpers::Clear_Async(queue, light_level_headers_->GetRHI());
            light_level_headers_.SafeRelease();
        }
        if (lights_) {
            Helpers::Clear_Async(queue, lights_->GetRHI());
            lights_.SafeRelease();
        }
        light_hierarchy_device_dirty_ = false;
        return;
    }

    size_t total_triangles = 0;
    size_t total_headers = 0;
    size_t total_nodes = 0;
    for (const auto & record : light_hierarchy_records_) {
        total_triangles += record.hierarchy.triangles.size();
        total_headers += record.hierarchy.headers.size();
        total_nodes += record.hierarchy.nodes.size();
    }

    std::vector<MeshLightTriangle> packed_triangles;
    std::vector<MeshLightTriangleHash> packed_triangle_hashes;
    std::vector<MeshLightTriangleBakedData> packed_triangle_baked_data;
    std::vector<MeshLightClusterHeader> packed_headers;
    std::vector<MeshLightClusterNode> packed_nodes;
    std::vector<MeshLightLevelHeader> packed_level_headers;
    std::vector<MeshLight> packed_mesh_lights;
    std::vector<MeshLightInstance> packed_mli_template;
    packed_triangles.reserve(total_triangles);
    packed_triangle_hashes.reserve(total_triangles);
    packed_triangle_baked_data.reserve(total_triangles);
    packed_headers.reserve(total_headers);
    packed_nodes.reserve(total_nodes);
    packed_mesh_lights.reserve(light_hierarchy_records_.size());
    packed_mli_template.reserve(light_hierarchy_records_.size());

    for (const auto & record : light_hierarchy_records_) {
        auto const & hierarchy = record.hierarchy;
        uint32_t mesh_light_index = (uint32_t)packed_mesh_lights.size();
        uint32_t triangle_base = (uint32_t)packed_triangles.size();
        uint32_t cluster_base = (uint32_t)packed_nodes.size();

        packed_triangles.insert(packed_triangles.end(), hierarchy.triangles.begin(), hierarchy.triangles.end());
        packed_triangle_hashes.insert(packed_triangle_hashes.end(), hierarchy.triangle_hashes.begin(), hierarchy.triangle_hashes.end());
        packed_triangle_baked_data.insert(packed_triangle_baked_data.end(), hierarchy.triangle_baked_data.begin(), hierarchy.triangle_baked_data.end());

        std::vector<std::vector<uint32_t>> cluster_levels;
        CollectMeshLightClusterDepthLevels(hierarchy, cluster_levels);
        uint32_t level_base = (uint32_t)packed_level_headers.size();
        uint32_t remapped_cluster_cursor = 0;
        std::vector<uint32_t> old_to_new_cluster(hierarchy.nodes.size(), UINT32_MAX);
        for (auto const & level : cluster_levels) {
            for (uint32_t old_cluster_idx : level) {
                old_to_new_cluster[old_cluster_idx] = remapped_cluster_cursor++;
            }
        }

        std::vector<uint32_t> new_to_old_cluster(remapped_cluster_cursor, UINT32_MAX);
        for (uint32_t old_cluster_idx = 0; old_cluster_idx < old_to_new_cluster.size(); ++old_cluster_idx) {
            uint32_t new_cluster_idx = old_to_new_cluster[old_cluster_idx];
            if (new_cluster_idx != UINT32_MAX) {
                new_to_old_cluster[new_cluster_idx] = old_cluster_idx;
            }
        }

        for (uint32_t new_cluster_idx = 0; new_cluster_idx < new_to_old_cluster.size(); ++new_cluster_idx) {
            uint32_t old_cluster_idx = new_to_old_cluster[new_cluster_idx];
            mi_check(old_cluster_idx != UINT32_MAX, "Missing remapped cluster index.");

            auto header = hierarchy.headers[old_cluster_idx];
            auto node = hierarchy.nodes[old_cluster_idx];
            header.MeshLightIndex = mesh_light_index;
            if (!node.L.bIsLeaf()) {
                node.L.SetIndex(old_to_new_cluster[node.L.Index()]);
            }
            if (!node.R.bIsLeaf()) {
                node.R.SetIndex(old_to_new_cluster[node.R.Index()]);
            }
            packed_headers.push_back(header);
            packed_nodes.push_back(node);
        }

        uint32_t level_cluster_offset = 0;
        for (auto const & level : cluster_levels) {
            packed_level_headers.push_back(MeshLightLevelHeader {
                (uint32_t)level.size(),
                level_cluster_offset
            });
            level_cluster_offset += (uint32_t)level.size();
        }

        MeshLight ml {};
        ml.ClusterOffset = cluster_base;
        ml.TriangleOffset = triangle_base;
        ml.LevelOffset = level_base;
        ml.StaticMeshIndex = device_static_mesh_ ? device_static_mesh_->GetIndex() : UINT32_MAX;
        ml.StaticMeshDescriptionIndex = record.static_mesh_local_geometry_index;
        ml.NumLevels = (uint32_t)cluster_levels.size();
        ml.NumClusters = (uint32_t)hierarchy.nodes.size();
        ml.NumTriangles = (uint32_t)hierarchy.triangles.size();
        packed_mesh_lights.push_back(ml);

        MeshLightInstance instance {};
        instance.MeshLightIndex = mesh_light_index;
        instance.RenderableIndex = UINT32_MAX;
        instance.MeshLightInstanceClusterOffset = MakeMeshLightInstanceElementOffset(ml.NumLevels == 0, 0);
        instance.MeshLightInstanceTriangleOffset = 0;
        packed_mli_template.push_back(instance);
    }

    bool upload_ok = true;
    upload_ok &= ReallocateUberBufferArrayIfNeeded(
        queue, light_triangle_streams_, alloc->GetMeshLightTriangleUberBufferArray(), packed_triangles.size(), "mesh light triangle stream buffers"
    );
    upload_ok &= ReallocateUberBufferArrayIfNeeded(
        queue, light_cluster_streams_, alloc->GetMeshLightClusterUberBufferArray(), packed_headers.size(), "mesh light cluster stream buffers"
    );
    upload_ok &= ReallocateUberBufferIfNeeded<MeshLightLevelHeader>(
        queue, light_level_headers_, alloc->GetMeshLightLevelHeaderUberBuffer(), packed_level_headers.size(), "mesh light level header uber buffer"
    );
    upload_ok &= ReallocateUberBufferIfNeeded<MeshLight>(
        queue, lights_, alloc->GetMeshLightUberBuffer(), packed_mesh_lights.size(), "mesh light uber buffer"
    );

    if (upload_ok) {
        uint32_t mesh_light_base = lights_ ? (uint32_t)(lights_->GetRHI().offset / sizeof(MeshLight)) : 0;
        uint32_t triangle_base = light_triangle_streams_ ? light_triangle_streams_->GetElementOffset() : 0;
        uint32_t cluster_base = light_cluster_streams_ ? light_cluster_streams_->GetElementOffset() : 0;
        uint32_t level_base = light_level_headers_ ? (uint32_t)(light_level_headers_->GetRHI().offset / sizeof(MeshLightLevelHeader)) : 0;

        for (auto & header : packed_headers) {
            header.MeshLightIndex += mesh_light_base;
        }
        for (auto & mesh_light : packed_mesh_lights) {
            mesh_light.ClusterOffset += cluster_base;
            mesh_light.TriangleOffset += triangle_base;
            mesh_light.LevelOffset += level_base;
        }
        for (auto & instance : packed_mli_template) {
            instance.MeshLightIndex += mesh_light_base;
        }

        if (light_triangle_streams_) {
            Helpers::Upload_Async(queue, light_triangle_streams_->GetRHI(kMeshLightTriangleStream_Triangle), packed_triangles.data(), packed_triangles.size() * sizeof(MeshLightTriangle));
            Helpers::Upload_Async(queue, light_triangle_streams_->GetRHI(kMeshLightTriangleStream_Hash), packed_triangle_hashes.data(), packed_triangle_hashes.size() * sizeof(MeshLightTriangleHash));
            Helpers::Upload_Async(queue, light_triangle_streams_->GetRHI(kMeshLightTriangleStream_BakedData), packed_triangle_baked_data.data(), packed_triangle_baked_data.size() * sizeof(MeshLightTriangleBakedData));
        }
        if (light_cluster_streams_) {
            Helpers::Upload_Async(queue, light_cluster_streams_->GetRHI(kMeshLightClusterStream_Header), packed_headers.data(), packed_headers.size() * sizeof(MeshLightClusterHeader));
            Helpers::Upload_Async(queue, light_cluster_streams_->GetRHI(kMeshLightClusterStream_Node), packed_nodes.data(), packed_nodes.size() * sizeof(MeshLightClusterNode));
        }
        upload_ok &= UploadPackedVector<MeshLightLevelHeader>(
            queue, light_level_headers_, alloc->GetMeshLightLevelHeaderUberBuffer(), packed_level_headers, "mesh light level header uber buffer"
        );
        upload_ok &= UploadPackedVector<MeshLight>(
            queue, lights_, alloc->GetMeshLightUberBuffer(), packed_mesh_lights, "mesh light uber buffer"
        );
    }

    if (!upload_ok) {
        mesh_light_triangles_.clear();
        mesh_light_triangle_hashes_.clear();
        mesh_light_triangle_baked_data_.clear();
        mesh_light_level_headers_.clear();
        mesh_lights_.clear();
        mesh_light_instance_template_.clear();
    } else {
        mesh_light_triangles_ = std::move(packed_triangles);
        mesh_light_triangle_hashes_ = std::move(packed_triangle_hashes);
        mesh_light_triangle_baked_data_ = std::move(packed_triangle_baked_data);
        mesh_light_level_headers_ = std::move(packed_level_headers);
        mesh_lights_ = std::move(packed_mesh_lights);
        mesh_light_instance_template_ = std::move(packed_mli_template);
    }
    light_hierarchy_device_dirty_ = !upload_ok;
}

TRef<StaticMesh> StaticMesh::Create(bool is_ray_traced, bool dynamic) {
    auto mesh = TRef(new StaticMesh());
    mesh->is_ray_traced_ = is_ray_traced;
    mesh->dynamic_ = dynamic;
    return mesh;
}


StaticMeshInstance::StaticMeshInstance(Scene * scene): Renderable(RenderableType::kStaticMeshInstance, scene) {}

StaticMeshInstance::~StaticMeshInstance() {}

void StaticMeshInstance::Update([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    aabb_ = static_mesh_ ? static_mesh_->GetAABB() : AABB::Empty();
    SetDirty(false);
}

void StaticMeshInstance::UpdateLights_Async(DeviceBindlessResourceAllocator *alloc, RHICommandQueueGraphics & queue) {
    // Clear allocated buffers
    mesh_light_instances_.clear();
    mli_buffer_.SafeRelease();
    mli_cluster_buffers_.clear();
    mli_triangle_buffers_.clear();
    // If no static mesh or empty static mesh, just return. No need to keep empty buffers for now.
    if (!static_mesh_ || static_mesh_->IsEmpty()) return ;
    if (!static_mesh_->GetDeviceStaticMesh()) {
        MI_WARN("Can not update lights for static mesh instance. static mesh is not updated on device.");
        return ;
    }

    // New path: upload mesh-light hierarchy and instance entries.
    static_mesh_->UploadLightClusterHierarchy_Async(alloc, queue);
    auto const & hierarchy_instances_template = static_mesh_->GetMeshLightInstanceTemplate();
    if (hierarchy_instances_template.empty()) {
        return;
    }

    mesh_light_instances_ = hierarchy_instances_template;
    auto const & mesh_lights = static_mesh_->GetMeshLights();
    mi_check(
        mesh_light_instances_.size() == mesh_lights.size(),
        "Mesh light instance template count must match static mesh light count."
    );
    bool upload_ok = true;
    for (uint32_t local_mesh_light_index = 0; local_mesh_light_index < (uint32_t)mesh_light_instances_.size(); ++local_mesh_light_index) {
        auto & mli = mesh_light_instances_[local_mesh_light_index];
        mli.RenderableIndex = GetIndex();
        auto const & ml = mesh_lights[local_mesh_light_index];

        auto mli_triangles = alloc->GetMeshLightInstanceTriangleUberBuffer()->AllocateRefCounted<MeshLightInstanceTriangle>(ml.NumTriangles);
        if (!mli_triangles) {
            MI_WARN("Failed to allocate mesh light instance triangle buffer for static mesh instance {}.", GetIndex());
            upload_ok = false;
            break;
        }
        mli.MeshLightInstanceTriangleOffset = (uint32_t)(mli_triangles->GetRHI().offset / sizeof(MeshLightInstanceTriangle));
        mli_triangle_buffers_.emplace_back(mli_triangles);

        if (ml.NumLevels == 0) {
            mli.MeshLightInstanceClusterOffset = MakeMeshLightInstanceElementOffset(true, mli.MeshLightInstanceTriangleOffset);
            continue;
        }

        auto mli_cluster_buffer = alloc->GetMeshLightInstanceClusterUberBufferArray()->AllocateRefCounted(ml.NumClusters);
        if (!mli_cluster_buffer.first) {
            MI_WARN("Failed to allocate mesh light instance cluster buffers for static mesh instance {}.", GetIndex());
            upload_ok = false;
            break;
        }
        uint32_t cluster_offset = mli_cluster_buffer.first->GetElementOffset();
        mli.MeshLightInstanceClusterOffset = MakeMeshLightInstanceElementOffset(false, cluster_offset);
        mli_cluster_buffers_.emplace_back(mli_cluster_buffer.first);
    }
    if (upload_ok) {
        mli_buffer_ = alloc->GetMeshLightInstanceUberBuffer()->AllocateRefCounted<MeshLightInstance>(mesh_light_instances_.size());
        if (mli_buffer_) {
            Helpers::Upload_Async(queue, mli_buffer_->GetRHI(), mesh_light_instances_.data(), mesh_light_instances_.size() * sizeof(MeshLightInstance));
        } else upload_ok = false;
    }
    if (!upload_ok) {
        mesh_light_instances_.clear();
        mli_buffer_.SafeRelease();
        mli_cluster_buffers_.clear();
        mli_triangle_buffers_.clear();
        MI_WARN("Failed to allocate buffers for mesh light instances of static mesh instance {}. Aborting light update.", GetIndex());
    }
}


TRef<StaticMeshInstance> StaticMeshInstance::Create(Scene *scene, StaticMesh * static_mesh, Transform transform) {
    auto mesh = TRef(new StaticMeshInstance(scene));
    if (mesh->IsValid()) {
        mesh->SetTransform(transform);
        mesh->scene_ = scene;
        mesh->static_mesh_ = static_mesh;;
        return std::move(mesh);
    }
    return {};
}
RenderableHeader StaticMeshInstance::GetDeviceRenderableHeader() const {
    return std::bit_cast<RenderableHeader>(StaticMeshInstanceHeader{
        GetStaticMesh()->GetDeviceStaticMesh()->GetIndex(), 0, 0,
        GetRenderableFlags()
    });
}

RHIAccelerationStructure *StaticMeshInstance::GetBLAS() const {
    if (static_mesh_) return static_mesh_->GetDeviceStaticMesh()->GetBLAS();
    return nullptr;
}

RHIASGeometryInstanceFlags StaticMeshInstance::GetASGeometryInstanceFlags() const {
    RHIASGeometryInstanceFlags flags = RHIASGeometryInstanceFlagBits::kNone;
    if (static_mesh_) {
        return static_mesh_->HasDoubleSidedMaterial() ? RHIASGeometryInstanceFlagBits::kDisableTriangleFaceCulling : RHIASGeometryInstanceFlagBits::kNone;
    }
    return flags;
}

uint32_t StaticMeshInstance::GetRayTracedClassIndex() const {
    return kClassRegistrator.GetClassIndex();
}

uint32_t StaticMeshInstance::GetInstanceCustomIndex() const {
    // Simply return the index of the instance
    return GetIndex() | (GetRayTracedClassIndex() << Renderable::kRenderableIndexNumBits);
}

bool StaticMeshInstance::IsEmpty() const {
    return !static_mesh_ || static_mesh_->IsEmpty();
}

MI_NAMESPACE_END
