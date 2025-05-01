/*
 * Created: 2025/4/27
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"
#include "rhi/rhi_thread.h"
#include "rhi/rhi_shader.h"
#include "rhi/rhi_pipeline.h"
#include "rhi/rhi_texture.h"
#include "rhi/rhi_param.h"
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"

#include <exception>
#include <cpptrace/from_current.hpp>

#include "stb_image.h"
#include "stb_image_write.h"

#include <rhi/rhi_buffer.h>

TEST(RHITest, RHIBindlessBasics) {
    using namespace mi;
    
    CPPTRACE_TRY {
        // 设置基础环境
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            // 创建测试用的资源
            // 1. 创建buffer资源
            auto buffer = RHI::Get().CreateBuffer(
                1024,
                RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kReadback
            );
            EXPECT_TRUE(buffer);

            // 写入一些数据供测试
            uint32_t* buffer_data = reinterpret_cast<uint32_t*>(buffer->Map());
            buffer_data[0] = 0xDEADBEEF;
            buffer_data[1] = 0x12345678;

            // 2. 创建纹理资源
            auto texture = RHI::Get().CreateTexture(
                RHITextureType::k2D,
                RHITextureDimensions{64, 64},
                PixelFormatType::kR8G8B8A8_UNORM,
                RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
            );
            EXPECT_TRUE(texture);

            // 测试 Bindless 资源槽创建
            MI_LOG(MIInfraLogType::kInfo, "创建 Bindless 资源槽");

            // 1. 创建 Buffer 的 Bindless 槽
            auto buffer_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHIBuffer>();
            EXPECT_TRUE(buffer_slot);
            EXPECT_TRUE(buffer_slot->GetType() == RHIBindlessResourceType::kReadOnlyStorageBuffer);

            // 2. 创建 Texture 的 Bindless 槽
            auto texture_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();
            EXPECT_TRUE(texture_slot);
            EXPECT_TRUE(texture_slot->GetType() == RHIBindlessResourceType::kSRV);

            // 3. 记录初始槽索引
            uint32_t buffer_slot_index = buffer_slot->GetSlot();
            uint32_t texture_slot_index = texture_slot->GetSlot();

            MI_LOG(MIInfraLogType::kInfo, "分配的 Buffer Slot: {}", buffer_slot_index);
            MI_LOG(MIInfraLogType::kInfo, "分配的 Texture Slot: {}", texture_slot_index);

            // 设置资源到槽位并提交
            MI_LOG(MIInfraLogType::kInfo, "设置资源到 Bindless 槽位");
            buffer_slot->SetAndCommit(buffer.Raw());
            texture_slot->SetAndCommit(texture.Raw());

            // 验证 Get 方法能正确获取到设置的资源
            EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
            EXPECT_EQ(texture_slot->Get(), texture.Raw());

            // 推进一帧，测试资源的持久性
            RHI::Get().AdvanceFrame();

            RHI::Get().WaitForIdle();

            // 重新检查资源是否仍然存在
            EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
            EXPECT_EQ(texture_slot->Get(), texture.Raw());

            // 重置槽位资源后测试
            MI_LOG(MIInfraLogType::kInfo, "重置资源并测试空槽位");
            buffer_slot->SetAndCommit(nullptr);
            texture_slot->SetAndCommit(nullptr);

            EXPECT_EQ(buffer_slot->Get(), nullptr);
            EXPECT_EQ(texture_slot->Get(), nullptr);

            // 测试批量更新
            MI_LOG(MIInfraLogType::kInfo, "测试批量更新");
            buffer_slot->Set(buffer.Raw());
            texture_slot->Set(texture.Raw());

            // 批量提交更新
            RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(buffer_slot.Raw());
            RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(texture_slot.Raw());

            // 验证更新后的资源
            EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
            EXPECT_EQ(texture_slot->Get(), texture.Raw());

            // 释放资源槽
            MI_LOG(MIInfraLogType::kInfo, "释放资源槽");
            buffer_slot = nullptr;  // 引用计数归零，应该会触发资源槽释放
            texture_slot = nullptr;

            // 推进一帧，使延迟释放的槽位真正释放
            RHI::Get().AdvanceFrame();
        }
        
        // 清理
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}


static auto v_shader_code = "// Vertex Shader\n"
                     "struct VSInput {\n"
                     "    float3 position : POSITION;\n"
                     "    float2 uv: TEXCOORD0;\n"
                     "};\n"
                     "struct VSOutput {\n"
                     "    float4 position : SV_POSITION;\n"
                     "    float2 uv : TEXCOORD0;\n"
                     "};\n"
                     "\n"
                     "VSOutput Main(VSInput input, in uint vid : SV_VertexID) {\n"
                     "    VSOutput output;\n"
                     "    output.position = float4(input.position, 1.0);"
                     "    output.uv = input.uv;\n"
                     "    return output;\n"
                     "}";
static auto f_shader_code = "// Fragment Shader\n"
                     "Texture2D __internal__BindlessIndicesBuffer_Texture[];"
                     "SamplerState sampler_linear;"
                     "uint tex_index;"
                     "struct PSInput {\n"
                     "    float2 uv: TEXCOORD0;\n"
                     "};\n"
                     "struct PSOutput {"
                     "    float4 color : SV_Target0;\n"
                     "};\n"
                     "PSOutput Main(PSInput input) {\n"
                     "    PSOutput output;\n"
                     "    output.color = float4(__internal__BindlessIndicesBuffer_Texture[tex_index].SampleLevel(sampler_linear, input.uv, 0));\n"
                     "    return output;\n"
                     "}";

TEST(RHITest, RHIBindlessTextureDraw) {
    using namespace mi;
    CPPTRACE_TRY {
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();
        // Hack: we need to pretend that we're a render thread to pass the assertions
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        // Sim render thread scope
        {
            std::vector<std::string> options;
            options.push_back("-fspv-target-env=vulkan1.3");
            // options.push_back("-fvk-use-scalar-layout");
            options.push_back("-Zi");

            std::string errmsg;

            auto v_shader_bcode = GetInfra().CompileHLSLToSPIRV(
                    L"", "Main", "vs_6_3", std::span(v_shader_code, strlen(v_shader_code)), options, errmsg);
            if (v_shader_bcode.empty()) {
                MI_LOG(MIInfraLogType::kError, "Failed to compile vertex shader: {}", errmsg);
            }
            auto v_shader = RHI::Get().CreateShader(
                    RHIShaderFrequencyFlagBits::kVertex, "Main",
                    RHIShaderIRType::kSPIRV, std::span(reinterpret_cast<const std::byte *>(v_shader_bcode.data()),
                                                       v_shader_bcode.size() * sizeof(uint32_t))
            );
            EXPECT_TRUE(v_shader);
            // Check reflection
            auto vinputs = v_shader->GetVertexInputDesc();
            EXPECT_TRUE(vinputs.size() == 2);
            EXPECT_TRUE(vinputs[0].name == "POSITION");
            EXPECT_TRUE(vinputs[1].name == "TEXCOORD0");
            EXPECT_TRUE(vinputs[0].format == RHIVertexAttributeFormatType::k3xFp32);
            EXPECT_TRUE(vinputs[1].format == RHIVertexAttributeFormatType::k2xFp32);

            auto f_shader_bcode = GetInfra().CompileHLSLToSPIRV(
                    L"", "Main", "ps_6_3", std::span(f_shader_code, strlen(f_shader_code)), options, errmsg);
            if (f_shader_bcode.empty()) {
                MI_LOG(MIInfraLogType::kError, "Failed to compile fragment shader: {}", errmsg);
            }
            auto f_shader = RHI::Get().CreateShader(
                    RHIShaderFrequencyFlagBits::kFragment, "Main",
                    RHIShaderIRType::kSPIRV, std::span(reinterpret_cast<const std::byte *>(f_shader_bcode.data()),
                                                       f_shader_bcode.size() * sizeof(uint32_t))
            );
            EXPECT_TRUE(f_shader);
            // Check reflection
            auto foutputs = f_shader->GetFragmentOutputDesc();
            EXPECT_TRUE(foutputs.size() == 1);
            EXPECT_TRUE(foutputs[0].name == "SV_Target0");
            EXPECT_TRUE(foutputs[0].format == RHIFragmentOutputFormatType::k4xFp32);

            auto attachment_descs = std::vector<RHIColorAttachmentDesc>{
                    RHIColorAttachmentDesc{
                            .blending = RHIColorAttachmentBlendDesc{
                                    false,
                                    RHIBlendFactorType::kSrcAlpha,
                                    RHIBlendFactorType::kOneMinusSrcAlpha,
                                    RHIBlendFactorType ::kOne,
                                    RHIBlendFactorType::kOne,
                                    static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd),
                                    static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd)
                            },
                            .format = PixelFormatType::kR8G8B8A8_SRGB,
                    }
            };

            auto vertex_attribute_descs = v_shader->GetVertexInputAttributeDescForPipeline(0);
            auto binding_descs = std::vector<RHIVertexInputBindingDesc>{
                    RHIVertexInputBindingDesc{
                            .binding = 0,
                            .stride = v_shader->GetVertexStride(),
                            .input_rate = RHIVertexInputRateType::kVertex
                    }
            };
            auto pipeline_desc = RHIGraphicsPipelineDesc{
                    .stages = {
                            .vertex_shader = v_shader.Raw(),
                            .fragment_shader = f_shader.Raw()
                    },
                    .vertex_input = {
                            .vertex_buffers = {binding_descs.begin(), binding_descs.end()},
                            .vertex_attributes = {vertex_attribute_descs.begin(), vertex_attribute_descs.end()}
                    },
                    .topology = RHIPrimitiveTopologyType::kTriangleList,
                    .depth_stencil = {
                            false,
                            true,
                            RHIDepthCompareOpType::kLess
                    },
                    .color_attachments = {
                            attachment_descs.begin(), attachment_descs.end()
                    },
                    .depth_stencil_attachment = {
                            .format = PixelFormatType::kD32_FLOAT
                    },
            };
            auto pipeline = RHI::Get().CreateGraphicsPipeline(pipeline_desc);
            EXPECT_TRUE(pipeline);
            EXPECT_TRUE(pipeline->IsValid());

            // Bindless textures
            auto bindless_texture_0_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();
            auto bindless_texture_1_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();

            TRef<RHITexture> bindless_texture_0;
            {
                const std::string filename = "rhi/resources/test1.png";
                int width, height, channels;
                auto data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
                EXPECT_TRUE(data);
                auto texture = RHI::Get().CreateTexture(
                        RHITextureType::k2D, RHITextureDimensions{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                        PixelFormatType::kR8G8B8A8_SRGB,
                        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
                );
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                    texture.Raw(), RHITextureLayoutType::kTransferDstOptimal,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
                );
                EXPECT_TRUE(texture);
                auto staging_buf = RHI::Get().CreateBuffer(
                        width * height * 4,
                        RHIBufferUsageFlagBits::kStaging
                );
                memcpy(staging_buf->Map(), data, width * height * 4);
                RHI::Get().GetGraphicsCommandQueue().CopyBufferToTexture(staging_buf->GetSpan(), texture.Raw());
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIGPUAccessFlagBits::kWrite,
                        RHIGPUAccessFlagBits::kRead
                );
                RHI::Get().GetGraphicsCommandQueue().WaitForIdle();
                bindless_texture_0 = texture;
                bindless_texture_0_slot->SetAndCommit(texture.Raw());
                stbi_image_free(data);
            }
            TRef<RHITexture> bindless_texture_1;
            {
                const std::string filename = "rhi/resources/test2.png";
                int width, height, channels;
                auto data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
                EXPECT_TRUE(data);
                auto texture = RHI::Get().CreateTexture(
                        RHITextureType::k2D, RHITextureDimensions{static_cast<uint32_t>(width), static_cast<uint32_t>(height)},
                        PixelFormatType::kR8G8B8A8_SRGB,
                        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
                );
                EXPECT_TRUE(texture);
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kTransferDstOptimal,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIGPUAccessFlagBits::kNone,
                        RHIGPUAccessFlagBits::kWrite
                );
                auto staging_buf = RHI::Get().CreateBuffer(
                        width * height * 4,
                        RHIBufferUsageFlagBits::kStaging
                );
                memcpy(staging_buf->Map(), data, width * height * 4);
                RHI::Get().GetGraphicsCommandQueue().CopyBufferToTexture(staging_buf->GetSpan(), texture.Raw());
                RHI::Get().GetGraphicsCommandQueue().TextureBarrier(
                        texture.Raw(), RHITextureLayoutType::kShaderReadOnlyOptimal,
                        RHIPipelineStageFlagBits::kTransfer,
                        RHIGPUAccessFlagBits::kWrite,
                        RHIGPUAccessFlagBits::kRead
                );
                RHI::Get().GetGraphicsCommandQueue().WaitForIdle();
                bindless_texture_1 = texture;
                bindless_texture_1_slot->SetAndCommit(texture.Raw());
                stbi_image_free(data);
            }

            // Render texture
            auto texture0 = RHI::Get().CreateTexture(
                    RHITextureType::k2D, RHITextureDimensions{1280, 720}, PixelFormatType::kR8G8B8A8_SRGB,
                    RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer
            );
            EXPECT_TRUE(texture0);

            auto uniform_buf = RHI::Get().CreateBuffer(
                    256,
                    RHIBufferUsageFlagBits::kUniform
            );

            auto &queue = RHI::Get().GetGraphicsCommandQueue();
            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kTransferDstOptimal,
                    // RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
            );
            queue.ClearTexture(texture0.Raw(), {0.f, 0.f, 0.f, 0.f});
            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kColorAttachment,
                    // RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRW
            );
            queue.BindPipeline(pipeline.Raw());
            RHIDrawDesc ds {};
            ds.SetAttachment(0, texture0.Raw());
            ds.SetClearValue(0, {0.f, 1.f, 0.f, 1.f});

            queue.UpdateDrawState(ds);

            auto vtx_buf = RHI::Get().CreateBuffer(
                    3 * sizeof(float) * 5,
                    RHIBufferUsageFlagBits::kVertex | RHIBufferUsageFlagBits::kTransferSrc
            );
            auto staging_buf = RHI::Get().CreateBuffer(
                    1024 * 1024 * 32,
                    RHIBufferUsageFlagBits::kStaging
            );
            float vbuf_host[] = {
                    -0.5f, -0.5f, 0.1f, 0.f, 0.f,
                     0.5f, -0.5f, 0.1f, 0.f, 1.f,
                     0.f,   0.5f, 0.1f, 1.f, 0.f
            };
            memcpy(staging_buf->Map(), vbuf_host, 3 * sizeof(float) * 5);
            queue.CopyBuffer(staging_buf->GetSpan(), vtx_buf->GetSpan());
            queue.BufferBarrier(vtx_buf->GetSpan(),
                                RHIPipelineStageFlagBits::kOrdinaryGraphics,
                                RHIGPUAccessFlagBits::kWrite,
                                RHIGPUAccessFlagBits::kRead);
            uint32_t texid = bindless_texture_1_slot->GetSlot();
            memcpy(((char*)staging_buf->Map()) + 1024, &texid, sizeof(texid));
            queue.CopyBuffer(staging_buf->GetSpan(1024, 16), uniform_buf->GetSpan(0, 16));
            queue.BufferBarrier(uniform_buf->GetSpan(),
                                RHIPipelineStageFlagBits::kAll,
                                RHIGPUAccessFlagBits::kWrite,
                                RHIGPUAccessFlagBits::kRead);
            queue.BindVertexBuffer(0, vtx_buf->GetSpan());
            auto params = RHIBindPipelineParametersDesc{};
            auto ubs = queue.Allocate<RHIPipelineParameterBufferDesc[]>(1);
            ubs[0].buffer  = uniform_buf->GetSpan();
            auto ub_binding = pipeline->ReflectResourceSlot("$Globals");
            ubs[0].slot = ub_binding.slot_index;
            params.uniforms = {ubs, 1};
            auto linear_wrap = RHI::Get().GetGlobalSamplers().linear_wrap;
            auto samplers = queue.Allocate<RHIPipelineParameterResourceDesc[]>(1);
            samplers[0].resource = linear_wrap;
            auto sampler_binding = pipeline->ReflectResourceSlot("sampler_linear");
            samplers[0].slot = sampler_binding.slot_index;
            params.samplers = {samplers, 1};
            queue.BindPipelineParameters(RHIBindPointType::kGraphics, params);
            queue.BeginRendering();
            queue.DrawPrimitive(3, 1);
            queue.EndRendering();

            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kTransferSrcOptimal,
                    // RHIPipelineStageFlagBits::kOrdinaryGraphics | RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRead
            );
            queue.CopyTextureToBuffer(texture0.Raw(), staging_buf.Raw());
            auto sync = RHI::Get().CreateSyncPoint();
            queue.EnqueueTranslateAndSubmit(sync.Raw());
            sync->Wait();

            RHI::Get().AdvanceFrame();
            // Convert to bitmap
            auto u8_tex = (uint8_t *) staging_buf->Map();
            auto bitmap = new uint32_t[1280 * 720];
            for (int i = 0; i < 1280 * 720; ++i) {
                auto r = static_cast<uint8_t>(u8_tex[i * 4 + 0]);
                auto g = static_cast<uint8_t>(u8_tex[i * 4 + 1]);
                auto b = static_cast<uint8_t>(u8_tex[i * 4 + 2]);
                auto a = static_cast<uint8_t>(u8_tex[i * 4 + 3]);
                // bgra
                bitmap[i] = (a << 24) | (b << 16) | (g << 8) | r;
            }
            // Save to file
            stbi_write_png("rhi_test_bindless.png", 1280, 720, 4, bitmap, 1280 * 4);
        }
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}