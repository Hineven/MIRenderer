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
    glm::uvec3 Padding;
};

class VolumePrimitivesClearCountersShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(RenderVolumePrimitivesUB, UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileInstanceOffsets)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(VolumePrimitivesClearCountersShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "VolumePrimitivesClearCounters");

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
        SHADER_UNIFORM_BUFFER(CollectVolumePrimitivesUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransforms)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveList)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize), "COLLECT_VOLUME_PRIMITIVES"};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(CollectVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "CollectVolumePrimitives");

class ProjectVolumePrimitivesShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(RenderVolumePrimitivesUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActivePrimitiveCount)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, ActivePrimitiveList)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveScreenAxis) // uint2 packed 2 screen vectors
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveScreenAxisZProjection) // uint packed fp16x2
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWActivePrimitiveCenterPixelPosition) // uint32 packed
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceCount)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceListKey)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWPrimitiveInstanceListPrimitiveIndex)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, RenderableTransforms)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(ProjectVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "ProjectVolumePrimitives");

class CollectTileInstanceOffsetsShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(RenderVolumePrimitivesUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceCount)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceListKeySorted)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, RWTileInstanceOffsets)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(CollectTileInstanceOffsetsShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "CollectTileInstanceOffsets");

class DrawVolumePrimitivesShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Params)
        SHADER_UNIFORM_BUFFER(ViewCommonShaderParameters, View)
        SHADER_UNIFORM_BUFFER(RenderVolumePrimitivesUB, UB)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveData)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceCount)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, TileInstanceOffsets)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceListKeySorted)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, PrimitiveInstanceListPrimitiveIndexSorted)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDensity)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeMinMax)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeColor)
    END_SHADER_PARAMETERS()
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(Params)
};

