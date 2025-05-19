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

class TriangleShader : public RDGShader {
    DECLARE_SHADER()
    struct TriangleShaderUB {
        float SpinRadians;
        glm::vec3 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(TriangleShaderUB, UB)
        SHADER_VERTEX_BUFFER(12 + 8, vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k3xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 12, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, OutColor)
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


    auto shader_params = builder.Allocate<TriangleShader::ShaderParameters>();
    shader_params->OutColor = fb.Raw();
    shader_params->OutColor.load_op = RHILoadOpType::kClear;
    shader_params->vertex_buffer = vertex_buffer.Raw();
    shader_params->UB = builder.Allocate<TriangleShader::TriangleShaderUB>();
    shader_params->UB->SpinRadians = float(rhi.GetFrameIndex()) * 0.006f;

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
    struct ImGuiRenderShaderUB {
        glm::vec2 Scale;
        glm::vec2 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(ImGuiRenderShaderUB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2D, ImGuiTexture)
        SHADER_RESOURCE_PARAMETER(SamplerState, ImGuiSampler)
        SHADER_RENDER_TARGET(PixelFormatType::kB8G8R8A8_SRGB, OutColor)
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
    auto params = builder.Allocate<ImGuiRenderShader::ShaderParameters>();
    params->OutColor = fb.Raw();
    params->OutColor.load_op = RHILoadOpType::kLoad;
    params->OutColor.store_op = RHIStoreOpType::kStore;
    params->vertex_buffer = vertex_buffer.Raw();
    auto window_size = ImGui::GetIO().DisplaySize;
    params->UB = builder.Allocate<ImGuiRenderShader::ImGuiRenderShaderUB>();
    params->UB->Scale = {1.f / window_size.x, 1.f / window_size.y};
    params->ImGuiTexture = nullptr; // This parameter is set inside the draw pass
    params->ImGuiSampler = rhi.GetGlobalSamplers().linear_wrap;

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
        ImTextureID prev_texture_id = (ImTextureID)-1;
        for (auto draw_cmd : draw_cmds) {
            if (draw_cmd.GetTexID() != prev_texture_id) {
                // Bind sampled texture if texture ID changed
                prev_texture_id = draw_cmd.GetTexID();
                auto texture = (RHITexture*)prev_texture_id;
                RHIBindPipelineParametersDesc desc {};
                auto texture_descs = cmd.Allocate<RHIPipelineParameterTextureDesc[]>(1);
                texture_descs[0] = {texture, 0};
                desc.srvs = {texture_descs, 1};
                cmd.BindPipelineParameters(RHIBindPointType::kGraphics, desc);
            }
            cmd.SetScissor(
                (int)draw_cmd.ClipRect.x, (int)draw_cmd.ClipRect.y,
                (uint32_t)(draw_cmd.ClipRect.z - draw_cmd.ClipRect.x),
                (uint32_t)(draw_cmd.ClipRect.w - draw_cmd.ClipRect.y)
            );
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
