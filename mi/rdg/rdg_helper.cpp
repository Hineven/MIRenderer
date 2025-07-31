/*
 * Created: 2025/5/30
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <rdg/rdg_helper.h>
#include <rhi/rhi_desc.h>

#include "rdg/rdg_builder.h"
#include "rdg/rdg_cmd.h"
#include "rdg/rdg_shader.h"
#include "rhi/rhi_buffer.h"
MI_NAMESPACE_BEGIN

void Helpers::Clear(RenderGraphBuilder &builder, RDGTexture *texture, glm::vec4 clear_value, uint32_t mip_level, uint32_t base_layer, uint32_t num_layers) {
    builder.AddPass("ClearTexture", RDGPassType::kGeneric, {}, {}, {}, {},
        [texture, clear_value, mip_level, base_layer, num_layers](RDGPass * pass, RHICommandQueueGraphics & queue) {
            std::array<float, 4> arr = {clear_value.x, clear_value.y, clear_value.z, clear_value.w};
            queue.ClearTexture(texture->GetRHI(), arr, mip_level, base_layer, num_layers);

    })->AddTexture(texture,
        RHITextureLayoutType::kTransferDstOptimal,
        RHIGPUAccessFlagBits::kTransferWrite,
        RHIPipelineStageFlagBits::kTransfer
    );
}


class SpawnDispatchIndirectCommand1DShader : public RDGShader {
public:
    struct SpawnDispatchIndirectCommand1DUB {
        uint32_t UpDivisor; // The divisor for calculating the number of dispatches
        uint32_t Padding0;
        uint32_t Padding1;
        uint32_t Padding2;
    };
    BEGIN_SHADER_PARAMETERS(SpawnDispatchIndirectCommand1DShaderParameters)
        SHADER_UNIFORM_BUFFER(SpawnDispatchIndirectCommand1DUB, UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, Command)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SpawnDispatchIndirectCommand1DShaderParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER(SpawnDispatchIndirectCommand1DShader, "mi/rdg/shaders/Helpers.hlsl", "SpawnDispatchIndirectCommand1D");

TRef<RDGBuffer> Helpers::SpawnDispatchIndirectCommand1D(RenderGraphBuilder &builder, RDGBuffer *count_buffer, uint32_t up_divisor) {
    auto command = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect | RHIBufferUsageFlagBits::kStorage,
        sizeof(RHIDispatchIndirectCommand)
    );
    command->SetName("DispatchIndirectCommand1D");
    auto params = builder.Allocate<SpawnDispatchIndirectCommand1DShader::SpawnDispatchIndirectCommand1DShaderParameters>();
    params->UB = builder.Allocate<SpawnDispatchIndirectCommand1DShader::SpawnDispatchIndirectCommand1DUB>();
    params->UB->UpDivisor = up_divisor;
    params->Command = command.Raw();
    params->Count = count_buffer;
    auto shader = RDGShaderLibrary::Get().GetShader<SpawnDispatchIndirectCommand1DShader>();
    builder.AddPass<SpawnDispatchIndirectCommand1DShader>({}, shader, params,
        [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::Dispatch<SpawnDispatchIndirectCommand1DShader>(queue, pass, shader, params);
        }
    );
    return command;
}

class SpawnTraceRaysIndirectCommand1DShader : public RDGShader {
public:
    struct SpawnTraceRaysIndirectCommand1DUB {
        uint64_t RaygenAddr;
        uint64_t RaygenSize;
        uint64_t MissAddr;
        uint64_t MissSize;
        uint64_t MissStride;
        uint64_t HitAddr;
        uint64_t HitSize;
        uint64_t HitStride;
    };
    BEGIN_SHADER_PARAMETERS(SpawnTraceRaysIndirectCommand1DShaderParameters)
        SHADER_UNIFORM_BUFFER(SpawnTraceRaysIndirectCommand1DUB, SpawnTraceRaysIndirectCommand1D_UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, SpawnTraceRaysIndirectCommand1D_Command)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, Count)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SpawnTraceRaysIndirectCommand1DShaderParameters)
    DECLARE_SHADER()
    static std::vector<std::string> GetShaderOptionalMacros() {
        return {"GRAPHICS_API=0"}; // Graphics apis, 0 for vulkan
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(SpawnTraceRaysIndirectCommand1DShader, "mi/rdg/shaders/Helpers.hlsl", "SpawnTraceRaysIndirectCommand1D");


TRef<RDGBuffer> Helpers::SpawnTraceRaysIndirectCommand1D(RenderGraphBuilder &builder, RDGShader * ray_tracing_shader, RDGBuffer *count_buffer) {
    if (!ray_tracing_shader || !ray_tracing_shader->IsValid()) {
        MI_WARN("Can not spawn trace rays indirect command for invalid ray tracing shaders.");
        return nullptr;
    }
    auto command = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect | RHIBufferUsageFlagBits::kStorage,
        sizeof(RHITraceRaysIndirectCommand)
    );
    command->SetName("TraceRaysIndirectCommand1D");
    auto ini = RDGShaderInitializationInfo{};
    if (RHI::Get().GetType() == RHIType::kVulkan) {
        ini.optional_macros.push_back("GRAPHICS_API=0");
    } else {
        assert(false);
    }
    auto shader = RDGShaderLibrary::Get().GetShader<SpawnTraceRaysIndirectCommand1DShader>(ini);
    auto sbt = ray_tracing_shader->GetSBTBuffers(RHI::Get().GetGraphicsCommandQueue());

    auto params = builder.Allocate<SpawnTraceRaysIndirectCommand1DShader::SpawnTraceRaysIndirectCommand1DShaderParameters>();
    params->SpawnTraceRaysIndirectCommand1D_Command = command.Raw();
    params->Count = count_buffer;
    {
        auto UB = builder.Allocate<SpawnTraceRaysIndirectCommand1DShader::SpawnTraceRaysIndirectCommand1DUB>();
        UB->RaygenAddr = sbt.raygen.buffer->GetDeviceAddress() + sbt.raygen.offset;
        UB->RaygenSize = sbt.raygen.size;
        UB->MissAddr = sbt.miss.buffer->GetDeviceAddress() + sbt.miss.offset;
        UB->MissSize = sbt.miss.size;
        UB->MissStride = sbt.miss_stride;
        UB->HitAddr = sbt.hit.buffer->GetDeviceAddress() + sbt.hit.offset;
        UB->HitSize = sbt.hit.size;
        UB->HitStride = sbt.hit_stride;
        params->SpawnTraceRaysIndirectCommand1D_UB = UB;
    }
    builder.AddPass<SpawnTraceRaysIndirectCommand1DShader>({}, shader, params,
        [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::Dispatch<SpawnTraceRaysIndirectCommand1DShader>(queue, pass, shader, params);
        }
    );
    return command;
}


void Helpers::Upload_Async(RHICommandQueueGraphics &queue, RHIBufferSpan buffer, const void *data, size_t size) {
    auto & rhi = RHI::Get();
    auto staging_buffer = rhi.CreateBuffer(size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, size);
    staging_buffer->Unmap();
    queue.BufferBarrier(buffer, RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
    queue.CopyBuffer(staging_buffer->GetSpan(), buffer);
    queue.BufferBarrier(buffer, RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAll);
}

void Helpers::Upload_Async(RHICommandQueueGraphics &queue, RHITexture *texture, const void *data, size_t size, RHITextureLayoutType dst_layout, RHIGPUAccessFlags dst_access) {
    auto & rhi = RHI::Get();
    auto staging_buffer = rhi.CreateBuffer(size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, size);
    staging_buffer->Unmap();
    queue.TextureBarrier(texture, RHITextureLayoutType::kTransferDstOptimal, RHIPipelineStageFlagBits::kAll,
        RHIPipelineStageFlagBits::kTransfer, RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
    queue.CopyBufferToTexture(staging_buffer->GetSpan(), texture);
    queue.TextureBarrier(texture, dst_layout, RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kTransferWrite, dst_access);
}


void Helpers::Upload_Async(RHIBufferSpan buffer, const void *data, size_t size) {
    Upload_Async(RHI::Get().GetGraphicsCommandQueue(), buffer, data, size);
}


void Helpers::UploadWithRDG_Unsafe(RenderGraphBuilder & builder, RHIBufferSpan buffer, const void * data) {
    auto & rhi = RHI::Get();
    auto staging_buffer = rhi.CreateBuffer(buffer.size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, buffer.size);
    staging_buffer->Unmap();
    builder.AddPass("UploadWithRDG_Unsafe", RDGPassType::kGeneric, {}, {}, {}, {},
        [src = staging_buffer.Raw(), dst = buffer]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            queue.BufferBarrier(dst,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
            queue.CopyBuffer(src->GetSpan(), dst);
            queue.BufferBarrier(dst,
                RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAll);
        }
    );
}

void Helpers::UploadWithRDG(RenderGraphBuilder &builder, RDGBuffer * buffer, const void *data, size_t size, size_t dst_offset) {
    auto & rhi = RHI::Get();
    auto staging_buffer = rhi.CreateBuffer(size, RHIBufferUsageFlagBits::kStaging);
    auto staging_buffer_ptr = static_cast<uint8_t *>(staging_buffer->Map());
    memcpy(staging_buffer_ptr, data, size);
    staging_buffer->Unmap();
    builder.AddPass("UploadWithRDG", RDGPassType::kGeneric, {}, {}, {}, {},
        [src = staging_buffer.Raw(), dst = buffer, size, dst_offset]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            auto dst_span = dst->GetRHI();
            dst_span.size = size;
            dst_span.offset += dst_offset;
            queue.CopyBuffer(src->GetSpan(), dst_span);
        }
    )->AddBufferH(buffer, RHIGPUAccessFlagBits::kTransferWrite);
}

void Helpers::ReadbackWithRDG(RenderGraphBuilder &builder, RDGBuffer *buffer, size_t src_offset, RHIBufferSpan readback_buffer) {
    mi_assert(readback_buffer.buffer->GetBufferUsage() & RHIBufferUsageFlagBits::kReadback, "Must be a readback buffer with corresponding usage.");
    mi_assert(src_offset < buffer->GetRequestedSize(), "OOB: offset must be less than the source buffer size.");
    size_t max_size = buffer->GetRequestedSize() - src_offset;
    mi_assert(readback_buffer.size <= max_size, "OOB: no enough data in the source buffer to read into the readback buffer.");

    builder.AddPass("ReadbackWithRDG", RDGPassType::kGeneric, RDGPassFlagBits::kNeverCull, {}, {}, {},
        [src = buffer, src_offset, dst = readback_buffer]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            auto src_span = src->GetRHI();
            src_span.offset += src_offset;
            src_span.size = dst.size;
            queue.BufferBarrier(dst,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
            queue.CopyBuffer(src_span, dst);
        }
    )->AddBufferH(buffer, RHIGPUAccessFlagBits::kTransferRead);
}


void Helpers::ReadbackWithRDG_Unsafe(RenderGraphBuilder &builder, RHIBufferSpan buffer, RHIBufferSpan readback_buffer) {
    mi_assert(buffer.size == readback_buffer.size, "Readback buffer size must match the source buffer size.");
    mi_assert(readback_buffer.buffer->GetBufferUsage() & RHIBufferUsageFlagBits::kReadback, "Must be a readback buffer with corresponding usage.");
    builder.AddPass("ReadbackWithRDG_Unsafe", RDGPassType::kGeneric, RDGPassFlagBits::kNeverCull, {}, {}, {},
        [src = buffer, dst = readback_buffer]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
            queue.BufferBarrier(src,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferRead);
            queue.BufferBarrier(dst,
                RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
                RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
            queue.CopyBuffer(src, dst);
            queue.BufferBarrier(src,
                RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kTransferRead, RHIGPUAccessFlagBits::kAll);
        }
    );
}

void Helpers::Readback(RHICommandQueueGraphics & queue, RHIBufferSpan buffer, void * data) {
    auto & rhi = RHI::Get();
    auto readback_buffer = rhi.CreateBuffer(buffer.size, RHIBufferUsageFlagBits::kReadback);

    queue.BufferBarrier(buffer,
        RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferRead);
    queue.BufferBarrier(readback_buffer->GetSpan(),
        RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
    queue.CopyBuffer(buffer, readback_buffer->GetSpan());
    queue.BufferBarrier(readback_buffer->GetSpan(),
        RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kTransferRead, RHIGPUAccessFlagBits::kAll);

    queue.WaitForIdle("Helpers::Readback");

    auto src_ptr = static_cast<const uint8_t*>(readback_buffer->Map());
    memcpy(data, src_ptr, buffer.size);
    readback_buffer->Unmap();
}

void Helpers::Clear_Async(RHICommandQueueGraphics &queue, RHIBufferSpan buffer, uint32_t clear_value) {
    queue.BufferBarrier(buffer,
        RHIPipelineStageFlagBits::kAll, RHIPipelineStageFlagBits::kTransfer,
        RHIGPUAccessFlagBits::kAll, RHIGPUAccessFlagBits::kTransferWrite);
    queue.ClearBuffer(buffer, clear_value);
    queue.BufferBarrier(buffer, RHIPipelineStageFlagBits::kTransfer, RHIPipelineStageFlagBits::kAll,
        RHIGPUAccessFlagBits::kTransferWrite, RHIGPUAccessFlagBits::kAll);
}


MI_NAMESPACE_END