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

#include "shaders/shared/SharedLight.hlsl"

MI_NAMESPACE_BEGIN

namespace {

constexpr float kOverlapEpsilon = 1e-6f;

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
    SetDirty(true);
}

void StaticMesh::ClearMeshPrimitives() {
    geometries_.clear();
    materials_.clear();
    device_static_mesh_ = {};
    aabb_ = {};
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
    if (lights_) {
        Helpers::Clear_Async(queue, lights_->GetRHI());
        lights_.SafeRelease();
    }
    if (!static_mesh_ || static_mesh_->IsEmpty()) return ;
    if (!static_mesh_->GetDeviceStaticMesh()) {
        MI_WARN("Can not update lights for static mesh instance. static mesh is not updated on device.");
        return ;
    }
    auto & geometries = static_mesh_->GetGeometries();
    auto & materials = static_mesh_->GetMaterials();
    std::vector<RawLight> lights;
    auto rng32 = std::mt19937(std::random_device{}());
    for (int i = 0; i < (int)geometries.size(); i++) {
        if (materials[i]->IsEmissive()) {
            // Emissive material found, insert all primitives as lights to the light buffer
            // TODO classify low level lights and intense lights into different rendering paths
            auto & geom = geometries[i];
            auto * emissive_map = materials[i]->GetEmissiveTexture();
            auto const & vertices = geom->GetVertexBuffer();
            auto const & indices = geom->GetIndexBufferRef();
            uint32_t triangle_count = geom->GetIndexCount() / 3;
            for (uint32_t j = 0; j < triangle_count; j++) {
                if (emissive_map) {
                    uint32_t i0 = indices[j * 3 + 0];
                    uint32_t i1 = indices[j * 3 + 1];
                    uint32_t i2 = indices[j * 3 + 2];
                    if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
                        continue;
                    }
                    if (!HasAnyEmissiveTexelInTriangle(emissive_map, vertices[i0].UV, vertices[i1].UV, vertices[i2].UV)) {
                        continue;
                    }
                }

                RawLight light {};
                light.Data0.x = GetIndex();
                light.Data0.y = i; // Geometry index
                light.Data0.z = j; // Primitive index
                // Dirty + random seed for the light, 0 is invalid
                light.Data0.w = std::max(0x80000000u | (rng32() & 0x7fffffffu), 1u);
                lights.emplace_back(light);
            }
        }
    }
    if (lights.empty()) return ;
    auto result = alloc->GetAreaLightsUberBuffer()->AllocateRefCounted(
        (uint32_t)(lights.size() * sizeof(RawLight))
    );
    if (result.second) {
        lights_ = result.first;
        Helpers::Upload_Async(queue, lights_->GetRHI(), lights.data(), lights.size() * sizeof(RawLight));
    } else {
        MI_WARN("Failed to allocate area lights buffer for static mesh instance {}. Maybe too many lights?", GetIndex());
        lights_ = {};
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

uint32_t StaticMeshInstance::GetInstanceCustomIndex() const {
    // Simply return the index of the instance
    return GetIndex();
}

bool StaticMeshInstance::IsEmpty() const {
    return !static_mesh_ || static_mesh_->IsEmpty();
}

MI_NAMESPACE_END