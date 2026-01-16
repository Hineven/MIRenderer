/*
 * Created: 2024/9/16
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

#include <exception>
#include <cpptrace/from_current.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <rhi/rhi_buffer.h>

#include "stb_image.h"
#include "stb_image_write.h"

TEST(RHITest, RHIStart) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();

    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);

    RHI::InitializeSingleton(RHIType::kVulkan);
    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}

TEST(RHITest, RHIShaderCompile) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);
    RHI::InitializeSingleton(RHIType::kVulkan);
    // Sim render thread scope
    {
        std::vector<std::string> options;
        options.push_back("-fspv-target-env=vulkan1.3");
        options.push_back("-fvk-use-scalar-layout");
        options.push_back("-Zi");

        auto shader_code = "RWTexture2D<float4> Tex; [numthreads(1, 1, 1)] void Main() {Tex[int2(0, 0)] = 0.f.xxxx;}";
        auto shader_code_span = std::span(reinterpret_cast<const char *>(shader_code), strlen(shader_code));
        std::string errmsg;
        auto shader_bytecode = GetInfra().CompileHLSLToSPIRV(L"", "Main", "cs_6_3", shader_code_span, options, errmsg);
        auto bytecode_span = std::span(reinterpret_cast<const std::byte *>(shader_bytecode.data()),
                                       shader_bytecode.size() * sizeof(uint32_t));
        if (shader_bytecode.empty()) {
            MI_LOG(MIInfraLogType::kError, "Failed to compile shader: {}", errmsg);
        }
        EXPECT_TRUE(shader_bytecode.size() > 0);
        auto rhi_shader = RHI::Get().CreateShader(
                RHIShaderFrequencyFlagBits::kCompute, "Main",
                RHIShaderIRType::kSPIRV, bytecode_span
        );
        EXPECT_TRUE(rhi_shader);
        auto uav_desc = rhi_shader->GetUAVDesc();
        EXPECT_TRUE(uav_desc.size() == 1);
        EXPECT_TRUE(uav_desc[0].name == "Tex");
    }
    RHI::DestroySingleton();
    GetInfra().Shutdown();
    DestroyInfra();
}
// Disable waring C4602 temporarily
#pragma warning (push)
#pragma warning (disable: 4602)
TEST(RHITest, RHIThreadTasks) {

    if constexpr (MI_BYPASS_RHI_THREAD) {
        return ; // If we're bypassing the RHI thread, we don't need to run this test
    }

    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);
    RHI::InitializeSingleton(RHIType::kVulkan);
    volatile static bool flag;
    {
        volatile static int value;
        EnqueueRHIThreadTask([]() {
            MI_LOG(MIInfraLogType::kInfo, "Task 1");
            value = 123;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds (100));
        EXPECT_TRUE(value == 123);
        auto fut = EnqueueRHIThreadTask([]() {
            MI_LOG(MIInfraLogType::kInfo, "Task 2");
        });
        EnqueueRHIThreadTask([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds (200));
            flag = true;
            MI_LOG(MIInfraLogType::kInfo, "Task 3");
        });
        fut.wait();
        EXPECT_FALSE(flag);
        EnqueueRHIThreadTask([]() {
            MI_LOG(MIInfraLogType::kInfo, "Task 4");
        });
    }
    RHI::DestroySingleton();
    EXPECT_TRUE(flag);
    GetInfra().Shutdown();
    DestroyInfra();

}
#pragma warning (pop)

static auto v_shader_code = "// Vertex Shader\n"
                     "RWStructuredBuffer<uint> someBuffer;"
                     "struct VSInput {\n"
                     "    float3 position : POSITION;\n"
                     "    float2 color : COLOR;\n"
                     "};\n"
                     "struct VSOutput {\n"
                     "    float4 position : SV_POSITION;\n"
                     "    float3 color : COLOR;\n"
                     "};\n"
                     "\n"
                     "VSOutput Main(VSInput input, in uint vid : SV_VertexID) {\n"
                     "    VSOutput output;\n"
                     "    output.position = float4(input.position, 1.0);"
                     "    if(vid > 0) someBuffer[vid] = vid + 123;\n"
                     "    output.color = float3(min(someBuffer[0], 1) ? 1 : 0, input.color);\n"
                     "    return output;\n"
                     "}";
static auto f_shader_code = "// Fragment Shader\n"
                     "struct PSInput {\n"
                     "    float3 color : COLOR;\n"
                     "};\n"
                     "struct PSOutput {"
                     "    float4 color0 : SV_Target0;\n"
                     "    uint4  color1 : SV_Target1;\n"
                     "};\n"
                     "PSOutput Main(PSInput input) {\n"
                     "    PSOutput output;\n"
                     "    output.color0 = float4(input.color, 1.0);\n"
                     "    output.color1 = uint4(input.color * 100, 1);\n"
                     "    return output;\n"
                     "}";

TEST(RHITest, RHITriangle) {
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
            options.push_back("-fvk-use-scalar-layout");
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
            EXPECT_TRUE(vinputs[1].name == "COLOR");
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
            EXPECT_TRUE(foutputs.size() == 2);
            EXPECT_TRUE(foutputs[0].name == "SV_Target0");
            EXPECT_TRUE(foutputs[0].format == RHIFragmentOutputFormatType::k4xFp32);
            EXPECT_TRUE(foutputs[1].name == "SV_Target1");
            EXPECT_TRUE(foutputs[1].format == RHIFragmentOutputFormatType::k4xUIint32);

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
                            .format = PixelFormatType::kR16G16B16A16_FLOAT,
                    },
                    RHIColorAttachmentDesc{
                            .blending = RHIColorAttachmentBlendDesc{
                                    false
                            },
                            .format = PixelFormatType::kR32G32B32A32_UINT,
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

            // Render texture
            auto texture0 = RHI::Get().CreateTexture(
                    RHITextureType::k2D, RHITextureDimensions{1280, 720}, PixelFormatType::kR16G16B16A16_FLOAT,
                    RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer
            );
            auto texture1 = RHI::Get().CreateTexture(
                    RHITextureType::k2D, RHITextureDimensions{1280, 720}, PixelFormatType::kR32G32B32A32_UINT,
                    RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer
            );
            auto depth = RHI::Get().CreateTexture(
                    RHITextureType::k2D, RHITextureDimensions{1280, 720}, PixelFormatType::kD32_FLOAT,
                    RHITextureUsageFlagBits::kDepthStencil
            );
            EXPECT_TRUE(texture0);
            EXPECT_TRUE(texture1);

            auto storage_buf = RHI::Get().CreateBuffer(
                    128 * 4,
                    RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kReadback
            );
            ((uint32_t*)storage_buf->Map())[0] = 123;

            auto &queue = RHI::Get().GetGraphicsCommandQueue();
            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kTransferDstOptimal,
                    RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
            );
            queue.TextureBarrier(
                    texture1.Raw(),
                    RHITextureLayoutType::kTransferDstOptimal,
                    RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kWrite
            );
            queue.TextureBarrier(
                    depth.Raw(),
                    RHITextureLayoutType::kDepthStencilAttachment,
                    RHIPipelineStageFlagBits::kNone,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics,
                    RHIGPUAccessFlagBits::kNone,
                    RHIGPUAccessFlagBits::kRW
            );
            queue.ClearTexture(texture0.Raw(), {0.f, 0.f, 0.f, 0.f});
            queue.ClearTexture(texture1.Raw(), {0.f, 0.f, 0.f, 0.f});
            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kColorAttachment,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRW
            );
            queue.TextureBarrier(
                    texture1.Raw(),
                    RHITextureLayoutType::kColorAttachment,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRW
            );
            queue.BindPipeline(pipeline.Raw());
            RHIDrawStateDesc ds {};
            ds.SetAttachment(0, texture0.Raw());
            ds.SetAttachment(1, texture1.Raw());
//            ds.SetAttachment(2, depth.Raw());
//            ds.SetClearValue(2, {0.f});
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
                                RHIPipelineStageFlagBits::kTransfer,
                                RHIPipelineStageFlagBits::kOrdinaryGraphics,
                                RHIGPUAccessFlagBits::kWrite,
                                RHIGPUAccessFlagBits::kRead);
            queue.BufferBarrier(storage_buf->GetSpan(),
                                RHIPipelineStageFlagBits::kAll,
                                RHIPipelineStageFlagBits::kAll,
                                RHIGPUAccessFlagBits::kNone,
                                RHIGPUAccessFlagBits::kRW);
            queue.BindVertexBuffer(0, vtx_buf->GetSpan());
            auto params = RHIBindPipelineParametersDesc{};
            auto storages = queue.Allocate<RHIPipelineParameterBufferDesc[]>(1);
            storages[0].buffer  = storage_buf->GetSpan();
            auto storage_binding = pipeline->ReflectResourceSlot("someBuffer");
            storages[0].slot = storage_binding.slot_index;
            params.storages = {storages, 1};
            queue.BindPipelineParameters(RHIBindPointType::kGraphics, params);
            queue.BeginRendering();
            queue.Draw(3, 1);
            queue.EndRendering();

            queue.TextureBarrier(
                    texture0.Raw(),
                    RHITextureLayoutType::kTransferSrcOptimal,
                    RHIPipelineStageFlagBits::kOrdinaryGraphics | RHIPipelineStageFlagBits::kTransfer,
                    RHIPipelineStageFlagBits::kTransfer,
                    RHIGPUAccessFlagBits::kWrite,
                    RHIGPUAccessFlagBits::kRead
            );
            queue.BufferBarrier(
                staging_buf->GetSpan(),
                RHIPipelineStageFlagBits::kAll,
                RHIPipelineStageFlagBits::kAll,
                RHIGPUAccessFlagBits::kRW,
                RHIGPUAccessFlagBits::kRW
            );
            queue.CopyTextureToBuffer(texture0.Raw(), staging_buf.Raw());
            auto sync = RHI::Get().CreateSyncPoint();
            queue.EnqueueTranslateAndSubmit(sync.Raw());
            sync->Wait();

            // Validate that we have received a correct storage buffer
            for (int i = 0; i < 3; ++i) {
                EXPECT_EQ((uint32_t)(i + 123), ((uint32_t*)storage_buf->Map())[i]);
            }


            RHI::Get().AdvanceFrame();
            // Convert to bitmap
            auto fp16tex = (uint16_t *) staging_buf->Map();
            auto bitmap = new uint32_t[1280 * 720];
            auto fp16tofp32 = [](uint16_t v) -> float {
                float sgn = (v & 0x8000) ? -1.f : 1.f;
                int exp = (v & 0x7C00) >> 10;
                int mant = v & 0x03FF;
                if (exp == 0) {
                    if (mant == 0) return 0.f;
                    return sgn * float(std::pow(2, -14)) * float(mant / 1024.f);
                }
                if (exp == 31) {
                    if (mant == 0) return sgn * std::numeric_limits<float>::infinity();
                    return std::numeric_limits<float>::quiet_NaN();
                }
                return sgn * float(std::pow(2, exp - 15)) * float((1 + mant / 1024.f));
            };
            for (int i = 0; i < 1280 * 720; ++i) {
                auto r = static_cast<uint8_t>(fp16tofp32(fp16tex[i * 4 + 0]) * 255);
                auto g = static_cast<uint8_t>(fp16tofp32(fp16tex[i * 4 + 1]) * 255);
                auto b = static_cast<uint8_t>(fp16tofp32(fp16tex[i * 4 + 2]) * 255);
                auto a = static_cast<uint8_t>(fp16tofp32(fp16tex[i * 4 + 3]) * 255);
                // bgra
                bitmap[i] = (a << 24) | (b << 16) | (g << 8) | r;
            }
            // Save to file
            stbi_write_png("rhi_test.png", 1280, 720, 4, bitmap, 1280 * 4);
        }
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}

TEST(PixelFormatTest, FormatFunctions) {
    using namespace mi;
    
    // 测试正常输入的情况
    // GetPixelFormatDataType
    EXPECT_EQ(PixelFormatDataType::kUNORM, GetPixelFormatDataType(PixelFormatType::kR8_UNORM));
    EXPECT_EQ(PixelFormatDataType::kSRGB, GetPixelFormatDataType(PixelFormatType::kR8G8B8A8_SRGB));
    EXPECT_EQ(PixelFormatDataType::kFLOAT, GetPixelFormatDataType(PixelFormatType::kR32_FLOAT));
    EXPECT_EQ(PixelFormatDataType::kUINT, GetPixelFormatDataType(PixelFormatType::kR32_UINT));
    
    // GetPixelFormatNumBytesPerChannel
    EXPECT_EQ(1u, GetPixelFormatNumBytesPerChannel(PixelFormatType::kR8_UNORM));
    EXPECT_EQ(2u, GetPixelFormatNumBytesPerChannel(PixelFormatType::kR16G16_FLOAT));
    EXPECT_EQ(4u, GetPixelFormatNumBytesPerChannel(PixelFormatType::kR32_FLOAT));
    
    // GetPixelFormatNumChannels
    EXPECT_EQ(1u, GetPixelFormatNumChannels(PixelFormatType::kR8_UNORM));
    EXPECT_EQ(2u, GetPixelFormatNumChannels(PixelFormatType::kR8G8_UNORM));
    EXPECT_EQ(3u, GetPixelFormatNumChannels(PixelFormatType::kR32G32B32_FLOAT));
    EXPECT_EQ(4u, GetPixelFormatNumChannels(PixelFormatType::kR8G8B8A8_UNORM));
    
    // IsDepthStencilPixelFormat
    EXPECT_TRUE(IsDepthStencilPixelFormat(PixelFormatType::kD32_FLOAT));
    EXPECT_FALSE(IsDepthStencilPixelFormat(PixelFormatType::kR8G8B8A8_UNORM));
    
    // GetPixelFormatName
    EXPECT_STREQ("R8_UNORM", GetPixelFormatName(PixelFormatType::kR8_UNORM));
    EXPECT_STREQ("R32G32B32A32_FLOAT", GetPixelFormatName(PixelFormatType::kR32G32B32A32_FLOAT));
    
    // ToString
    EXPECT_EQ(std::string("R16G16B16A16_FLOAT"), ToString(PixelFormatType::kR16G16B16A16_FLOAT));
    
    // IsFloatPixelFormat
    EXPECT_TRUE(IsFloatPixelFormat(PixelFormatType::kR32_FLOAT));
    EXPECT_TRUE(IsFloatPixelFormat(PixelFormatType::kR8G8B8A8_UNORM)); // UNORM也被视为float
    EXPECT_FALSE(IsFloatPixelFormat(PixelFormatType::kR32_UINT));
    
    // IsUIntPixelFormat
    EXPECT_TRUE(IsUIntPixelFormat(PixelFormatType::kR32_UINT));
    EXPECT_FALSE(IsUIntPixelFormat(PixelFormatType::kR32_FLOAT));
    
    // IsPixelFormat4ComponentFloat
    EXPECT_TRUE(IsPixelFormat4ComponentFloat(PixelFormatType::kR32G32B32A32_FLOAT));
    EXPECT_TRUE(IsPixelFormat4ComponentFloat(PixelFormatType::kR8G8B8A8_UNORM));
    EXPECT_FALSE(IsPixelFormat4ComponentFloat(PixelFormatType::kR8G8_UNORM)); // 只有2个组件
    EXPECT_FALSE(IsPixelFormat4ComponentFloat(PixelFormatType::kR32G32B32A32_UINT)); // 不是float类型
    
    // GetPixelFormatBytesPerPixel
    EXPECT_EQ(1u, GetPixelFormatBytesPerPixel(PixelFormatType::kR8_UNORM)); // 1通道 × 1字节
    EXPECT_EQ(4u, GetPixelFormatBytesPerPixel(PixelFormatType::kR8G8B8A8_UNORM)); // 4通道 × 1字节
    EXPECT_EQ(8u, GetPixelFormatBytesPerPixel(PixelFormatType::kR16G16B16A16_FLOAT)); // 4通道 × 2字节
    EXPECT_EQ(16u, GetPixelFormatBytesPerPixel(PixelFormatType::kR32G32B32A32_FLOAT)); // 4通道 × 4字节
    
    // 测试异常情况下的断言（这些测试会触发断言，仅在调试模式下有效）
#ifndef NDEBUG
    // 如果在调试模式下，可以测试断言是否会触发
    // 注意：这些测试会导致程序终止，所以需要在条件编译下执行
    
    // 以下代码仅作示例，实际上会导致程序崩溃，因此被注释掉
    // 要测试断言，可以使用Google Test的EXPECT_DEATH或类似宏
    
    // 测试无效的PixelFormatType输入
    EXPECT_DEATH({
        GetPixelFormatDataType(static_cast<PixelFormatType>(999));
    }, "");
    
    EXPECT_DEATH({
        GetPixelFormatNumBytesPerChannel(static_cast<PixelFormatType>(999));
    }, "");
    
    EXPECT_DEATH({
        GetPixelFormatNumChannels(static_cast<PixelFormatType>(999));
    }, "");
    
    EXPECT_DEATH({
        GetPixelFormatName(static_cast<PixelFormatType>(999));
    }, "");
#endif
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
