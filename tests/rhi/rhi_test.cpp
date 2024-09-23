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

TEST(RHITest, RHIThreadTasks) {
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

static auto v_shader_code = "// Vertex Shader\n"
                     "struct VSInput {\n"
                     "    float3 position : POSITION;\n"
                     "    float2 color : COLOR;\n"
                     "};\n"
                     "\n"
                     "struct VSOutput {\n"
                     "    float4 position : SV_POSITION;\n"
                     "    float3 color : COLOR;\n"
                     "};\n"
                     "\n"
                     "VSOutput Main(VSInput input) {\n"
                     "    VSOutput output;\n"
                     "    output.position = float4(input.position, 1.0);\n"
                     "    output.color = float3(input.color, 1);\n"
                     "    return output;\n"
                     "}";
static auto f_shader_code = "// Fragment Shader\n"
                     "struct PSInput {\n"
                     "    float3 color : COLOR;\n"
                     "};\n"
                     "struct PSOutput {"
                     "    float4 color0 : SV_Target0;\n"
                     "    int4   color1 : SV_Target1;\n"
                     "};\n"
                     "PSOutput Main(PSInput input) {\n"
                     "    PSOutput output;\n"
                     "    output.color0 = float4(input.color, 1.0);\n"
                     "    output.color1 = int4(input.color * 2, 0.5);\n"
                     "    return output;\n"
                     "}";

TEST(RHITest, RHIPipelineAssemble) {
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
                        .blending = RHIColorAttachmentBlendDesc {
                                true,
                                RHIBlendFactorType::kSrcAlpha,
                                RHIBlendFactorType::kOneMinusSrcAlpha,
                                RHIBlendFactorType::kOne,
                                RHIBlendFactorType::kOne,
                                static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd),
                                static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd)
                        },
                        .format = PixelFormatType::kR16G16B16A16_FLOAT,
                },
                RHIColorAttachmentDesc{
                        .blending = RHIColorAttachmentBlendDesc {
                                true,
                                RHIBlendFactorType::kSrcAlpha,
                                RHIBlendFactorType::kOneMinusSrcAlpha,
                                RHIBlendFactorType::kOne,
                                RHIBlendFactorType::kOne,
                                static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd),
                                static_cast<RHIBlendOpType>(RHIBlendOpType::kBlendAdd)
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
                        true,
                        true,
                        RHIDepthCompareOpType::kLess
                },
                .color_attachments = {
                        attachment_descs.begin(), attachment_descs.end()
                },
                .depth_stencil_attachment = {
                        .format = PixelFormatType::kD32_FLOAT,
                        .load_op = RHILoadOpType::kClear,
                        .store_op = RHIStoreOpType::kStore,
                },
        };
        auto pipeline = RHI::Get().CreateGraphicsPipeline(pipeline_desc);
        EXPECT_TRUE(pipeline);
        EXPECT_TRUE(pipeline->IsValid());

        // Render texture
        auto texture0 = RHI::Get().CreateTexture(
                RHITextureType::k2D, RHITextureDimensions {1280, 720}, PixelFormatType::kR16G16B16A16_FLOAT,
                RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferSrc
        );
        auto texture1 = RHI::Get().CreateTexture(
                RHITextureType::k2D, RHITextureDimensions {1280, 720}, PixelFormatType::kR32G32B32A32_UINT,
                RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferSrc
        );
        EXPECT_TRUE(texture0);
        EXPECT_TRUE(texture1);
        auto & queue = RHI::Get().GetGraphicsCommandQueue();
        queue.ClearTexture(texture0.Raw(), {0.f, 0.f, 0.f, 0.f});
        queue.BindPipeline(pipeline.Raw());
    }
    RHI::DestroySingleton();
    GetInfra().Shutdown();
    DestroyInfra();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}