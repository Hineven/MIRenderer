/*
 * Created: 2025/6/1
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <renderer/mi_volume_primitives.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_resource_allocator.h"
#include "rhi/rhi_as.h"

MI_NAMESPACE_BEGIN
    // DeviceVolumePrimitives implementation
DeviceVolumePrimitives::DeviceVolumePrimitives(DeviceBindlessResourceAllocator * allocator) {
    index_ = allocator->AllocateVolumePrimitivesSlot();
}

DeviceVolumePrimitives::~DeviceVolumePrimitives() {
    // Note: We don't have access to the allocator here, so we assume it will be cleaned up elsewhere
    // This follows the same pattern as DeviceStaticMesh in the reference implementation
}

// VolumePrimitives implementation
TRef<VolumePrimitives> VolumePrimitives::Create() {
    auto volume_primitives = TRef(new VolumePrimitives());
    return volume_primitives;
}

void VolumePrimitives::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_) {
        tracker_->OnObjectTurnedDirty(this);
    }
}

void VolumePrimitives::SetPrimitives(const std::vector<PackedVolumePrimitive> & primitives) {
    primitives_ = primitives;
    SetDirty();
}

void VolumePrimitives::SetupAllocatorUberBuffer(DeviceBindlessResourceAllocator * allocator) {
    // Setup the uber buffer for volume primitives similar to static mesh implementation
    auto uber_buffer = allocator->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex);
    if (!uber_buffer) {
        // Create a new uber buffer for volume primitives if it doesn't exist
        allocator->RegisterCustomUberBuffer(
            kVolumePrimitiveAllocatorUberBufferIndex,
            DefaultDeviceUberBuffer::Create(RHIBufferUsageFlagBits::kStorage, 16).Raw()
        );
    }
}

static VolumePrimitive UnpackPrimitive(PackedVolumePrimitive packed) {
    VolumePrimitive prim;
    prim.Position = packed.Position;
    prim.Scales = packed.Scales;
    prim.Rotation = glm::unpackSnorm4x8(packed.PackedRotation_OpacityHi);
    prim.Rotation.w = sqrt(glm::max(0.f, 1.0f - glm::dot(glm::vec3(prim.Rotation), glm::vec3(prim.Rotation)))); // Reconstruct W
    prim.Color = glm::vec3(glm::unpackUnorm4x8(packed.PackedColor_OpacityLo));
    prim.Opacity = glm::unpackHalf2x16(
        ((packed.PackedRotation_OpacityHi & 0xFF000000u) >> 16)
    | ((packed.PackedColor_OpacityLo & 0xFF000000u) >> 24)).x;
    return prim;
}

static glm::mat3 EvaluateRotationMatrix(const glm::vec4 & quat) {
    float x = quat.x, y = quat.y, z = quat.z, w = quat.w;
    auto mat = glm::mat3(
        1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
        2 * (y * x + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
        2 * (z * x - w * y),     2 * (z * y + w * x),     1 - 2 * (x * x + y * y)
    );
    return glm::transpose(mat); // glm is col major.
}

void VolumePrimitives::UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue) {
    if (!dirty_) return;
    if (primitives_.empty()) return;

    if (!device_volume_primitives_) {
        device_volume_primitives_ = new DeviceVolumePrimitives(alloc);
        mi_check(device_volume_primitives_->IsValid(), "Failed to allocate volume primitives slot.");
    }

    // Calculate required buffer size
    auto required_size = primitives_.size() * sizeof(PackedVolumePrimitive);

    // Allocate or reallocate buffer if needed
    if (!device_volume_primitives_->primitive_buffer_ ||
        device_volume_primitives_->primitive_buffer_->GetRHI().size < required_size) {
        device_volume_primitives_->primitive_buffer_ = alloc->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex)
            ->AllocateRefCounted((uint32_t)required_size).first;
    }

    // Upload primitive data
    Helpers::Upload_Async(queue,
        device_volume_primitives_->primitive_buffer_->GetRHI(),
        primitives_.data(),
        required_size
    );

    VolumePrimitivesHeader header = {
        (uint32_t)primitives_.size(),
        (uint32_t)(device_volume_primitives_->primitive_buffer_->GetRHI().offset / sizeof(PackedVolumePrimitive)),
    };

    // Upload header to device
    Helpers::Upload_Async(queue,
        alloc->GetCustomUberBuffer(kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI(),
        sizeof(VolumePrimitivesHeader) * device_volume_primitives_->index_,
        header
    );

    // Update BLAS if needed
    if (!IsRayTraced()) {
        device_volume_primitives_->BLAS_ = {};
    } else {
        if (primitives_.empty()) {
            // No geometries, release BLAS. NullDescriptorSet feature will take care of this case.
            device_volume_primitives_->BLAS_ = {};
        } else {
            // Update acceleration structure for raytracing
            RHIAccelerationStructureBuildFlags build_flags = RHIAccelerationStructureBuildFlagBits::kPreferFastTrace;
            build_flags = build_flags | (dynamic_ ? RHIAccelerationStructureBuildFlagBits::kAllowUpdate : RHIAccelerationStructureBuildFlagBits::kNone);

            RHIASGeometryFlags geometry_flags =
                // We rely on any-hit to accumulate transmittance
                RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation;
            // Spawn and upload buffers

            std::vector<uint32_t> index_data;
            std::vector<glm::vec3> vertex_data;
            index_data.resize(primitives_.size() * 60); // 20 faces, 3 indices each
            vertex_data.resize(primitives_.size() * 12); // 12 vertices per icosahedron
            for (const auto& [prim_index, packed_prim] : primitives_ | std::views::enumerate) {
                auto prim = UnpackPrimitive(packed_prim);
                float4x4 ToWorld = 0;
	            {
		            float3x3 MS = float3x3(
			            float3(prim.Scales.x, 0, 0),
			            float3(0, prim.Scales.y, 0),
			            float3(0, 0, prim.Scales.z)
		            );
		            float3x3 MR = EvaluateRotationMatrix(prim.Rotation);
		            float3x3 M = MR * MS;

		            ToWorld = float4x4(
			            float4(M[0], prim.Position.x),
			            float4(M[1], prim.Position.y),
			            float4(M[2], prim.Position.z),
			            float4(0, 0, 0, 1)
		            );
                    ToWorld = glm::transpose(ToWorld);
	            }

	            // Magic scaling number
	            float MagicScale = 1.f;

	            const float3 IcoVertices[12] = {
		            float3(0.000000, -1.000000, 0.000000),
		            float3(0.723600, -0.447215, 0.525720),
		            float3(-0.276385, -0.447215, 0.850640),
		            float3(-0.894425, -0.447215, 0.000000),
		            float3(-0.276385, -0.447215, -0.850640),
		            float3(0.723600, -0.447215, -0.525720),
		            float3(0.276385, 0.447215, 0.850640),
		            float3(-0.723600, 0.447215, 0.525720),
		            float3(-0.723600, 0.447215, -0.525720),
		            float3(0.276385, 0.447215, -0.850640),
		            float3(0.894425, 0.447215, 0.000000),
		            float3(0.000000, 1.000000, 0.000000)
	            };
	            const int3 IcoFaces[20] = {
		            int3(0, 1, 2),
		            int3(1, 0, 5),
		            int3(0, 2, 3),
		            int3(0, 3, 4),
		            int3(0, 4, 5),
		            int3(1, 5, 10),
		            int3(2, 1, 6),
		            int3(3, 2, 7),
		            int3(4, 3, 8),
		            int3(5, 4, 9),
		            int3(1, 10, 6),
		            int3(2, 6, 7),
		            int3(3, 7, 8),
		            int3(4, 8, 9),
		            int3(5, 9, 10),
		            int3(6, 10, 11),
		            int3(7, 6, 11),
		            int3(8, 7, 11),
		            int3(9, 8, 11),
		            int3(10, 9, 11)
	            };

	            // Emit the vertices
	            auto VertexBase = (uint32_t)(prim_index * 12);
	            for(int i = 0; i < 12; i++) {
		            float3 Vertex = glm::vec3(ToWorld * float4(IcoVertices[i] * MagicScale, 1));
		            vertex_data[VertexBase + i] = Vertex;
	            }
	            // Emit the indices
	            auto IndexBase = (uint32_t)(prim_index * 60);
	            for(int i = 0; i < 20; i++) {
		            int3 Face = IcoFaces[i];
		            index_data[IndexBase + i * 3 + 0] = Face.x + VertexBase;
	                // Crucial: Flip the face winding order to make the faces flipped.
	                // Thus, the rays will only hit the 'back faces'
	                // Often we trace rays within volume primitives and ends up outside of the primitives.
	                // Hitting back faces make transmittance estimation from such traces more accurate.
		            index_data[IndexBase + i * 3 + 1] = Face.z + VertexBase;
		            index_data[IndexBase + i * 3 + 2] = Face.y + VertexBase;
	            }
            }
            auto device_vertex_buffer = RHI::Get().CreateBuffer(
                (uint32_t)(vertex_data.size() * sizeof(glm::vec3)),
                RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
            );
            auto device_index_buffer = RHI::Get().CreateBuffer(
                (uint32_t)(index_data.size() * sizeof(uint32_t)),
                RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
            );
            Helpers::Upload_Async(queue, device_vertex_buffer->GetSpan(), vertex_data.data(), vertex_data.size() * sizeof(glm::vec3));
            Helpers::Upload_Async(queue, device_index_buffer->GetSpan(), index_data.data(), index_data.size() * sizeof(uint32_t));
            // Build BLAS
            auto as_geom = queue.Allocate<RHIASGeometry>();
            *as_geom = RHIASGeometry{
                RHIASGeometryType::kTriangles,
                geometry_flags,
                {
                    device_vertex_buffer->GetSpan(),
                    sizeof(glm::vec3),
                    (uint32_t)vertex_data.size(),
                    RHIVertexAttributeFormatType::k3xFp32,
                    device_index_buffer->GetSpan(),
                    (uint32_t)index_data.size(),
                    RHIIndexType::kUint32
                }
            };
            if (!device_volume_primitives_->BLAS_) {
                // Create
                device_volume_primitives_->BLAS_ = RHI::Get().CreateAccelerationStructure(
                    RHIAccelerationStructureType::kBottomLevel
                );
            }
            auto build_info = RHIAccelerationStructureBuildGeometryInfo{
                RHIAccelerationStructureType::kBottomLevel,
                build_flags,
                RHIAccelerationStructureBuildMode::kUpdate,
                device_volume_primitives_->BLAS_.Raw(),
                device_volume_primitives_->BLAS_.Raw(),
                {as_geom, 1}, {}, {}
            };
            auto sizes = device_volume_primitives_->BLAS_->GetBuildSizes(build_info);
            bool updated = false;
            // Try to update
            if (dynamic_ && sizes.acceleration_structure_size <= device_volume_primitives_->BLAS_->GetSize()) {
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
                updated = true;
            }
            if (!updated) {
                // Need rebuild
                // Re-create
                device_volume_primitives_->BLAS_->Create(sizes.acceleration_structure_size);
                // Build
                build_info.mode = RHIAccelerationStructureBuildMode::kBuild;
                build_info.src_acceleration_structure = {};
                build_info.dst_acceleration_structure = device_volume_primitives_->BLAS_.Raw();
                auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
                queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());
            }
            // Barrier
            queue.AccelerationStructureBarrier(
                device_volume_primitives_->BLAS_.Raw(),
                RHIPipelineStageFlagBits::kAccelerationStructureBuild,
                RHIPipelineStageFlagBits::kRayTracing,
                RHIGPUAccessFlagBits::kAccelerationStructureWrite,
                RHIGPUAccessFlagBits::kAccelerationStructureRead
            );
        }
    }

    SetDirty(false);
}

void VolumePrimitives::UpdateOnDevice(DeviceBindlessResourceAllocator * alloc) {
    auto & queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("VolumePrimitives::UpdateOnDevice");
}

// VolumePrimitivesInstance implementation
VolumePrimitivesInstance::VolumePrimitivesInstance(Scene * scene)
    : Renderable(RenderableType::kVolumePrimitivesInstance, scene) {
}

VolumePrimitivesInstance::~VolumePrimitivesInstance() {
}

TRef<VolumePrimitivesInstance> VolumePrimitivesInstance::Create(Scene * scene, VolumePrimitives * primitives, Transform transform) {
    auto instance = TRef(new VolumePrimitivesInstance(scene));
    if (instance->IsValid()) {
        instance->SetTransform(transform);
        instance->scene_ = scene;
        instance->volume_primitives_ = primitives;
        return std::move(instance);
    }
    return {};
}

void VolumePrimitivesInstance::Update([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    SetDirty(false);
}

RenderableHeader VolumePrimitivesInstance::GetDeviceRenderableHeader() const {
    return std::bit_cast<RenderableHeader>(VolumePrimitivesInstanceHeader{
        GetVolumePrimitives()->GetDeviceVolumePrimitives()->GetIndex(), 0, 0, 0
    });
}

RHIAccelerationStructure *VolumePrimitivesInstance::GetBLAS() const {
    if (volume_primitives_) return volume_primitives_->GetDeviceVolumePrimitives()->GetBLAS();
    return nullptr;
}

uint32_t VolumePrimitivesInstance::GetInstanceCustomIndex() const {
    // Return the index of the instance with a custom flag indicating the type of renderable
    return GetIndex() | INSTANCE_CUSTOM_INDEX_FLAG_VOLUME_PRIMITIVES;
}

bool VolumePrimitivesInstance::IsEmpty() const {
    return !volume_primitives_ || volume_primitives_->IsEmpty();
}

MI_NAMESPACE_END