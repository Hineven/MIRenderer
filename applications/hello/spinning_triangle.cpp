/*
 * Created: 2025/3/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <imgui.h>
#include "spinning_triangle.h"

#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_param.h>
#include <rhi/rhi_buffer.h>

#include "rdg/rdg.h"
#include "rdg/rdg_pool.h"

using namespace mi;

BEGIN_SHADER_PARAMETERS(BackbufferRenderPass)
    SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, OutColor)
END_SHADER_PARAMETERS()
class TriangleShader : public RDGShader {
    DECLARE_SHADER()
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(float, SpinRadians)
        SHADER_VERTEX_BUFFER(12 + 8, vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k3xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 12, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_USE_RENDERPASS(BackbufferRenderPass, pass)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    static std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(TriangleShader, "triangle.hlsl", "TriangleVS", "TrianglePS");

void RenderTriangle (TRef<RDGResourcePool> pool, RenderGraphBuilder & builder) {

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

    auto pass = builder.Allocate<BackbufferRenderPass>();
    pass->OutColor = fb.Raw();
    pass->OutColor.load_op = RHILoadOpType::kClear;

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
}

class ImGuiRenderShader : public RDGShader {
    DECLARE_SHADER()
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(float2, Scale)
        SHADER_USE_RENDERPASS(BackbufferRenderPass, pass)
        SHADER_VERTEX_BUFFER(sizeof(ImDrawVert), vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k2xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 8, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_VERTEX_ATTRIBUTE(0, 16, RHIVertexAttributeFormatType::k1xFp32, col) // This is a uint actually
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
};

IMPLEMENT_RDG_GRAPHICS_SHADER(ImGuiRenderShader, "imgui.hlsl", "ImGuiVS", "ImGuiPS");


void RenderImGui (TRef<RDGResourcePool> pool, RenderGraphBuilder & builder) {
    ImGui::Render();
    ImDrawData * draw_data = ImGui::GetDrawData();
    if (draw_data->TotalVtxCount == 0 || draw_data->TotalIdxCount == 0) {
        return;
    }
    auto vertex_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kVertex, draw_data->TotalVtxCount * sizeof(ImDrawVert));
    auto index_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kIndex, draw_data->TotalIdxCount * sizeof(ImDrawIdx));

    auto & rhi = RHI::Get();
    auto vertex_staging = rhi.CreateBuffer(draw_data->TotalVtxCount * sizeof(ImDrawVert), RHIBufferUsageFlagBits::kStaging);
    auto index_staging = rhi.CreateBuffer(draw_data->TotalIdxCount * sizeof(ImDrawIdx), RHIBufferUsageFlagBits::kStaging);
    // Upload vertex data
    size_t vertex_buffer_byte_offset = 0;
    size_t index_buffer_byte_offset = 0;
    for (int i = 0; i < draw_data->CmdListsCount; i++) {
        const ImDrawList * cmd_list = draw_data->CmdLists[i];
        size_t vertex_size = cmd_list->VtxBuffer.Size * sizeof(ImDrawVert);
        size_t index_size = cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx);
        memcpy((std::byte*)vertex_staging->Map() + vertex_buffer_byte_offset, cmd_list->VtxBuffer.Data, vertex_size);
        memcpy((std::byte*)index_staging->Map() + index_buffer_byte_offset, cmd_list->IdxBuffer.Data, index_size);
        vertex_buffer_byte_offset += vertex_size;
        index_buffer_byte_offset += index_size;
    }

    auto fb = RDGTexture::Import(rhi.GetBackBuffer());
    auto pass = builder.Allocate<BackbufferRenderPass>();
    pass->OutColor = fb.Raw();
    pass->OutColor.load_op = RHILoadOpType::kLoad;
    pass->OutColor.store_op = RHIStoreOpType::kStore;
    auto params = builder.Allocate<ImGuiRenderShader::ShaderParameters>();
    params->pass = pass;
    params->vertex_buffer = vertex_buffer.Raw();
    params->Scale = {1.f / 800, 1.f / 600}; // TODO: Get actual scale

    // Upload vertex data
    builder.AddPass("Upload ImGui Vertices", {},
        [vertex_raw = vertex_buffer.Raw(), vertex_staging_raw = vertex_staging.Raw(),
            index_raw = index_buffer.Raw(), index_staging_raw = index_staging.Raw()]
        ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & cmd) {
        cmd.CopyBuffer(vertex_staging_raw->GetSpan(), vertex_raw->GetRHI());
        cmd.CopyBuffer(index_staging_raw->GetSpan(), index_raw->GetRHI());
    })->AddBuffer(vertex_buffer.Raw(), RHIGPUAccessFlagBits::kWrite)
    ->AddBuffer(index_buffer.Raw(), RHIGPUAccessFlagBits::kWrite);

    // Collect all ImDrawCmd
    std::vector<ImDrawCmd> draw_cmds;
    for (int i = 0; i < draw_data->CmdListsCount; i++) {
        const ImDrawList* cmd_list = draw_data->CmdLists[i];
        for (int j = 0; j < cmd_list->CmdBuffer.Size; j++) {
            const ImDrawCmd* pcmd = &cmd_list->CmdBuffer[j];
            draw_cmds.push_back(*pcmd);
            draw_cmds.back().UserCallbackDataOffset = 0;
        }
        // Use this unused field to store the vertex buffer offset after completion of each draw list
        draw_cmds.back().UserCallbackDataOffset = cmd_list->VtxBuffer.Size;
    }

    // Draw
    builder.AddPass<ImGuiRenderShader>(RDGPassFlagBits::kNeverCull, params, [
        index_raw = index_buffer.Raw(), draw_cmds, params
    ](RDGPass * pass, RHICommandQueueGraphics & cmd) {
        auto shader = RDGShaderLibrary::Get().GetShader<ImGuiRenderShader>();
        RDGCommandHelper::BindGraphicsShader(cmd, pass, shader, params);
        cmd.BeginRendering();
        int vertex_offset = 0;
        int index_offset = 0;
        for (auto draw_cmd : draw_cmds) {
            cmd.SetScissor(
                (int)draw_cmd.ClipRect.x, (int)draw_cmd.ClipRect.y,
                (uint32_t)(draw_cmd.ClipRect.z - draw_cmd.ClipRect.x),
                (uint32_t)(draw_cmd.ClipRect.w - draw_cmd.ClipRect.y));
            cmd.DrawIndexedPrimitive(index_raw->GetRHI(), draw_cmd.ElemCount, 1,
                           index_offset, vertex_offset, 0, RHIIndexType::kUint16);
            index_offset += draw_cmd.ElemCount;
            vertex_offset += draw_cmd.UserCallbackDataOffset;
        }
        cmd.EndRendering();
    })->AddBuffer(index_buffer.Raw(), RHIGPUAccessFlagBits::kRead);
}

void RenderFrame(mi::TRef<mi::RDGResourcePool> pool) {

    ImGui::Begin("Rendering");
    ImGui::Text("Hello");
    ImGui::End();

    RenderGraphBuilder builder;
    RenderTriangle(pool, builder);
    RenderImGui(pool, builder);
    auto graph = builder.Compile();
    graph->Execute(pool.Raw());
}
