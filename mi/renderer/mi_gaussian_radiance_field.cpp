/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_gaussian_radiance_field.h>

#include <renderer/mi_resource_allocator.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_helper.h>
#include <rhi/rhi_as.h>
#include <rhi/rhi.h>
#include "shaders/shared/SharedRenderable.hlsl"

#include <ranges>
#include "rdg/rdg_ray_tracing_registry.h"

MI_NAMESPACE_BEGIN

static RayTracedRenderableClassRegistrator g_ray_traced_gaussian_rf_registrator("GaussianRadianceField", "GaussianRadianceField");

// ---------------- DeviceGaussianRadianceField ----------------
DeviceGaussianRadianceField::DeviceGaussianRadianceField(DeviceBindlessResourceAllocator * alloc) {
    slot_ = alloc->AllocateGaussianRadianceFieldSlotKeeper();
}

DeviceGaussianRadianceField::~DeviceGaussianRadianceField() {
    // Slot is freed (delayed) by SlotKeeper.
}

// ---------------- GaussianRadianceField ----------------
TRef<GaussianRadianceField> GaussianRadianceField::Create() {
    return TRef(new GaussianRadianceField());
}

void GaussianRadianceField::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_ && dirty_) tracker_->OnObjectTurnedDirty(this);
}

void GaussianRadianceField::SetRayTraced(bool rt) {
    if (rt != ray_traced_) {
        ray_traced_ = rt;
        SetDirty();
    }
}

void GaussianRadianceField::SetPoints(const std::vector<PackedGaussian3D> & points) {
    points_ = points;
    // Recompute AABB (rough: max scale as half size cube)
    aabb_ = AABB::Empty();
    for (const auto & p : points_) {
        float max_scale = glm::max(glm::max(p.Scales.x, p.Scales.y), p.Scales.z);
        AABB point_aabb = AABB::FromCenterAndHalfSize(p.Position, glm::vec3(max_scale));
        aabb_ = AABB::Merge(aabb_, point_aabb);
    }
    SetDirty();
}

void GaussianRadianceField::SetupAllocatorUberBuffer(DeviceBindlessResourceAllocator * alloc) {
    auto ub = alloc->GetCustomUberBuffer(kGaussianRadianceAllocatorUberBufferIndex);
    if (!ub) {
        alloc->RegisterCustomUberBuffer(
            kGaussianRadianceAllocatorUberBufferIndex,
            DefaultDeviceUberBuffer::Create(RHIBufferUsageFlagBits::kStorage, 16).Raw()
        );
    }
    auto sh_ub = alloc->GetCustomUberBuffer(kGaussianRadianceSHAllocatorUberBufferIndex);
    if (!sh_ub) {
        alloc->RegisterCustomUberBuffer(
            kGaussianRadianceSHAllocatorUberBufferIndex,
            DefaultDeviceUberBuffer::Create(RHIBufferUsageFlagBits::kStorage, 16).Raw()
        );
    }
}

static Gaussian3D UnpackPoint(PackedGaussian3D packed) {
    Gaussian3D pt {};
    pt.Position = packed.Position;
    pt.Scales = packed.Scales;
    auto rotation_3 = glm::vec3(glm::unpackSnorm4x8(packed.PackedRotation_Opacity));
    float rotation_w = sqrt(glm::max(0.f, 1.f - glm::dot(rotation_3, rotation_3)));
    pt.Rotation = glm::vec4(rotation_3.x, rotation_3.y, rotation_3.z, rotation_w);
    // Normalize quaternion to avoid precision issues
    pt.Rotation = glm::normalize(pt.Rotation);
    pt.Opacity = glm::unpackUnorm4x8(packed.PackedRotation_Opacity).w;
    return pt;
}

