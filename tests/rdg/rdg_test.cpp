/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <cpptrace/from_current.hpp>
#include <gtest/gtest.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_pool.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_pipeline.h>

#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"
#include "rdg/rdg_shader.h"
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN

struct UB1 {
    int TestInteger1;
    int Padding0;
    int Padding1;
    int Padding2;
};

struct UB2 {
    glm::vec3 TestFloat3;
    uint32_t TestInteger1;
};

BEGIN_SHADER_PARAMETERS(TestParamInnerStruct)
    SHADER_UNIFORM_BUFFER(UB1, TestInteger1)
    SHADER_RESOURCE_PARAMETER(Texture2D, texture1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestParamsInnerStructRef)
    SHADER_UNIFORM_BUFFER(UB1, TestInteger1)
    SHADER_UNIFORM_BUFFER(UB2, TestFloat3AndInteger1)
    SHADER_RESOURCE_PARAMETER(Texture2D, texture2)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

TEST(RDGTest, RDGShaderParams) {
    using namespace mi;
    CPPTRACE_TRY {
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();

        auto meta_test_inner = *TestParamInnerStruct::GetParamStructInfo();
        EXPECT_EQ(meta_test_inner.cpp_members.size(), 2);
        EXPECT_EQ(meta_test_inner.cpp_members[0].name, "TestInteger1");
        EXPECT_EQ(meta_test_inner.cpp_members[1].name, "texture1");
        EXPECT_EQ(meta_test_inner.cpp_members[0].cpp_offset, offsetof(TestParamInnerStruct, TestInteger1));
        EXPECT_EQ(meta_test_inner.cpp_members[1].cpp_offset, offsetof(TestParamInnerStruct, texture1));

        auto meta_test_inner2 = *TestParamsInnerStructRef::GetParamStructInfo();
        EXPECT_EQ(meta_test_inner2.cpp_members.size(), 3);
        EXPECT_EQ(meta_test_inner2.cpp_members[0].name, "TestInteger1");
        EXPECT_EQ(meta_test_inner2.cpp_members[1].name, "TestFloat3AndInteger1");
        EXPECT_EQ(meta_test_inner2.cpp_members[2].name, "texture2");
        EXPECT_EQ(meta_test_inner2.cpp_members[0].cpp_offset, offsetof(TestParamsInnerStructRef, TestInteger1));
        EXPECT_EQ(meta_test_inner2.cpp_members[1].cpp_offset, offsetof(TestParamsInnerStructRef, TestFloat3AndInteger1));
        EXPECT_EQ(meta_test_inner2.cpp_members[2].cpp_offset, offsetof(TestParamsInnerStructRef, texture2));

        // Query members
        EXPECT_EQ(meta_test_inner.GetCppMemberIndex("TestInteger1"), 0);
        EXPECT_EQ(meta_test_inner.GetCppMemberIndex("texture1"), 1);
        EXPECT_EQ(meta_test_inner2.GetCppMemberIndex("TestInteger1"), 0);
        EXPECT_EQ(meta_test_inner2.GetCppMemberIndex("TestFloat3AndInteger1"), 1);
        EXPECT_EQ(meta_test_inner2.GetCppMemberIndex("texture2"), 2);

        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}

using namespace mi;
class TestShader1 : public RDGShader {
public:
    DECLARE_SHADER()
    struct TestShader1UB {
        glm::vec4 TestFloat4;
        glm::vec2 TestFloat2;
        glm::vec2 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(TestShader1UB, UB)
        SHADER_RESOURCE_PARAMETER(RWStructuredBuffer, TestBuffer)
        SHADER_RESOURCE_PARAMETER(RWTexture2D, TestTexture)
        SHADER_DISPATCH_COMMAND(command)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    static std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

IMPLEMENT_RDG_COMPUTE_SHADER(TestShader1, "test_shader_1.hlsl", "TestComputeShaderMain")


TEST(RDGTest, RDGShaderLibrary) {
    using namespace mi;
    CPPTRACE_TRY {
        auto pwd = std::filesystem::current_path();
        auto resource_dir = pwd / "rdg" / "resources";
        TransferInfra(std::make_unique<MyInfra>(false, resource_dir.string()));
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            auto & lib = RDGShaderLibrary::Get();
            lib.Init();
            auto shader = lib.GetShader<TestShader1>();
            EXPECT_TRUE(shader->IsValid());
            lib.ReleaseCompiledShaders();
        }

        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}

TEST(RDGTest, RDGSimpleComputeShader) {
    using namespace mi;
    CPPTRACE_TRY {
        auto pwd = std::filesystem::current_path();
        auto resource_dir = pwd / "rdg" / "resources";
        TransferInfra(std::make_unique<MyInfra>(false, resource_dir.string()));
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            auto & lib = RDGShaderLibrary::Get();
            lib.Init();
            auto shader = lib.GetShader<TestShader1>();
            EXPECT_TRUE(shader->IsValid());
            RenderGraphBuilder builder;
            auto params = builder.Allocate<TestShader1::Parameters>();
            params->UB = builder.Allocate<TestShader1::TestShader1UB>();
            params->UB->TestFloat2 = {0.1f, 0.2f};
            params->UB->TestFloat4 = {0.3f, 0.4f, 0.5f, 0.6f};
            auto storage_buffer_ref = RDGBuffer::Create(RHIBufferUsageFlagBits::kStorage, 1024);
            params->TestBuffer = storage_buffer_ref.Raw();
            auto test_texture = builder.CreateTexture2D(
                128, 128, PixelFormatType::kR32G32B32A32_FLOAT,
                RHITextureUsageFlagBits::kTransferSrc | RHITextureUsageFlagBits::kUnorderedAccess
            );
            params->TestTexture = test_texture.Raw();
            builder.AddPass("SimpleShader", RDGPassType::kCompute, {},
                TestShader1::GetShaderParamStructInfo(), params,
                [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
                    RDGCommandHelper::Dispatch<TestShader1>(queue, pass, shader, params);
                });
            auto out_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kReadback, 1024 * 1024 * 16);
            out_buffer->SetExport();
            builder.AddPass({},
                [ttex = test_texture.Raw(), obuf = out_buffer.Raw(), stor = storage_buffer_ref.Raw()]
                ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                queue.CopyTextureToBuffer(ttex->GetRHI(), obuf->GetRHI().buffer, 0, 0, 0, 1, 0, 0,
                    0, 0, 0, 2, 2, 1);
                auto src_span = stor->GetRHI();
                src_span.size = 64;
                auto dst_span = obuf->GetRHI();
                dst_span.offset = 128;
                dst_span.size = 64;
                queue.CopyBuffer(src_span, dst_span);
            })->AddTexture(test_texture.Raw(), RDGTextureUsageType::kTransferSrc)
              ->AddBuffer(out_buffer.Raw(), RHIGPUAccessFlagBits::kWrite)
              ->AddBuffer(storage_buffer_ref.Raw(), RHIGPUAccessFlagBits::kRead);
            auto rdg = builder.Compile();
            auto pool = RDGResourcePool::Create();
            rdg->Execute(pool.Raw());
            RHI::Get().WaitForIdle();

            auto out_buffer_span = out_buffer->GetRHI();
            auto out_ptr = (std::byte*)out_buffer_span.buffer->Map() + out_buffer_span.offset;
            auto out_float_array = (float*)out_ptr;

            // First pixel: TestFloat4
            EXPECT_EQ(out_float_array[0], 0.3f);
            EXPECT_EQ(out_float_array[1], 0.4f);
            EXPECT_EQ(out_float_array[2], 0.5f);
            EXPECT_EQ(out_float_array[3], 0.6f);
            // Second pixel: TestFloat2 and 2 ones
            EXPECT_EQ(out_float_array[8], 0.1f);
            EXPECT_EQ(out_float_array[9], 0.2f);
            EXPECT_EQ(out_float_array[10], 1.0f);
            EXPECT_EQ(out_float_array[11], 1.0f);

            // Test storage buffer
            EXPECT_EQ(out_float_array[32], 123.0f);
            EXPECT_EQ(out_float_array[33], 0.0f);
            EXPECT_EQ(out_float_array[34], 111.0f);

            EXPECT_EQ(out_float_array[36], 0.3f);
            EXPECT_EQ(out_float_array[37], 0.4f);
            EXPECT_EQ(out_float_array[38], 0.5f);
            EXPECT_EQ(out_float_array[39], 0.6f);

            lib.ReleaseCompiledShaders();
        }

        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}

using namespace mi;
class TestShader2 : public RDGShader {
public:
    DECLARE_SHADER()
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(TestShader1::TestShader1UB, UB)
        SHADER_VERTEX_BUFFER(12 + 8, vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k3xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 12, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_RENDER_TARGET(PixelFormatType::kR32G32B32A32_FLOAT, OutColor)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    static std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

IMPLEMENT_RDG_GRAPHICS_SHADER(TestShader2, "test_shader_1.hlsl", "TestGraphicsShaderVS", "TestGraphicsShaderPS")

TEST(RDGTest, RDGSimpleGraphicsShader) {
    using namespace mi;
    CPPTRACE_TRY {
        auto pwd = std::filesystem::current_path();
        auto resource_dir = pwd / "rdg" / "resources";
        TransferInfra(std::make_unique<MyInfra>(false, resource_dir.string()));
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            auto & lib = RDGShaderLibrary::Get();
            lib.Init();
            auto shader = lib.GetShader<TestShader2>();
            EXPECT_TRUE(shader->IsValid());
            RenderGraphBuilder builder;
            auto params = builder.Allocate<TestShader2::Parameters>();
            // auto pass = builder.Allocate<TestShaderRenderPass>();
            params->UB = builder.Allocate<TestShader1::TestShader1UB>();
            params->UB->TestFloat4 = {0.3f, 0.4f, 0.5f, 0.6f};
            params->UB->TestFloat2 = {0.7f, 1.f};
            auto test_texture = builder.CreateTexture2D(
                128, 128, PixelFormatType::kR32G32B32A32_FLOAT,
                RHITextureUsageFlagBits::kTransferSrc | RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kRenderTarget
            );
            auto vertex_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kVertex, 1024);
            params->vertex_buffer = vertex_buffer.Raw();
            // Simply allocate a new staging buffer. Let RHI handle its lifetime.
            auto staging_buffer = RHI::Get().CreateBuffer(1024, RHIBufferUsageFlagBits::kStaging);
            auto ptr = staging_buffer->Map();
            float vbuf_host[] = {
                -0.5f, -0.5f, 0.1f, 0.f, 0.f,
                 0.5f, -0.5f, 0.1f, 0.f, 1.f,
                 0.f,   0.5f, 0.1f, 1.f, 0.f
            };
            memcpy(ptr, vbuf_host, 3 * sizeof(float) * 5);
            params->OutColor.clear_value = {1.f, 0.f, 0.f, 1.f};
            params->OutColor.load_op = RHILoadOpType::kClear;
            params->OutColor = test_texture.Raw();
            // Upload
            builder.AddPass({},
                [staging = staging_buffer.Raw(), vb = vertex_buffer.Raw()]
                ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                    queue.CopyBuffer(staging->GetSpan(), vb->GetRHI());
            })->AddBuffer(vertex_buffer.Raw(), RHIGPUAccessFlagBits::kWrite);
            // Draw
            builder.AddPass("SimpleShader", RDGPassType::kGraphics, {},
                TestShader2::GetShaderParamStructInfo(), params,
                [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
                    RDGCommandHelper::Draw<TestShader2>(queue, pass, shader, params, 3);
                });
            // Readback
            auto readback_buffer = RDGBuffer::Create(RHIBufferUsageFlagBits::kReadback, 1024 * 1024 * 16, false, true);
            readback_buffer->SetExport();
            builder.AddPass({},
                [ttex = test_texture.Raw(), obuf = readback_buffer.Raw()]
                ([[maybe_unused]] RDGPass * pass, RHICommandQueueGraphics & queue) {
                queue.CopyTextureToBuffer(ttex->GetRHI(), obuf->GetRHI().buffer);
            })->AddTexture(test_texture.Raw(), RDGTextureUsageType::kTransferSrc)
              ->AddBuffer(readback_buffer.Raw(), RHIGPUAccessFlagBits::kWrite);
            auto rdg = builder.Compile();
            auto pool = RDGResourcePool::Create();
            auto sync = RHI::Get().CreateSyncPoint();
            rdg->Execute(pool.Raw(), sync.Raw());
            sync->Wait();

            // Convert to bitmap
            auto fp32tex = (float *) readback_buffer->Map();
            auto bitmap = new uint32_t[128 * 128];
            for (int i = 0; i < 128 * 128; ++i) {
                auto r = static_cast<uint8_t>(fp32tex[i * 4 + 0] * 255);
                auto g = static_cast<uint8_t>(fp32tex[i * 4 + 1] * 255);
                auto b = static_cast<uint8_t>(fp32tex[i * 4 + 2] * 255);
                auto a = static_cast<uint8_t>(fp32tex[i * 4 + 3] * 255);
                // bgra
                bitmap[i] = (a << 24) | (b << 16) | (g << 8) | r;
            }
            // Save to file
            stbi_write_png("rdg_test.png", 128, 128, 4, bitmap, 128 * 4);

            lib.ReleaseCompiledShaders();
        }

        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}

class TestShader3 : public RDGShader {
public:
    DECLARE_SHADER()
    struct TestShader3UB {
        uint32_t Index;
        glm::uvec3 Padding;
    };
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_UNIFORM_BUFFER(TestShader3UB, UB)
        SHADER_RESOURCE_PARAMETER(Texture2DArray, TextureArray)
        SHADER_VERTEX_BUFFER(12 + 8, vertex_buffer)
        SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k3xFp32, pos)
        SHADER_VERTEX_ATTRIBUTE(0, 12, RHIVertexAttributeFormatType::k2xFp32, uv)
        SHADER_RENDER_TARGET(PixelFormatType::kR32G32B32A32_FLOAT, OutColor)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    static std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}