/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <cpptrace/from_current.hpp>
#include <gtest/gtest.h>
#include <rdg/rdg_builder.h>
#include <rdg/rdg_cmd.h>
#include <rdg/rdg_pool.h>
#include <rhi/rhi_pipeline.h>

#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"
#include "rdg/rdg_shader.h"
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN

BEGIN_SHADER_PARAMETERS(TestParamInnerStruct)
    SHADER_PARAMETER(int, TestInteger1)
    SHADER_PARAMETER(int2, TestInteger2_1)
    SHADER_PARAMETER(Texture2D, texture1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestParamsInnerStructRef)
    SHADER_PARAMETER_STRUCT_INCLUDE(TestParamInnerStruct, inner2)
    SHADER_PARAMETER(float3, TestFloat3)
    SHADER_PARAMETER(float3, TestFloat3_1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestNestParams)
    SHADER_PARAMETER(int, TestInteger1)
    SHADER_PARAMETER(int2, TestInteger2_1)
END_SHADER_PARAMETERS()


BEGIN_SHADER_PARAMETERS(TestParams)
    SHADER_PARAMETER_STRUCT_REF(TestNestParams, in1)
    SHADER_PARAMETER_STRUCT_INCLUDE(TestParamsInnerStructRef, in2)
    SHADER_PARAMETER(int, TestInteger0)
    SHADER_PARAMETER(int2, TestInteger2_0)
    SHADER_PARAMETER_STRUCT_NESTED(TestNestParams, inner)
    SHADER_PARAMETER(int, TestInteger33)
    SHADER_PARAMETER(int4, TestInteger44)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

