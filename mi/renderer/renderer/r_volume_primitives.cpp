/*
 * Created: 2025/6/27
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <ranges>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_shader.h>
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include <renderer/util/radix_sort.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_volume_primitives.h>

#include "r_view_common.h"
#include "r_persistent.h"
#include "r_volume_primitives.h"

MI_NAMESPACE_BEGIN

static CVar<float> CVar_VolumeDistributionMergingEpsilon(
    "r.volume_primitives.distribution_merging_epsilon",
    "Epsilon value for merging volume distributions when rendering volume primitives. "
    "Smaller values preserve more details but may increase noise.",
    0.02f
);

static CVar<bool> CVar_VolumePrimitivesUseCloserRepresentativeDepth(
    "r.volume_primitives.use_closer_representative_depth",
    "Whether to always use the closer representative depth when updating volume primitive history. "
    "This can help reduce ghosting artifacts in some cases.",
    true
);

void VolumePrimitivesViewData::Allocate(RenderGraphBuilder &builder, RendererView * view) {
    G_volume_min_max_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR16G16_FLOAT);
    G_volume_min_max_->SetName("GBuffer Volume Min Max");
    G_volume_density_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR32_FLOAT);
    G_volume_density_->SetName("GBuffer Volume Density");
    G_volume_color_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR8G8B8A8_UNORM);
    G_volume_color_->SetName("GBuffer Volume Color");
    G_volume_density_fourier_ = builder.CreateTexture2DArray(
        view->film_width_, view->film_height_, 3, PixelFormatType::kR32_FLOAT);
    G_volume_density_fourier_->SetName("GBuffer Volume Density Fourier");
    G_volume_weighted_color_fourier_ = builder.CreateTexture2DArray(
        view->film_width_, view->film_height_, 3, PixelFormatType::kR8G8B8A8_UNORM);
    G_volume_weighted_color_fourier_->SetName("GBuffer Volume Weighted Color Fourier");
    G_volume_cdf_attenuation_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR16G16_FLOAT);
    G_volume_cdf_attenuation_->SetName("GBuffer Volume CDF Attenuation");

    volume_sample_color_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR8G8B8A8_UNORM);
    volume_sample_color_->SetName("Volume Sample Color");
    volume_sample_linear_depth_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR32_FLOAT);
    volume_sample_linear_depth_->SetName("Volume Sample Linear Depth");
    volume_sample_transmittance_and_pdf_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR16G16_FLOAT);
    volume_sample_transmittance_and_pdf_->SetName("Volume Sample Transmittance and PDF");

    volume_representative_depth_and_variation_ = builder.CreateTexture2D(
        view->film_width_, view->film_height_, PixelFormatType::kR32G32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    volume_representative_depth_and_variation_->SetName("Volume Representative Depth and Variation");
}

bool VolumePrimitivesViewPersistentData::MakeSureExists([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    bool flag = false;
    // Actually, we don't need to do anything. Passing null resources to shader
    // fallbacks to a default and safe behavior. Just tell the shader not to use history.
    if (!prev_volume_representative_depth_and_variation_) {
        flag = true;
    }
    return flag;
}

void VolumePrimitivesViewPersistentData::FinalUpdate(RendererView *view) {
    prev_volume_representative_depth_and_variation_ =
        view->volume_primitives_->volume_representative_depth_and_variation_;
    prev_volume_representative_depth_and_variation_->SetName("PrevVolumeRepresentativeDepthAndVariation");
    prev_volume_representative_depth_and_variation_->SetExport();
}


struct RenderVolumePrimitivesUB {
    glm::uvec2 TileDimensions;
    float ExpandFactor;
    uint32_t NumTiles;
    uint32_t MaxNumPrimitiveInstances;
    uint32_t FrameIndex;
    float    VolumeDistributionMergingEpsilon;
    uint32_t VolumeDistributionAlwaysUseClosest;
    uint32_t EnableFourier; // True:Use Fourier volume
    uint32_t DensityFourierOrder; // If IsFourier, the Fourier order of Density
    uint32_t ColorFourierOrder; // The Fourier order of Color
    uint32_t FourierSampleNum; // Primitive intersection sample num
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

    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeMinMax)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeDensity)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeColor)
    SHADER_RESOURCE_PARAMETER(RWTexture2DArray, RWVolumeDensityFourier)
    SHADER_RESOURCE_PARAMETER(RWTexture2DArray, RWVolumeWeightedColorFourier)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeCdfAttenuation)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeSampleColor)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeSampleLinearDepth)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeSampleTransmittanceAndPdf)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWVolumeRepresentativeDepthAndVariation)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWTransmittance)

    SHADER_RESOURCE_PARAMETER(Texture2D, G_Depth)
    SHADER_RESOURCE_PARAMETER(RWTexture2D, RWFlags)
    SHADER_RESOURCE_PARAMETER(SamplerState, PointEdgeSampler)
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

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(VolumePrimitivesClearCountersShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "VolumePrimitivesClearCounters");

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

IMPLEMENT_RDG_COMPUTE_SHADER(CollectVolumePrimitivesShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "CollectVolumePrimitives");

class ProjectVolumePrimitivesShader : public RDGShader {
public:

    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ProjectVolumePrimitivesShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "ProjectVolumePrimitives");

class ClampPrimitiveInstanceCountShader : public RDGShader {
public:
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(ClampPrimitiveInstanceCountShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "ClampPrimitiveInstanceCount");

class CollectTileInstanceOffsetsShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(CollectTileInstanceOffsetsShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "CollectTileInstanceOffsets");

class CountTileInstancesShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(CountTileInstancesShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "CountTileInstances");

class DrawVolumePrimitivesShader : public RDGShader {
public:
    constexpr static uint32_t kThreadGroupSize = 128; // 128 threads per group
    static std::vector<std::string> GetShaderDefaultMacros () {
        return {"THREAD_GROUP_SIZE=" + std::to_string(kThreadGroupSize)};
    }
    static std::vector<std::string> GetShaderOptionalMacros () {
        return {
            "ENABLE_FOURIER_VOLUME_RENDERING"
        };
    }
    DECLARE_SHADER()
    RDG_SHADER_USE_PARAMETERS(VolumePrimitivesShaderParameters)
};

IMPLEMENT_RDG_COMPUTE_SHADER_SHADER_SHARED_PARAMETER(DrawVolumePrimitivesShader, "mi/renderer/shaders/DrawVolumePrimitives.hlsl", "DrawVolumePrimitives");

constexpr static uint32_t kTileSize = 16;

static CVar<bool> CVar_EnableFourier(
    "r.volume_primitives.enable_fourier",
    "Enable Fourier Volume",
#ifdef MI_VOLUME_PRIMITIVES_ENABLE_FOURIER_DEFAULT
    true
#else
    false
#endif
);

static CVar<int> CVar_DensityFourierOrder(
    "r.volume_primitives.density_fourier_order",
    "The Fourier order of volume density",
    3
);

static CVar<int> CVar_ColorFourierOrder(
    "r.volume_primitives.color_fourier_order",
    "The Fourier order of volume color",
    3
);

static CVar<int> CVar_FourierSampleNum(
    "r.volume_primitives.fourier_sample_num",
    "The sample num of each intersection in calculating Fourier series",
    4
);

void Renderer::Render_DrawVolumePrimitives(RendererView *view, RenderGraphBuilder &builder) {
    RDGSectionGuard section(builder, "Render_DrawVolumePrimitives");
    glm::uvec2 tile_dimensions = {
        DivideAndRoundUp(view->film_width_, kTileSize),
        DivideAndRoundUp(view->film_height_, kTileSize)
    };
    uint32_t num_tiles = tile_dimensions.x * tile_dimensions.y;
    mi_check(num_tiles < (1<<13), "There're only 13 bits allocated for tile index in splatting sorting keys.");
    auto primitive_data = builder.Import(
        device_allocator_->GetCustomUberBuffer(VolumePrimitives::kVolumePrimitiveAllocatorUberBufferIndex)->GetRHI()
    );
    auto active_primitive_count = builder.CreateBuffer<uint32_t>();
    auto active_primitive_list = builder.CreateBuffer<uint32_t>(kMaxNumActiveVolumePrimitives);
    auto primitive_instance_count = builder.CreateBuffer<uint32_t>();
    auto primitive_instance_list_key = builder.CreateBuffer<uint32_t>(kMaxNumVolumePrimitiveInstances);
    auto primitive_instance_list_value = builder.CreateBuffer<uint32_t>(kMaxNumVolumePrimitiveInstances);
    auto primitive_instance_key_sorted = builder.CreateBuffer<uint32_t>(kMaxNumVolumePrimitiveInstances);
    auto primitive_instance_list_value_sorted = builder.CreateBuffer<uint32_t>(kMaxNumVolumePrimitiveInstances);
    auto tile_instance_count = builder.CreateBuffer<uint32_t>(num_tiles);
    auto tile_instance_offset = builder.CreateBuffer<uint32_t>(num_tiles);
    auto common_ub = builder.Allocate<RenderVolumePrimitivesUB>();
    auto renderable_transforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_transforms_.Raw());
    auto renderable_inverse_transforms = builder.Import(view->scene_->GetDeviceScene()->d_renderable_inverse_transforms_.Raw());
    common_ub->TileDimensions = tile_dimensions;
    common_ub->ExpandFactor = 1.f;
    common_ub->NumTiles = num_tiles;
    common_ub->MaxNumPrimitiveInstances = kMaxNumVolumePrimitiveInstances;
    common_ub->FrameIndex = view->persistent_data_->frame_index_;
    // Allowed relative distance for merging ray volume distributions rather than break them apart
    // |vol1| ..(distance).. |vol2|: if distance < epsilon * (size_of_vol1 + size_of_vol2), then merge
    common_ub->VolumeDistributionMergingEpsilon = CVar_VolumeDistributionMergingEpsilon.Get();
    common_ub->VolumeDistributionAlwaysUseClosest = CVar_VolumePrimitivesUseCloserRepresentativeDepth.Get() ? 1 : 0;
    common_ub->EnableFourier = CVar_EnableFourier.Get() ? 1 : 0;
    common_ub->DensityFourierOrder = CVar_DensityFourierOrder.Get();
    common_ub->ColorFourierOrder = CVar_ColorFourierOrder.Get();
    common_ub->FourierSampleNum = CVar_FourierSampleNum.Get();
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
        auto vol = view->volume_primitives_;
        params->RWVolumeMinMax = vol->G_volume_min_max_.Raw();
        params->RWVolumeDensity = vol->G_volume_density_.Raw();
        params->RWVolumeColor = vol->G_volume_color_.Raw();
        params->RWVolumeDensityFourier = vol->G_volume_density_fourier_.Raw();
        params->RWVolumeWeightedColorFourier = vol->G_volume_weighted_color_fourier_.Raw();
        params->RWVolumeCdfAttenuation = vol->G_volume_cdf_attenuation_.Raw();
        params->RWVolumeSampleColor = vol->volume_sample_color_.Raw();
        params->RWVolumeSampleLinearDepth = vol->volume_sample_linear_depth_.Raw();
        params->RWVolumeSampleTransmittanceAndPdf = vol->volume_sample_transmittance_and_pdf_.Raw();
        params->RWVolumeRepresentativeDepthAndVariation = vol->volume_representative_depth_and_variation_.Raw();
        params->RWTransmittance = view->G_transmittance_.Raw();

        params->G_Depth = view->G_depth_.Raw();
        params->RWFlags = view->G_flags_.Raw();
        params->PointEdgeSampler = RHI::Get().GetGlobalSamplers().point_edge;
    }
    auto & lib = RDGShaderLibrary::Get();
    {
        auto shader = lib.GetShader<VolumePrimitivesClearCountersShader>();
        auto groups = DivideAndRoundUp(num_tiles, VolumePrimitivesClearCountersShader::kThreadGroupSize);
        Helpers::AddComputePass(builder, shader, params, groups);
    }
    {
        auto shader = lib.GetShader<CollectVolumePrimitivesShader>();
        for (const auto& e : ctx.visible_renderables) {
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
        auto shader = lib.GetShader<ProjectVolumePrimitivesShader>();
        auto cmd = Helpers::SpawnDispatchIndirectCommand1D(builder, active_primitive_count.Raw(), ProjectVolumePrimitivesShader::kThreadGroupSize);
        Helpers::AddComputeIndirectPass(builder, shader, params, cmd.Raw());
    }
    {
        auto shader = lib.GetShader<ClampPrimitiveInstanceCountShader>();
        Helpers::AddComputePass(builder, shader, params);
    }
    DeviceRadixSort::AddRadixSort32BitsPass(builder, kMaxNumVolumePrimitiveInstances,
        primitive_instance_list_key.Raw(), primitive_instance_key_sorted.Raw(),
        primitive_instance_list_value.Raw(), primitive_instance_list_value_sorted.Raw(),
        primitive_instance_count.Raw()
    );
    auto instance_indirect_buffer = Helpers::SpawnDispatchIndirectCommand1D(
        builder, primitive_instance_count.Raw(), CollectTileInstanceOffsetsShader::kThreadGroupSize);
    {
        auto shader = lib.GetShader<CollectTileInstanceOffsetsShader>();
        Helpers::AddComputeIndirectPass(builder, shader, params, instance_indirect_buffer.Raw());
    }

    {
        auto shader = lib.GetShader<CountTileInstancesShader>();
        Helpers::AddComputePass(builder, shader, params, DivideAndRoundUp(num_tiles, CountTileInstancesShader::kThreadGroupSize));
    }

    {
        auto ini = RDGShaderInitializationInfo {};
        if (CVar_EnableFourier.Get()) {
            ini.optional_macros.push_back("ENABLE_FOURIER_VOLUME_RENDERING");
        }
        auto shader = lib.GetShader<DrawVolumePrimitivesShader>(ini);
        Helpers::AddComputePass(builder, shader, params, tile_dimensions.x, tile_dimensions.y);
    }
}
MI_NAMESPACE_END