void GaussianRadianceField::UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!dirty_ || points_.empty()) return;

    if (!device_field_) {
        device_field_ = new DeviceGaussianRadianceField(alloc);
        mi_check(device_field_->IsValid(), "Failed to allocate GaussianRadianceField slot.");
    }

    // Upload packed points to uber buffer
    size_t required_size = points_.size() * sizeof(PackedGaussian3D);
    if (!device_field_->point_buffer_ || device_field_->point_buffer_->GetRHI().size < required_size) {
        device_field_->point_buffer_ = alloc->GetCustomUberBuffer(kGaussianRadianceAllocatorUberBufferIndex)
            ->AllocateRefCounted(static_cast<uint32_t>(required_size)).first;
    }
    Helpers::Upload_Async(queue, device_field_->point_buffer_->GetRHI(), points_.data(), required_size);

    // Upload SH coefficients (layout expects 16 coefficients per point for 4th-order)
    if (!sh_coeffs_.empty()) {
        size_t sh_required_size = sh_coeffs_.size() * sizeof(glm::vec3);
        if (!device_field_->sh_coeff_buffer_ || device_field_->sh_coeff_buffer_->GetRHI().size < sh_required_size) {
            device_field_->sh_coeff_buffer_ = alloc->GetCustomUberBuffer(kGaussianRadianceSHAllocatorUberBufferIndex)
                ->AllocateRefCounted(static_cast<uint32_t>(sh_required_size)).first;
        }
        Helpers::Upload_Async(queue, device_field_->sh_coeff_buffer_->GetRHI(), sh_coeffs_.data(), sh_required_size);
    }

    GaussianRadianceFieldHeader header {
        static_cast<uint32_t>(points_.size()),
        static_cast<uint32_t>(device_field_->point_buffer_->GetRHI().offset / sizeof(PackedGaussian3D)), // element offset
        srgb_space_ ? 1u : 0u,
        0
    };
    Helpers::Upload_Async(queue, alloc->GetGaussianRadianceFieldHeaderBuffer(), sizeof(GaussianRadianceFieldHeader) * device_field_->GetIndex(), header);

    auto EvaluateRotationMatrix = [](glm::vec4 q) -> glm::mat3 {
        float x = q.x, y = q.y, z = q.z, w = q.w;
        return glm::mat3(
            glm::vec3(1 - 2 * (y * y + z * z),     2 * (x * y - z * w),     2 * (x * z + y * w)),
            glm::vec3(    2 * (x * y + z * w), 1 - 2 * (x * x + z * z),     2 * (y * z - x * w)),
            glm::vec3(    2 * (x * z - y * w),     2 * (y * z + x * w), 1 - 2 * (x * x + y * y))
        );
    };

    // Optional BLAS build (disabled unless ray_traced_ true)
    if (!ray_traced_) {
        device_field_->BLAS_ = {};
    } else {
        if (points_.empty()) {
            device_field_->BLAS_ = {};
        } else {
            // Similar to volume primitives: use icosahedron proxies for each gaussian
            RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
            build_flags = build_flags | (dynamic_ ? RHIAccelerationStructureBuildFlagBits::kAllowUpdate : RHIAccelerationStructureBuildFlagBits::kNone);
            RHIASGeometryFlags geometry_flags = RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation; // no opaque flag per spec

            std::vector<uint32_t> indices;
            std::vector<glm::vec3> vertices;
            indices.resize(points_.size() * 60);
            vertices.resize(points_.size() * 12);
            const float MagicScale = 1.3f;
            const float3 IcoVertices[12] = {
                float3(0.000000, -1.000000, 0.000000), float3(0.723600, -0.447215, 0.525720), float3(-0.276385, -0.447215, 0.850640), float3(-0.894425, -0.447215, 0.000000),
                float3(-0.276385, -0.447215, -0.850640), float3(0.723600, -0.447215, -0.525720), float3(0.276385, 0.447215, 0.850640), float3(-0.723600, 0.447215, 0.525720),
                float3(-0.723600, 0.447215, -0.525720), float3(0.276385, 0.447215, -0.850640), float3(0.894425, 0.447215, 0.000000), float3(0.000000, 1.000000, 0.000000)
            };
            const int3 IcoFaces[20] = {
                int3(0,1,2),int3(1,0,5),int3(0,2,3),int3(0,3,4),int3(0,4,5),int3(1,5,10),int3(2,1,6),int3(3,2,7),int3(4,3,8),int3(5,4,9),
                int3(1,10,6),int3(2,6,7),int3(3,7,8),int3(4,8,9),int3(5,9,10),int3(6,10,11),int3(7,6,11),int3(8,7,11),int3(9,8,11),int3(10,9,11)
            };
            for (const auto & [i, packed] : points_ | std::views::enumerate) {
                auto pt = UnpackPoint(packed);
                glm::mat3 MR = EvaluateRotationMatrix(pt.Rotation);
                glm::mat3 MS = glm::mat3(
                    glm::vec3(pt.Scales.x, 0, 0),
                    glm::vec3(0, pt.Scales.y, 0),
                    glm::vec3(0, 0, pt.Scales.z)
                );
                glm::mat3 M = MR * MS;
                uint32_t vb = static_cast<uint32_t>(i * 12);
                for (int v = 0; v < 12; ++v) {
                    glm::vec3 local = glm::vec3(IcoVertices[v]) * MagicScale;
                    vertices[vb + v] = (M * local) + pt.Position;
                }
                uint32_t ib = static_cast<uint32_t>(i * 60);
                for (int f = 0; f < 20; ++f) {
                    auto Face = IcoFaces[f];
                    indices[ib + f*3 + 0] = Face.x + vb;
                    indices[ib + f*3 + 1] = Face.y + vb;
                    indices[ib + f*3 + 2] = Face.z + vb;
                }
            }
            auto device_vertex_buffer = RHI::Get().CreateBuffer(static_cast<uint32_t>(vertices.size() * sizeof(glm::vec3)), RHIBufferUsageFlagBits::kAccelerationStructureBuildInput);
            auto device_index_buffer = RHI::Get().CreateBuffer(static_cast<uint32_t>(indices.size() * sizeof(uint32_t)), RHIBufferUsageFlagBits::kAccelerationStructureBuildInput);
            Helpers::Upload_Async(queue, device_vertex_buffer->GetSpan(), vertices.data(), vertices.size() * sizeof(glm::vec3));
            Helpers::Upload_Async(queue, device_index_buffer->GetSpan(), indices.data(), indices.size() * sizeof(uint32_t));
            // Barrier uploads -> build input read
            queue.BufferBarrier(device_vertex_buffer->GetSpan(), RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAccelerationStructureRead);
            queue.BufferBarrier(device_index_buffer->GetSpan(), RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAccelerationStructureRead);

            auto as_geom = queue.Allocate<RHIASGeometry>();
            *as_geom = RHIASGeometry{
                RHIASGeometryType::kTriangles,
                geometry_flags,
                {device_vertex_buffer->GetSpan(), sizeof(glm::vec3), static_cast<uint32_t>(vertices.size()), RHIVertexAttributeFormatType::k3xFp32,
                 device_index_buffer->GetSpan(), static_cast<uint32_t>(indices.size()), RHIIndexType::kUint32}
            };
            if (!device_field_->BLAS_) {
                device_field_->BLAS_ = RHI::Get().CreateAccelerationStructure(RHIAccelerationStructureType::kBottomLevel);
            }
            auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                RHIAccelerationStructureType::kBottomLevel,
                build_flags,
                RHIAccelerationStructureBuildMode::kUpdate,
                device_field_->BLAS_.Raw(), device_field_->BLAS_.Raw(), {as_geom, 1}, {}, {}
            };
            auto sizes = device_field_->BLAS_->GetBuildSizes(build_info);
            bool topology_changed = (device_field_->last_vertex_count_ != (uint32_t)vertices.size()) || (device_field_->last_index_count_ != (uint32_t)indices.size());
            bool updated = false;
            if (!topology_changed && dynamic_ && sizes.acceleration_structure_size <= device_field_->BLAS_->GetSize()) {
                queue.AccelerationStructureBarrier(device_field_->BLAS_.Raw(),
                    RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild, RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                    RHIGPUAccessFlagBits::kAccelerationStructureRW, RHIGPUAccessFlagBits::kAccelerationStructureRW
                );
                auto scratch = RHI::Get().CreateBuffer(sizes.update_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
                updated = true;
            }
            if (!updated) {
                device_field_->BLAS_->Create(sizes.acceleration_structure_size);
                build_info.mode = RHIAccelerationStructureBuildMode::kBuild;
                build_info.src_acceleration_structure = {};
                build_info.dst_acceleration_structure = device_field_->BLAS_.Raw();
                auto scratch = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
            }
            device_field_->last_vertex_count_ = (uint32_t)vertices.size();
            device_field_->last_index_count_ = (uint32_t)indices.size();
            queue.AccelerationStructureBarrier(device_field_->BLAS_.Raw(),
                RHIPipelineStageFlagBits::kAccelerationStructureBuild, RHIPipelineStageFlagBits::kRayTracing | RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIGPUAccessFlagBits::kAccelerationStructureRW, RHIGPUAccessFlagBits::kAccelerationStructureRW
            );
        }
    }

    SetDirty(false);
}

