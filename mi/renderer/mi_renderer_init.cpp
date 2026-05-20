/*
 * Created: 2025/9/24
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <rdg/rdg_helper.h>
#include <renderer/mi_renderer.h>
#include <renderer/mi_noise.h>
#include <renderer/mi_resource_allocator.h>
#include <renderer/mi_volume_primitives.h>
#include <renderer/mi_gaussian_radiance_field.h>
#include "dlss/ngx_context.h"
#include "dlss/dlss_rr_context.h"
#include "sobol_samples.h"
#include "rhi/rhi_buffer.h"

MI_NAMESPACE_BEGIN
void Renderer::Init(DeviceBindlessResourceAllocator * allocator, RDGResourcePool * pool) {
    device_allocator_ = allocator;
    pool_ = pool;
    // Do some initialization related to special data structures.
    VolumePrimitives::SetupAllocatorUberBuffer(device_allocator_.Raw());
    GaussianRadianceField::SetupAllocatorUberBuffer(device_allocator_.Raw());
    // Initialize blue noise texture
    {
        blue_noise_128x128_ = RHI::Get().CreateTexture(RHITextureType::k2D, {128, 128, 1}, PixelFormatType::kR32_FLOAT,
            RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransfer);
        auto blue_noise_data_128x128 = NoiseHelpers::BlueNoiseTexture2D(128, 128);
        Helpers::Upload_Async(
            RHI::Get().GetGraphicsCommandQueue(), blue_noise_128x128_.Raw(),
            blue_noise_data_128x128.data(), sizeof(float) * blue_noise_data_128x128.size(),
            RHITextureLayoutType::kShaderReadOnlyOptimal, RHIGPUAccessFlagBits::kShaderRead
        );
    }
    // Initialize sobol sampler
    {
        sobol_256x256_ = RHI::Get().CreateBuffer(
            sizeof(uint8_t) * 256 * 256,
            RHIBufferUsageFlagBits::kStorage);
        sobol_scrambling_tile_256x256x8_ = RHI::Get().CreateBuffer(
            sizeof(uint8_t) * 256 * 256 * 8,
            RHIBufferUsageFlagBits::kStorage);
        Helpers::Upload_Async(sobol_256x256_->GetSpan(), Sobol256x256, sizeof(Sobol256x256));
        Helpers::Upload_Async(sobol_scrambling_tile_256x256x8_->GetSpan(), ScramblingTiles, sizeof(ScramblingTiles));
    }
    // Register console commands for the renderer
    Console::RegisterCommands();

    // Initialize DLSS Ray Reconstruction as optional component
    if (NGXContext::ProbeAvailability()) {
        ngx_context_ = TRef<NGXContext>(new NGXContext());
        if (!ngx_context_->Initialize()) {
            MI_LOG(MIInfraLogType::kWarning, "NGX initialization failed, DLSS Ray Reconstruction disabled.");
            ngx_context_ = nullptr;
        } else {
            MI_LOG(MIInfraLogType::kInfo, "NGX initialized successfully.");
        }
    } else {
        MI_LOG(MIInfraLogType::kInfo, "DLSS Ray Reconstruction not available (non-RTX GPU or NGX not found).");
    }
}

MI_NAMESPACE_END