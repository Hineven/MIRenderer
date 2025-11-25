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

MI_NAMESPACE_BEGIN

// ---------------- DeviceGaussianRadianceField ----------------
DeviceGaussianRadianceField::DeviceGaussianRadianceField(DeviceBindlessResourceAllocator * alloc) {
    index_ = alloc->AllocateGaussianRadianceFieldSlot();
}

DeviceGaussianRadianceField::~DeviceGaussianRadianceField() {
    // Slot released by allocator lifetime management (same pattern as other device objects)
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

void GaussianRadianceField::SetPoints(const std::vector<PackedGaussianRadiancePoint> & points) {
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
}

static GaussianRadiancePoint UnpackPoint(PackedGaussianRadiancePoint packed) {
    GaussianRadiancePoint pt;
    pt.Position = packed.Position;
    pt.Scales = packed.Scales;
    pt.Rotation = glm::unpackSnorm4x8(packed.PackedRotation_OpacityHi);
    pt.Rotation.w = sqrt(glm::max(0.f, 1.f - glm::dot(glm::vec3(pt.Rotation), glm::vec3(pt.Rotation))));
    pt.Radiance = glm::vec3(glm::unpackUnorm4x8(packed.PackedColor_OpacityLo));
    pt.Opacity = glm::unpackHalf2x16(((packed.PackedRotation_OpacityHi & 0xFF000000u) >> 16) | ((packed.PackedColor_OpacityLo & 0xFF000000u) >> 24)).x;
    return pt;
}

void GaussianRadianceField::UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!dirty_ || points_.empty()) return;

    if (!device_field_) {
        device_field_ = new DeviceGaussianRadianceField(alloc);
        mi_check(device_field_->IsValid(), "Failed to allocate GaussianRadianceField slot.");
    }

    // Upload packed points to uber buffer
    size_t required_size = points_.size() * sizeof(PackedGaussianRadiancePoint);
    if (!device_field_->point_buffer_ || device_field_->point_buffer_->GetRHI().size < required_size) {
        device_field_->point_buffer_ = alloc->GetCustomUberBuffer(kGaussianRadianceAllocatorUberBufferIndex)
            ->AllocateRefCounted(static_cast<uint32_t>(required_size)).first;
    }

    Helpers::Upload_Async(queue, device_field_->point_buffer_->GetRHI(), points_.data(), required_size);

    GaussianRadianceFieldHeader header {
        static_cast<uint32_t>(points_.size()),
        static_cast<uint32_t>(device_field_->point_buffer_->GetRHI().offset / sizeof(PackedGaussianRadiancePoint))
    };
    Helpers::Upload_Async(queue, alloc->GetGaussianRadianceFieldHeaderBuffer(), sizeof(GaussianRadianceFieldHeader) * device_field_->index_, header);

    // Optional BLAS build (disabled unless ray_traced_ true)
    if (!ray_traced_) {
        device_field_->BLAS_ = {};
    } else {
        // Build icosahedron proxies (similar to volume primitives) for each point
        std::vector<uint32_t> indices;
        std::vector<glm::vec3> vertices;
        indices.resize(points_.size() * 60); // 20 faces * 3 indices
        vertices.resize(points_.size() * 12); // 12 vertices
        for (const auto & [i, packed] : points_ | std::views::enumerate) {
            auto pt = UnpackPoint(packed);
            // Build transform matrix (ignore rotation for now)
            float3x3 MS = float3x3(
                float3(pt.Scales.x, 0, 0),
                float3(0, pt.Scales.y, 0),
                float3(0, 0, pt.Scales.z)
            );
            float3x3 MR = float3x3(1); // TODO apply rotation if needed
            float3x3 M = MR * MS;
            float4x4 ToWorld = float4x4(
                float4(M[0], pt.Position.x),
                float4(M[1], pt.Position.y),
                float4(M[2], pt.Position.z),
                float4(0,0,0,1)
            );
            ToWorld = glm::transpose(ToWorld);
            const float3 IcoVertices[12] = {
                float3(0.000000, -1.000000, 0.000000), float3(0.723600, -0.447215, 0.525720), float3(-0.276385, -0.447215, 0.850640), float3(-0.894425, -0.447215, 0.000000),
                float3(-0.276385, -0.447215, -0.850640), float3(0.723600, -0.447215, -0.525720), float3(0.276385, 0.447215, 0.850640), float3(-0.723600, 0.447215, 0.525720),
                float3(-0.723600, 0.447215, -0.525720), float3(0.276385, 0.447215, -0.850640), float3(0.894425, 0.447215, 0.000000), float3(0.000000, 1.000000, 0.000000)
            };
            const int3 IcoFaces[20] = {
                int3(0,1,2),int3(1,0,5),int3(0,2,3),int3(0,3,4),int3(0,4,5),int3(1,5,10),int3(2,1,6),int3(3,2,7),int3(4,3,8),int3(5,4,9),
                int3(1,10,6),int3(2,6,7),int3(3,7,8),int3(4,8,9),int3(5,9,10),int3(6,10,11),int3(7,6,11),int3(8,7,11),int3(9,8,11),int3(10,9,11)
            };
            uint32_t vb = static_cast<uint32_t>(i * 12);
            for (int v = 0; v < 12; ++v) {
                vertices[vb + v] = glm::vec3(ToWorld * float4(IcoVertices[v], 1));
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
        auto as_geom = queue.Allocate<RHIASGeometry>();
        *as_geom = RHIASGeometry{
            RHIASGeometryType::kTriangles,
            RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation,
            {device_vertex_buffer->GetSpan(), sizeof(glm::vec3), static_cast<uint32_t>(vertices.size()), RHIVertexAttributeFormatType::k3xFp32,
             device_index_buffer->GetSpan(), static_cast<uint32_t>(indices.size()), RHIIndexType::kUint32}
        };
        if (!device_field_->BLAS_) {
            device_field_->BLAS_ = RHI::Get().CreateAccelerationStructure(RHIAccelerationStructureType::kBottomLevel);
        }
        auto build_info = RHIAccelerationStructureBuildGeometryInfo{
            RHIAccelerationStructureType::kBottomLevel,
            RHIAccelerationStructureBuildFlagBits::kPreferFastTrace,
            RHIAccelerationStructureBuildMode::kBuild,
            {}, {}, {as_geom, 1}, {}, {}
        };
        auto sizes = device_field_->BLAS_->GetBuildSizes(build_info);
        device_field_->BLAS_->Create(sizes.acceleration_structure_size);
        auto scratch = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
        build_info.dst_acceleration_structure = device_field_->BLAS_.Raw();
        queue.BuildAccelerationStructure(build_info, scratch->GetSpan());
        queue.AccelerationStructureBarrier(
            device_field_->BLAS_.Raw(),
            RHIPipelineStageFlagBits::kAccelerationStructureBuild,
            RHIPipelineStageFlagBits::kRayTracing,
            RHIGPUAccessFlagBits::kAccelerationStructureWrite,
            RHIGPUAccessFlagBits::kAccelerationStructureRead
        );
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

uint32_t GaussianRadianceFieldInstance::GetInstanceCustomIndex() const {
    // No custom flag yet; return index.
    return GetIndex();
}

bool GaussianRadianceFieldInstance::IsEmpty() const {
    return !field_ || field_->IsEmpty();
}

MI_NAMESPACE_END