void GaussianRadianceField::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("GaussianRadianceField::UpdateOnDevice");
}

// ---------------- GaussianRadianceFieldInstance ----------------
GaussianRadianceFieldInstance::GaussianRadianceFieldInstance(Scene * scene)
    : Renderable(RenderableType::kGaussianRadianceFieldInstance, scene) {}

GaussianRadianceFieldInstance::~GaussianRadianceFieldInstance() = default;

TRef<GaussianRadianceFieldInstance> GaussianRadianceFieldInstance::Create(Scene * scene, GaussianRadianceField * field, Transform transform) {
    auto inst = TRef(new GaussianRadianceFieldInstance(scene));
    if (inst->IsValid()) {
        inst->SetTransform(transform);
        inst->scene_ = scene;
        inst->field_ = field;
        return std::move(inst);
    }
    return {};
}

void GaussianRadianceFieldInstance::Update([[maybe_unused]] RendererView * view, [[maybe_unused]] RenderGraphBuilder & builder) {
    aabb_ = field_ ? field_->GetAABB() : AABB::Empty();
    SetDirty(false);
}

RenderableHeader GaussianRadianceFieldInstance::GetDeviceRenderableHeader() const {
    // Reuse StaticMeshInstanceHeader packing style until a dedicated header is added.
    return std::bit_cast<RenderableHeader>(GaussianRadianceFieldInstanceHeader{
        field_ ? field_->GetDeviceField()->GetIndex() : 0xFFFFFFFFu,
        0, 0, GetRenderableFlags()
    });
}

RHIAccelerationStructure * GaussianRadianceFieldInstance::GetBLAS() const {
    if (field_ && field_->IsRayTraced()) return field_->GetDeviceField()->GetBLAS();
    return nullptr;
}

RayTracedRenderableClassRegistrator<GaussianRadianceFieldInstance> GaussianRadianceFieldInstance::kClassRegistrator("GaussianRadianceField", "GaussianRadianceField");

uint32_t GaussianRadianceFieldInstance::GetRayTracedClassIndex() const {
    return kClassRegistrator.GetClassIndex();
}

uint32_t GaussianRadianceFieldInstance::GetInstanceCustomIndex() const {
    // Encode index with gaussian radiance field flag
    return GetIndex() | (GetRayTracedClassIndex() << Renderable::kRenderableIndexNumBits);
}

bool GaussianRadianceFieldInstance::IsEmpty() const {
    return !field_ || field_->IsEmpty();
}

MI_NAMESPACE_END
