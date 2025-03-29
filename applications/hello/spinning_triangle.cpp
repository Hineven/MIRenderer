/*
 * Created: 2025/3/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "spinning_triangle.h"

#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_param.h>
#include <rhi/rhi_buffer.h>

#include "rdg/rdg.h"
#include "rdg/rdg_pool.h"

using namespace mi;

BEGIN_SHADER_PARAMETERS(TriangleRenderPass)
    SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, OutColor)
END_SHADER_PARAMETERS()

class TriangleShader : public RDGShader {
    DECLARE_SHADER()
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(float, SpinRadians)
        SHADER_VERTEX_BUFFER(12 + 8, vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k3xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 12, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_USE_RENDERPASS(TriangleRenderPass, pass)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    static std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(TriangleShader, "triangle.hlsl", "TriangleVS", "TrianglePS");

void RenderFrame (TRef<RDGResourcePool> pool) {
    RenderGraphBuilder builder;

    auto & rhi = RHI::Get();

    auto & lib = RDGShaderLibrary::Get();
    auto shader = lib.GetShader<TriangleShader>();

    auto fb = RDGTexture::Import(rhi.GetBackBuffer());

    auto vertex_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kVertex, sizeof(float) * 5 * 3);
    auto staging_buffer = rhi.CreateBuffer(sizeof(float) * 5 * 3, RHIBufferUsageFlagBits::kStaging);
    float vertices[] = {
        -0.5f, -0.5f, 0.1f, 0.f, 0.f,
         0.5f, -0.5f, 0.1f, 0.f, 1.f,
         0.f,   0.5f, 0.1f, 1.f, 0.f
    };
    memcpy(staging_buffer->Map(), vertices, sizeof(float) * 5 * 3);

    auto pass = builder.Allocate<TriangleRenderPass>();
    pass->OutColor = fb.Raw();

    auto shader_params = builder.Allocate<TriangleShader::ShaderParameters>();
    shader_params->pass = pass;
    shader_params->vertex_buffer = vertex_buffer.Raw();
    shader_params->SpinRadians = float(rhi.GetFrameIndex()) * 0.006f;

    builder.AddPass("Upload Vertices", {},
        [shader_params, staging_raw = staging_buffer.Raw(), vertex_raw = vertex_buffer.Raw()]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & cmd) {
        cmd.CopyBuffer(staging_raw->GetSpan(), vertex_raw->GetRHI());
    })->AddBuffer(vertex_buffer.Raw(), RHIGPUAccessFlagBits::kWrite);

    builder.AddPass<TriangleShader>(RDGPassFlagBits::kNeverCull, shader_params, [shader, shader_params]([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & cmd) {
        RDGCommandHelper::Draw<TriangleShader>(cmd, pass, shader, shader_params, 3, 1);
    });

    auto graph = builder.Compile();

    graph->Execute(pool.Raw());
}
