/*
 * Created: 2025/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include <renderer/mi_renderer.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>
#include <renderer/util/radix_sort.h>

#include "r_view_common.h"
#include "rdg/rdg_helper.h"
#include "renderer/mi_resource_allocator.h"
#include "renderer/mi_volume_primitives.h"
#include "renderer/util/radix_sort.h"

MI_NAMESPACE_BEGIN

struct RenderVolumePrimitivesUB {
    glm::uvec2 TileDimensions;
    float ExpandFactor;
    uint32_t NumTiles;
    uint32_t MaxNumPrimitiveInstances;
    uint32_t FrameIndex;
    glm::uvec2 Padding;
};


BEGIN_SHADER_PARAMETERS(VolumePrimitivesShaderParameters)
    SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
    SHADER_UNIFORM_BUFFER(RenderVolumePrimitivesUB, UB)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableInverseTransformBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActivePrimitiveCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveCount)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActivePrimitiveListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveListBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceCount)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceListBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceListKeyBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceListSortedBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceListKeySortedBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, TileInstanceOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileInstanceOffsetBuffer)
    SHADER_RESOURCE_PARAMETER(StructuredBuffer, TileInstanceCountBuffer)
    SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileInstanceCountBuffer)

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDensity)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeMinMax)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeColor)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeCdfAttenuation)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeSampleColorAndLinearDepth)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeSampleTransmittanceAndPdf)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWTransmittance)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointClampSampler)
END_SHADER_PARAMETERS()

IMPLEMENT_SHADER_PARAMETERS(VolumePrimitivesShaderParameters)

class VolumePrimitivesClearCountersShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(VolumePrimitivesClearCountersShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "VolumePrimitivesClearCounters");

class CollectVolumePrimitivesShader : public RDGShader {
public:
    struct CollectVolumePrimitivesUB {
        uint32_t RenderableIndex;
        uint32_t InstancePrimitiveOffset;
        uint32_t InstanceNumPrimitives;
        uint32_t Padding;
    };
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(CollectVolumePrimitivesUB, UB_Collect)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransformBuffer)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveListBuffer)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(CollectVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "CollectVolumePrimitives");

class ProjectVolumePrimitivesShader : public RDGShader {
public:

    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ProjectVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "ProjectVolumePrimitives");

class CollectTileInstanceOffsetsShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(CollectTileInstanceOffsetsShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "CollectTileInstanceOffsets");

class CountTileInstancesShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(CountTileInstancesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "CountTileInstances");

class DrawVolumePrimitivesShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(DrawVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "DrawVolumePrimitives");

constexpr static uint32_t kTileSize = 16;

void Renderer::Render_DrawVolumePrimitives(RendererView *view, RenderGraphBuilder &builder) {

    glm::uvec2 tile_dimensions = {
        DivideAndRoundUp(view->film_width_, kTileSize),
        DivideAndRoundUp(view->film_height_, kTileSize)
    };
    uint32_t num_tiles = tile_dimensions.x * tile_dimensions.y;
    auto primitive_data = builder.Import(
        device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
    );
    auto active_primitive_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t));
    auto active_primitive_list = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    auto primitive_instance_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t));
    auto primitive_instance_list_key = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    auto primitive_instance_list_value = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    auto primitive_instance_key_sorted = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives
    );
    auto primitive_instance_list_value_sorted = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives
    );
    auto tile_instance_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * num_tiles);
    auto tile_instance_offset = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * num_tiles);
    auto common_ub = builder.Allocate<RenderVolumePrimitivesUB>();
    auto renderable_transforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    auto renderable_inverse_transforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_inverse_transforms_.Raw());
    common_ub->TileDimensions = tile_dimensions;
    common_ub->ExpandFactor = 1.f;
    common_ub->NumTiles = num_tiles;
    common_ub->MaxNumPrimitiveInstances = kMaxNumActiveVolumePrimitives;
    common_ub->FrameIndex = view->persistent_data_->frame_index_;
    auto params = builder.Allocate<VolumePrimitivesShaderParameters>();
    {
        params->View = view->view_common_params_;
        params->UB = common_ub;
        params->PrimitiveData = primitive_data;
        params->RenderableTransformBuffer = renderable_transforms;
        params->RenderableInverseTransformBuffer = renderable_inverse_transforms;
        params->ActivePrimitiveCount = active_primitive_count.Raw();
        params->RWActivePrimitiveCount = active_primitive_count.Raw();
        params->ActivePrimitiveListBuffer = active_primitive_list.Raw();
        params->RWActivePrimitiveListBuffer = active_primitive_list.Raw();
        params->PrimitiveInstanceCount = primitive_instance_count.Raw();
        params->RWPrimitiveInstanceCount = primitive_instance_count.Raw();
        params->RWPrimitiveInstanceListBuffer = primitive_instance_list_value.Raw();
        params->RWPrimitiveInstanceListKeyBuffer = primitive_instance_list_key.Raw();
        params->PrimitiveInstanceListSortedBuffer = primitive_instance_list_value_sorted.Raw();
        params->PrimitiveInstanceListKeySortedBuffer = primitive_instance_key_sorted.Raw();
        params->TileInstanceOffsetBuffer = tile_instance_offset.Raw();
        params->RWTileInstanceOffsetBuffer = tile_instance_offset.Raw();
        params->TileInstanceCountBuffer = tile_instance_count.Raw();
        params->RWTileInstanceCountBuffer = tile_instance_count.Raw();
        params->RWVolumeDensity = view->G_volume_density_.Raw();
        params->RWVolumeMinMax = view->G_volume_min_max_.Raw();
        params->RWVolumeColor = view->G_volume_color_.Raw();
        params->RWVolumeCdfAttenuation = view->G_volume_cdf_attenuation_.Raw();
        params->RWVolumeSampleColorAndLinearDepth = view->volume_sample_color_and_linear_depth_.Raw();
        params->RWVolumeSampleTransmittanceAndPdf = view->volume_sample_transmittance_and_pdf_.Raw();
        params->RWTransmittance = view->G_transmittance_.Raw();

        params->G_Depth = view->G_depth_.Raw();
        params->PointClampSampler = RHI::Get().GetGlobalSamplers().point_clamp;
    }
    {
        auto shader = RDGShaderLibrary::Get().GetShader<VolumePrimitivesClearCountersShader>();
        auto groups = DivideAndRoundUp(num_tiles, VolumePrimitivesClearCountersShader::kThreadGroupSize);
        Helpers::AddComputePass(builder, shader, params, groups);
    }
    {
        auto shader = RDGShaderLibrary::Get().GetShader<CollectVolumePrimitivesShader>();
        for (auto e : ctx.visible_renderables) {
            if (auto inst = e->As<VolumePrimitivesInstance>()) {
                auto vol = inst->GetVolumePrimitives();
                if (vol) {
                    auto collect_params = builder.Allocate<CollectVolumePrimitivesShader::ShaderParameters>();
                    auto UB = builder.Allocate<CollectVolumePrimitivesShader::CollectVolumePrimitivesUB>();\
                    UB->RenderableIndex = inst->GetIndex();
                    UB->InstancePrimitiveOffset = vol->GetDeviceVolumePrimitives()->GetPrimitiveOffset();
                    UB->InstanceNumPrimitives = vol->GetNumPrimitives();
                    collect_params->UB_Collect = UB;
                    collect_params->View = view->view_common_params_;
                    collect_params->PrimitiveData = primitive_data;
                    collect_params->RenderableTransformBuffer = renderable_transforms;
                    collect_params->RWActivePrimitiveCount = active_primitive_count.Raw();
                    collect_params->RWActivePrimitiveListBuffer = active_primitive_list.Raw();
                    if (UB->RenderableIndex >= 4 * 1024 || UB->InstanceNumPrimitives + UB->InstancePrimitiveOffset >= 1024 * 1024) {
                        assert(false && "Overflowing (RenderableIndex, PrimitiveIndex) packing. See shader for details.");
                    }
                    auto groups = DivideAndRoundUp(vol->GetNumPrimitives(), 128);
                    Helpers::AddComputePass(builder, shader, collect_params, groups);
                }
            }
        }
    }
    {
        auto shader = RDGShaderLibrary::Get().GetShader<ProjectVolumePrimitivesShader>();
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, active_primitive_count.Raw(), ProjectVolumePrimitivesShader::kThreadGroupSize);
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }
    RadixSort::AddRadixSort32BitsPass(builder, kMaxNumActiveVolumePrimitives,
        primitive_instance_list_key.Raw(), primitive_instance_key_sorted.Raw(),
        primitive_instance_list_value.Raw(), primitive_instance_list_value_sorted.Raw(),
        primitive_instance_count.Raw()
    );
    auto instance_indirect_buffer = Helpers::SpawnDispatchIndirectCommand1D(
        builder, primitive_instance_count.Raw(), CollectTileInstanceOffsetsShader::kThreadGroupSize);
    {
        auto shader = RDGShaderLibrary::Get().GetShader<CollectTileInstanceOffsetsShader>();
        Helpers::AddComputeIndirectPass(builder, shader, params, instance_indirect_buffer.Raw());
    }

    {
        auto shader = RDGShaderLibrary::Get().GetShader<CountTileInstancesShader>();
        Helpers::AddComputePass(builder, shader, params, DivideAndRoundUp(num_tiles, CountTileInstancesShader::kThreadGroupSize));
    }

    {
        auto shader = RDGShaderLibrary::Get().GetShader<DrawVolumePrimitivesShader>();
        Helpers::AddComputePass(builder, shader, params, tile_dimensions.x, tile_dimensions.y);
    }
}


MI_NAMESPACE_END