IMPLEMENT_RDG_COMPUTE_SHADER(DrawVolumePrimitivesShader, "mi/renderer/shaders/VolumePrimitives.hlsl", "DrawVolumePrimitives");

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
    auto tile_instance_offset = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * num_tiles);
    auto common_ub = builder.Allocate<RenderVolumePrimitivesUB>();
    common_ub->ExpandFactor = 1.f;
    common_ub->NumTiles = num_tiles;
    common_ub->MaxNumPrimitiveInstances = kMaxNumActiveVolumePrimitives;
    common_ub->TileDimensions = tile_dimensions;
    {
        auto shader = RDGShaderLibrary::Get().GetShader<VolumePrimitivesClearCountersShader>();
        auto params = builder.Allocate<VolumePrimitivesClearCountersShader::Params>();
        auto groups = DivideAndRoundUp(num_tiles, VolumePrimitivesClearCountersShader::kThreadGroupSize);
        params->UB = common_ub;
        params->RWActivePrimitiveCount = active_primitive_count.Raw();
        params->RWTileInstanceOffsets = tile_instance_offset.Raw();
        params->RWPrimitiveInstanceCount = primitive_instance_count.Raw();
        builder.AddPass<VolumePrimitivesClearCountersShader>({}, params, [shader, params, groups] (RDGPass *pass, RHICommandQueueGraphics &queue) {
            RDGCommandHelper::Dispatch<VolumePrimitivesClearCountersShader>(queue, pass, shader, params, groups);
        });
    }
    auto renderable_transforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    {
        auto shader = RDGShaderLibrary::Get().GetShader<CollectVolumePrimitivesShader>();
        for (auto e : ctx.visible_renderables) {
            if (auto inst = e->As<VolumePrimitivesInstance>()) {
                auto vol = inst->GetVolumePrimitives();
                if (vol) {
                    auto params = builder.Allocate<CollectVolumePrimitivesShader::ShaderParameters>();
                    auto UB = builder.Allocate<CollectVolumePrimitivesShader::CollectVolumePrimitivesUB>();\
                    UB->RenderableIndex = inst->GetIndex();
                    UB->InstancePrimitiveOffset = vol->GetDeviceVolumePrimitives()->GetPrimitiveOffset();
                    UB->InstanceNumPrimitives = vol->GetNumPrimitives();
                    params->UB = UB;
                    params->View = view->view_common_params_;
                    params->PrimitiveData = primitive_data;
                    params->RenderableTransforms = renderable_transforms;
                    params->RWActivePrimitiveCount = active_primitive_count.Raw();
                    params->RWActivePrimitiveList = active_primitive_list.Raw();
                    if (UB->RenderableIndex >= 4 * 1024 || UB->InstanceNumPrimitives + UB->InstancePrimitiveOffset >= 1024 * 1024) {
                        assert(false && "Overflowing (RenderableIndex, PrimitiveIndex) packing. See shader for details.");
                    }
                    builder.AddPass<CollectVolumePrimitivesShader>({},
                        params,  [shader, params, threads = vol->GetNumPrimitives()] (RDGPass *pass, RHICommandQueueGraphics &queue) {
                            auto groups = DivideAndRoundUp(threads, 128);
                            RDGCommandHelper::Dispatch<CollectVolumePrimitivesShader>(queue, pass, shader, params, groups);
                    });
                }
            }
        }
    }
    // auto active_primitive_screen_axis = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives * 2);
    // auto active_primitive_screen_axis_z_projection = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    // auto active_primitive_center_pixel_position = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    auto primitive_instance_list_key = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    auto primitive_instance_list_primitive_index = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives);
    {
        auto shader = RDGShaderLibrary::Get().GetShader<ProjectVolumePrimitivesShader>();
        auto params = builder.Allocate<ProjectVolumePrimitivesShader::ShaderParameters>();
        params->View = view->view_common_params_;
        params->UB = common_ub;
        params->PrimitiveData = primitive_data;
        params->ActivePrimitiveCount = active_primitive_count.Raw();
        params->ActivePrimitiveList = active_primitive_list.Raw();
        // params->RWActivePrimitiveScreenAxis = active_primitive_screen_axis.Raw();
        // params->RWActivePrimitiveScreenAxisZProjection = active_primitive_screen_axis_z_projection.Raw();
        // params->RWActivePrimitiveCenterPixelPosition = active_primitive_center_pixel_position.Raw();
        params->RWPrimitiveInstanceCount = primitive_instance_count.Raw();
        params->RWPrimitiveInstanceListKey = primitive_instance_list_key.Raw();
        params->RWPrimitiveInstanceListPrimitiveIndex = primitive_instance_list_primitive_index.Raw();
        params->RenderableTransforms = renderable_transforms;
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, active_primitive_count.Raw(), ProjectVolumePrimitivesShader::kThreadGroupSize);
        builder.AddPass<ProjectVolumePrimitivesShader>({}, params, [shader, params, indirect = cmd.Raw()] (RDGPass *pass, RHICommandQueueGraphics &queue) {
            RDGCommandHelper::DispatchIndirect<ProjectVolumePrimitivesShader>(queue, pass, shader, params, indirect);
        });
    }
    auto primitive_instance_key_sorted = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives
    );
    auto primitive_instance_list_primitive_index_sorted = builder.CreateBuffer(
        RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * kMaxNumActiveVolumePrimitives
    );
    RadixSort::AddRadixSort32BitsPass(builder, kMaxNumActiveVolumePrimitives,
        primitive_instance_list_key.Raw(), primitive_instance_list_primitive_index.Raw(),
        primitive_instance_key_sorted.Raw(), primitive_instance_list_primitive_index_sorted.Raw(),
        primitive_instance_count.Raw()
    );
    auto tile_instance_count = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t) * num_tiles);
    auto instance_indirect_buffer = Helpers::SpawnDispatchIndirectCommand1D(
        builder, primitive_instance_count.Raw(), CollectTileInstanceOffsetsShader::kThreadGroupSize);
    {
        auto shader = RDGShaderLibrary::Get().GetShader<CollectTileInstanceOffsetsShader>();
        auto params = builder.Allocate<CollectTileInstanceOffsetsShader::ShaderParameters>();
        params->UB = common_ub;
        params->PrimitiveInstanceCount = primitive_instance_count.Raw();
        params->PrimitiveInstanceListKeySorted = primitive_instance_key_sorted.Raw();
        params->RWTileInstanceOffsets = tile_instance_offset.Raw();
        builder.AddPass<CollectTileInstanceOffsetsShader>({}, params,
            [shader, params, cmd = instance_indirect_buffer.Raw()] (RDGPass *pass, RHICommandQueueGraphics &queue) {
            RDGCommandHelper::DispatchIndirect<CollectTileInstanceOffsetsShader>(
                queue, pass, shader, params, cmd
            );
        });
    }

    {
        auto shader = RDGShaderLibrary::Get().GetShader<DrawVolumePrimitivesShader>();
        auto params = builder.Allocate<DrawVolumePrimitivesShader::ShaderParameters>();
        params->View = view->view_common_params_;
        params->UB = common_ub;
        params->PrimitiveData = primitive_data;
        // params->ActivePrimitiveScreenAxis = active_primitive_screen_axis.Raw();
        // params->ActivePrimitiveScreenAxisZProjection = active_primitive_screen_axis_z_projection.Raw();
        // params->ActivePrimitiveCenterPixelPosition = active_primitive_center_pixel_position.Raw();
        params->PrimitiveInstanceCount = primitive_instance_count.Raw();
        params->PrimitiveInstanceListPrimitiveIndexSorted = primitive_instance_list_primitive_index_sorted.Raw();
        params->RWVolumeDensity = view->G_volume_density_.Raw();
        params->RWVolumeMinMax = view->G_volume_min_max_.Raw();
        params->RWVolumeColor = view->G_volume_color_.Raw();
        builder.AddPass<DrawVolumePrimitivesShader>({}, params,
            [shader, params, tile_dimensions] (RDGPass *pass, RHICommandQueueGraphics &queue) {
                RDGCommandHelper::Dispatch<DrawVolumePrimitivesShader>(queue, pass, shader, params, tile_dimensions.x, tile_dimensions.y);
        });
    }
}


MI_NAMESPACE_END