TEST(RDGTest, RDGShaderParams) {
    using namespace mi;
    CPPTRACE_TRY {
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();
        TestParams t {};

        auto meta_test_inner = *TestParamInnerStruct::GetParamStructInfo();
        EXPECT_EQ(meta_test_inner.cpp_members.size(), 3);
        EXPECT_EQ(meta_test_inner.cpp_members[0].name, "TestInteger1");
        EXPECT_EQ(meta_test_inner.cpp_members[1].name, "TestInteger2_1");
        EXPECT_EQ(meta_test_inner.cpp_members[0].cpp_offset, offsetof(TestParamInnerStruct, TestInteger1));
        EXPECT_EQ(meta_test_inner.cpp_members[1].cpp_offset, offsetof(TestParamInnerStruct, TestInteger2_1));

        auto meta_test_inner2 = *TestParamsInnerStructRef::GetParamStructInfo();
        EXPECT_EQ(meta_test_inner2.cpp_members.size(), 5);
        EXPECT_EQ(meta_test_inner2.cpp_members[0].name, "TestInteger1");
        EXPECT_EQ(meta_test_inner2.cpp_members[1].name, "TestInteger2_1");
        EXPECT_EQ(meta_test_inner2.cpp_members[3].name, "TestFloat3");
        EXPECT_EQ(meta_test_inner2.cpp_members[4].name, "TestFloat3_1");
        EXPECT_EQ(meta_test_inner2.cpp_members[0].cpp_offset, offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger1));
        EXPECT_EQ(meta_test_inner2.cpp_members[1].cpp_offset, offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger2_1));
        EXPECT_EQ(meta_test_inner2.cpp_members[3].cpp_offset, offsetof(TestParamsInnerStructRef, TestFloat3));
        EXPECT_EQ(meta_test_inner2.cpp_members[4].cpp_offset, offsetof(TestParamsInnerStructRef, TestFloat3_1));

        auto meta_test = *TestParams::GetParamStructInfo();
        EXPECT_EQ(meta_test.cpp_members.size(), 11);
        EXPECT_EQ(meta_test.cpp_members[0].name, "in1");
        EXPECT_EQ(meta_test.cpp_members[1].name, "TestInteger1");
        EXPECT_EQ(meta_test.cpp_members[2].name, "TestInteger2_1");
        EXPECT_EQ(meta_test.cpp_members[4].name, "TestFloat3");
        EXPECT_EQ(meta_test.cpp_members[5].name, "TestFloat3_1");
        EXPECT_EQ(meta_test.cpp_members[6].name, "TestInteger0");
        EXPECT_EQ(meta_test.cpp_members[7].name, "TestInteger2_0");
        EXPECT_EQ(meta_test.cpp_members[8].name, "inner");
        EXPECT_EQ(meta_test.cpp_members[9].name, "TestInteger33");
        EXPECT_EQ(meta_test.cpp_members[10].name, "TestInteger44");
        // Test offsets for all the members in TestParams
        EXPECT_EQ(meta_test.cpp_members[0].cpp_offset, offsetof(TestParams, in1));
        EXPECT_EQ(meta_test.cpp_members[1].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger1));
        EXPECT_EQ(meta_test.cpp_members[2].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger2_1));
        EXPECT_EQ(meta_test.cpp_members[4].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, TestFloat3));
        EXPECT_EQ(meta_test.cpp_members[5].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, TestFloat3_1));
        EXPECT_EQ(meta_test.cpp_members[6].cpp_offset, offsetof(TestParams, TestInteger0));
        EXPECT_EQ(meta_test.cpp_members[7].cpp_offset, offsetof(TestParams, TestInteger2_0));
        EXPECT_EQ(meta_test.cpp_members[8].cpp_offset, offsetof(TestParams, inner));
        EXPECT_EQ(meta_test.cpp_members[9].cpp_offset, offsetof(TestParams, TestInteger33));
        EXPECT_EQ(meta_test.cpp_members[10].cpp_offset, offsetof(TestParams, TestInteger44));

        // Test HLSL offsets, which follow D3D constant buffer packing rules
        // Basic types (float, int) are 4 bytes and float2/int2 are 8 bytes (aligned to 8)
        // float3/int3 are 12 bytes but aligned to 16, float4/int4 are 16 bytes
        EXPECT_EQ(meta_test.members[0].offset, 0);  // in1 (pointer) to a struct takes 12 bytes on device
        EXPECT_EQ(meta_test.members[1].offset, 0); // TestInteger1 (int, aligned to 4)
        EXPECT_EQ(meta_test.members[2].offset, 4); // TestInteger2_1 (int2, aligned to 4)
        EXPECT_EQ(meta_test.members[4].offset, 16); // TestFloat3 (float3, original alignment is 4. Buffer-row: aligned to 16)
        EXPECT_EQ(meta_test.members[5].offset, 32); // TestFloat3_1 (float3, Buffer-row: aligned to 16)
        EXPECT_EQ(meta_test.members[6].offset, 44); // TestInteger0 (int, aligned to 4)
        EXPECT_EQ(meta_test.members[7].offset, 48); // TestInteger2_0 (int2, aligned to 4)
        EXPECT_EQ(meta_test.members[8].offset, 64); // inner (struct of size 12, aligned to 16 with buffer row rule)
        EXPECT_EQ(meta_test.members[9].offset, 76); // TestInteger33 (int, aligned to 4)
        EXPECT_EQ(meta_test.members[10].offset, 80); // TestInteger44 (int4, aligned to 16)

        // Query members
        EXPECT_EQ(meta_test.GetCppMemberIndex("in1"), 0);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestInteger1"), 1);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestInteger2_1"), 2);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestFloat3"), 4);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestFloat3_1"), 5);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestInteger0"), 6);
        EXPECT_EQ(meta_test.GetCppMemberIndex("TestInteger2_0"), 7);

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
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(float4, TestFloat4)
        SHADER_PARAMETER(float2, TestFloat2)
        SHADER_PARAMETER(RWTexture2D, TestTexture)
        // SHADER_VERTEX_BUFFER(16, vertex_buffer)
        // SHADER_VERTEX_ATTRIBUTE(0, 0, RHIVertexAttributeFormatType::k4xFp32, pos)
        // SHADER_VERTEX_ATTRIBUTE(0, 16, RHIVertexAttributeFormatType::k2xFp32, uv)
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
        TransferInfra(std::make_unique<MyInfra>(resource_dir.string()));
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            auto & lib = RDGShaderLibrary::GetInstance();
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
        TransferInfra(std::make_unique<MyInfra>(resource_dir.string()));
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        {
            auto & lib = RDGShaderLibrary::GetInstance();
            lib.Init();
            auto shader = lib.GetShader<TestShader1>();
            EXPECT_TRUE(shader->IsValid());
            RenderGraphBuilder builder;
            auto params = builder.Allocate<TestShader1::Parameters>();
            params->TestFloat2 = {0.1f, 0.2f};
            params->TestFloat4 = {0.3f, 0.4f, 0.5f, 0.6f};
            auto test_texture = builder.CreateTexture2D(128, 128, PixelFormatType::kR32G32B32A32_FLOAT);
            params->TestTexture = test_texture.Raw();
            builder.AddPass("SimpleShader", RDGPassType::kCompute, RDGPassFlagBits::kNeverCull,
                TestShader1::GetShaderParamStructInfo(), params,
                [shader, params](RDGPass * pass, RHICommandQueueGraphics & queue) {
                    RDGCommandHelper::Dispatch<TestShader1>(queue, pass, shader, params);
                });
            auto rdg = builder.Compile();
            auto pool = std::make_unique<RDGResourcePool>();
            rdg->Execute(pool.get());
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


int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}