/*
 * Created: 2025/11/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "renderer/mi_volume_grid.h"

#include <vector>
#include <bit>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_buffer_heap.h"
#include "rhi/rhi_as.h"
#include "rdg/rdg_ray_tracing_registry.h"

MI_NAMESPACE_BEGIN

static RayTracedRenderableClassRegistrator g_ray_traced_volume_grid_registrator("VolumeGrid", "VolumeGrid");

// ------------------------------------------------------------------
// DeviceVolumeGrid Implementation
// ------------------------------------------------------------------

DeviceVolumeGrid::DeviceVolumeGrid(DeviceBindlessResourceAllocator* allocator) {
    slot_ = allocator->AllocateVolumeGridSlotKeeper();
}

DeviceVolumeGrid::~DeviceVolumeGrid() {
    // Slot is freed (delayed) by SlotKeeper.
}

// ------------------------------------------------------------------
// VolumeGrid Implementation
// ------------------------------------------------------------------

TRef<VolumeGrid> VolumeGrid::Create() {
    return TRef(new VolumeGrid());
}

void VolumeGrid::SetTexture(TRef<VolumeTexture> texture) {
    if (texture_ != texture) {
        texture_ = texture;
        SetDirty();
    }
}

void VolumeGrid::SetAABB(const AABB& aabb) {
    aabb_ = aabb;
    // AABB 改变意味着 BLAS (几何体) 形状改变，必须重建
    if (device_volume_grid_ && device_volume_grid_->BLAS_) {
        // 标记为 dirty 会在下一次 update 时重建 BLAS
        SetDirty(); 
    }
}

void VolumeGrid::UpdateOnDevice_Async(DeviceBindlessResourceAllocator* alloc, RHICommandQueueGraphics& queue) {
    if (!dirty_) return;

    if (!device_volume_grid_) {
        device_volume_grid_ = new DeviceVolumeGrid(alloc);
    }

    // 确保 3D 纹理已上传并 Resident
    uint32_t tex_bindless_index = 0; 
    if (texture_) {
        texture_->UpdateOnDevice_Async(queue);
        texture_->ConvertToBindless(true); 
        tex_bindless_index = texture_->GetBindlessIndex();
    }

    // 上传 Header 数据到 Allocator 的 Buffer
    VolumeGridHeader header;
    header.TextureBindlessIndex = tex_bindless_index;
    header.LocalMin = aabb_.min;
    header.LocalMax = aabb_.max;

    Helpers::Upload_Async(queue,
        alloc->GetVolumeGridHeaderBuffer(),
        sizeof(VolumeGridHeader) * device_volume_grid_->GetIndex(),
        header
    );

    // 构建/更新 BLAS
    if (!IsRayTraced()) {
        device_volume_grid_->BLAS_ = {};
    } else {
        // 基于 aabb_ 构建一个立方体
        std::vector<glm::vec3> vertices(8);
        const auto& min = aabb_.min;
        const auto& max = aabb_.max;

        // 8个顶点
        vertices[0] = {min.x, min.y, min.z};
        vertices[1] = {max.x, min.y, min.z};
        vertices[2] = {max.x, max.y, min.z};
        vertices[3] = {min.x, max.y, min.z};
        vertices[4] = {min.x, min.y, max.z};
        vertices[5] = {max.x, min.y, max.z};
        vertices[6] = {max.x, max.y, max.z};
        vertices[7] = {min.x, max.y, max.z};

        // 12个三角形 (36个索引) - 标准立方体拓扑
        std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0, // Front (Z-)
            1, 5, 6, 6, 2, 1, // Right
            5, 4, 7, 7, 6, 5, // Back
            4, 0, 3, 3, 7, 4, // Left
            3, 2, 6, 6, 7, 3, // Top
            4, 5, 1, 1, 0, 4  // Bottom
        };

        auto vertex_buffer = RHI::Get().CreateBuffer(
            (uint32_t)(vertices.size() * sizeof(glm::vec3)),
            RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
        );
        auto index_buffer = RHI::Get().CreateBuffer(
            (uint32_t)(indices.size() * sizeof(uint32_t)),
            RHIBufferUsageFlagBits::kAccelerationStructureBuildInput
        );

        Helpers::Upload_Async(queue, vertex_buffer->GetSpan(), vertices.data(), vertices.size() * sizeof(glm::vec3));
        Helpers::Upload_Async(queue, index_buffer->GetSpan(), indices.data(), indices.size() * sizeof(uint32_t));

        // 创建 BLAS 几何体描述
        auto as_geom = queue.Allocate<RHIASGeometry>();
        *as_geom = RHIASGeometry{
            RHIASGeometryType::kTriangles,
            RHIASGeometryFlagBits::kNoDuplicateAnyHitInvocation,
            {
                vertex_buffer->GetSpan(),
                sizeof(glm::vec3),
                (uint32_t)vertices.size(),
                RHIVertexAttributeFormatType::k3xFp32,
                index_buffer->GetSpan(),
                (uint32_t)indices.size(),
                RHIIndexType::kUint32
            }
        };

        // 如果 BLAS 未创建，则创建
        if (!device_volume_grid_->BLAS_) {
            device_volume_grid_->BLAS_ = RHI::Get().CreateAccelerationStructure(RHIAccelerationStructureType::kBottomLevel);
        }

        auto build_info = RHIAccelerationStructureBuildGeometryInfo{
            RHIAccelerationStructureType::kBottomLevel,
            RHIAccelerationStructureBuildFlagBits::kPreferFastTrace,
            RHIAccelerationStructureBuildMode::kBuild, // 总是重建
            {},
            device_volume_grid_->BLAS_.Raw(),
            {as_geom, 1}, {}, {}
        };

        auto sizes = device_volume_grid_->BLAS_->GetBuildSizes(build_info);

        // 如果现有 buffer 不够大，重新创建
        if (device_volume_grid_->BLAS_->GetSize() < sizes.acceleration_structure_size) {
            device_volume_grid_->BLAS_->Create(sizes.acceleration_structure_size);
        }

        auto scratch_buffer = RHI::Get().CreateBuffer(sizes.build_scratch_size, RHIBufferUsageFlagBits::kAccelerationStructureScratch);
        queue.BuildAccelerationStructure(build_info, scratch_buffer->GetSpan());

        queue.AccelerationStructureBarrier(
            device_volume_grid_->BLAS_.Raw(),
            RHIPipelineStageFlagBits::kAccelerationStructureBuild,
            RHIPipelineStageFlagBits::kRayTracing,
            RHIGPUAccessFlagBits::kAccelerationStructureWrite,
            RHIGPUAccessFlagBits::kAccelerationStructureRead
        );
    }

    SetDirty(false);
}

void VolumeGrid::UpdateOnDevice(DeviceBindlessResourceAllocator* alloc) {
    auto& queue = RHI::Get().GetGraphicsCommandQueue();
    UpdateOnDevice_Async(alloc, queue);
    queue.WaitForIdle("VolumeGrid::UpdateOnDevice");
}

void VolumeGrid::SetDirty(bool dirty) {
    dirty_ = dirty;
    if (tracker_) {
        tracker_->OnObjectTurnedDirty(this);
    }
}

// ------------------------------------------------------------------
// VolumeGridInstance Implementation
// ------------------------------------------------------------------

VolumeGridInstance::VolumeGridInstance(Scene* scene) 
    : Renderable(RenderableType::kVolumeGridInstance, scene) {
}

VolumeGridInstance::~VolumeGridInstance() {}

TRef<VolumeGridInstance> VolumeGridInstance::Create(Scene* scene, VolumeGrid* volume_grid, Transform transform) {
    auto instance = TRef(new VolumeGridInstance(scene));
    if (instance->IsValid()) {
        instance->volume_grid_ = volume_grid;
        instance->SetTransform(transform);
        return instance;
    }
    return {};
}

RenderableHeader VolumeGridInstance::GetDeviceRenderableHeader() const {
    return std::bit_cast<RenderableHeader>(VolumeGridInstanceHeader{
        GetVolumeGrid()->GetDeviceVolumeGrid()->GetIndex(), 0, 0,
        GetRenderableFlags()
    });
}

void VolumeGridInstance::Update([[maybe_unused]] RendererView* view, [[maybe_unused]] RenderGraphBuilder& builder) {
    aabb_ = volume_grid_ ? volume_grid_->GetAABB() : AABB::Empty();
    SetDirty(false);
}

RHIAccelerationStructure* VolumeGridInstance::GetBLAS() const {
    if (volume_grid_) {
        return volume_grid_->GetDeviceVolumeGrid()->GetBLAS();
    }
    return nullptr;
}

RayTracedRenderableClassRegistrator<VolumeGridInstance> VolumeGridInstance::kClassRegistrator("VolumeGrid", "VolumeGrid");

uint32_t VolumeGridInstance::GetRayTracedClassIndex() const {
    return kClassRegistrator.GetClassIndex();
}

uint32_t VolumeGridInstance::GetInstanceCustomIndex() const {
    // 标记这是 VolumeGrid 类型，以便 Shader 通过 InstanceID 区分
    return GetIndex() | (GetRayTracedClassIndex() << Renderable::kRenderableIndexNumBits);
}

bool VolumeGridInstance::IsEmpty() const {
    return !volume_grid_ || !volume_grid_->GetTexture();
}

MI_NAMESPACE_END