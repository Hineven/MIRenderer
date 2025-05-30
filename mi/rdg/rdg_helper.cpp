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
MI_NAMESPACE_BEGIN

class SpawnDispatchIndirectCommand1DShader : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(SpawnDispatchIndirectCommand1DShaderParameters)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, Command)
        SHADER_RESOURCE_PARAMETER(StructuredBuffer, CountBuffer)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(SpawnDispatchIndirectCommand1DShaderParameters)
    DECLARE_SHADER()
};

IMPLEMENT_RDG_COMPUTE_SHADER(SpawnDispatchIndirectCommand1DShader, "mi/rdg/shaders/Helpers.hlsl", "SpawnDispatchIndirectCommand1D");

TRef<RDGBuffer> RDGHelper::SpawnDispatchIndirectCommand1D(RenderGraphBuilder &builder, RDGBuffer *count_buffer) {
    auto command = RDGBuffer::Create(
        RHIBufferUsageFlagBits::kIndirect | RHIBufferUsageFlagBits::kStorage,
        sizeof(RHIDispatchIndirectCommand)
    );
    command->SetName("DispatchIndirectCommand1D");
    auto params = builder.Allocate<SpawnDispatchIndirectCommand1DShader::SpawnDispatchIndirectCommand1DShaderParameters>();
    params->Command = command.Raw();
    params->CountBuffer = count_buffer;
    auto shader = RDGShaderLibrary::Get().GetShader<SpawnDispatchIndirectCommand1DShader>();
    builder.AddPass<SpawnDispatchIndirectCommand1DShader>(RDGPassFlagBits::kNeverCull, params,
        [shader, params, cmd = command.Raw(), count_buffer](RDGPass * pass, RHICommandQueueGraphics & queue) {
            RDGCommandHelper::Dispatch<SpawnDispatchIndirectCommand1DShader>(queue, pass, shader, params);
        }
    );
    return command;
}


MI_NAMESPACE_END