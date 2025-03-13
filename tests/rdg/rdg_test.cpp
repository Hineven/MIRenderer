/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
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
    SHADER_PARAMETER_INCLUDE(TestParamInnerStruct, inner2)
    SHADER_PARAMETER(float3, TestFloat3)
    SHADER_PARAMETER(float3, TestFloat3_1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestParams)
    SHADER_PARAMETER_STRUCT_REF(TestParamInnerStruct, in1)
    SHADER_PARAMETER_INCLUDE(TestParamsInnerStructRef, in2)
    SHADER_PARAMETER(int, TestInteger0)
    SHADER_PARAMETER(int2, TestInteger2_0)
    SHADER_PARAMETER_STRUCT(TestParamInnerStruct, inner)
    SHADER_PARAMETER(int, TestInteger33)
    SHADER_PARAMETER(int4, TestInteger44)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

TEST(RDGTest, RDGShaderParams) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    TestParams t {};

    auto meta_test_inner = TestParamInnerStruct::GetParamsMetaData();
    EXPECT_EQ(meta_test_inner.cpp_members.size(), 2);
    EXPECT_EQ(meta_test_inner.cpp_members[0].name, "TestInteger1");
    EXPECT_EQ(meta_test_inner.cpp_members[1].name, "TestInteger2_1");
    EXPECT_EQ(meta_test_inner.cpp_members[0].cpp_offset, offsetof(TestParamInnerStruct, TestInteger1));
    EXPECT_EQ(meta_test_inner.cpp_members[1].cpp_offset, offsetof(TestParamInnerStruct, TestInteger2_1));

    auto meta_test_inner2 = TestParamsInnerStructRef::GetParamsMetaData();
    EXPECT_EQ(meta_test_inner2.cpp_members.size(), 4);
    EXPECT_EQ(meta_test_inner2.cpp_members[0].name, "TestInteger1");
    EXPECT_EQ(meta_test_inner2.cpp_members[1].name, "TestInteger2_1");
    EXPECT_EQ(meta_test_inner2.cpp_members[2].name, "TestFloat3");
    EXPECT_EQ(meta_test_inner2.cpp_members[3].name, "TestFloat3_1");
    EXPECT_EQ(meta_test_inner2.cpp_members[0].cpp_offset, offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger1));
    EXPECT_EQ(meta_test_inner2.cpp_members[1].cpp_offset, offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger2_1));
    EXPECT_EQ(meta_test_inner2.cpp_members[2].cpp_offset, offsetof(TestParamsInnerStructRef, TestFloat3));
    EXPECT_EQ(meta_test_inner2.cpp_members[3].cpp_offset, offsetof(TestParamsInnerStructRef, TestFloat3_1));

    auto meta_test = TestParams::GetParamsMetaData();
    EXPECT_EQ(meta_test.cpp_members.size(), 10);
    EXPECT_EQ(meta_test.cpp_members[0].name, "in1");
    EXPECT_EQ(meta_test.cpp_members[1].name, "TestInteger1");
    EXPECT_EQ(meta_test.cpp_members[2].name, "TestInteger2_1");
    EXPECT_EQ(meta_test.cpp_members[3].name, "TestFloat3");
    EXPECT_EQ(meta_test.cpp_members[4].name, "TestFloat3_1");
    EXPECT_EQ(meta_test.cpp_members[5].name, "TestInteger0");
    EXPECT_EQ(meta_test.cpp_members[6].name, "TestInteger2_0");
    EXPECT_EQ(meta_test.cpp_members[7].name, "inner");
    EXPECT_EQ(meta_test.cpp_members[8].name, "TestInteger33");
    EXPECT_EQ(meta_test.cpp_members[9].name, "TestInteger44");
    // Test offsets for all the members in TestParams
    EXPECT_EQ(meta_test.cpp_members[0].cpp_offset, offsetof(TestParams, in1));
    EXPECT_EQ(meta_test.cpp_members[1].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger1));
    EXPECT_EQ(meta_test.cpp_members[2].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, inner2) + offsetof(TestParamInnerStruct, TestInteger2_1));
    EXPECT_EQ(meta_test.cpp_members[3].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, TestFloat3));
    EXPECT_EQ(meta_test.cpp_members[4].cpp_offset, offsetof(TestParams, in2) + offsetof(TestParamsInnerStructRef, TestFloat3_1));
    EXPECT_EQ(meta_test.cpp_members[5].cpp_offset, offsetof(TestParams, TestInteger0));
    EXPECT_EQ(meta_test.cpp_members[6].cpp_offset, offsetof(TestParams, TestInteger2_0));
    EXPECT_EQ(meta_test.cpp_members[7].cpp_offset, offsetof(TestParams, inner));
    EXPECT_EQ(meta_test.cpp_members[8].cpp_offset, offsetof(TestParams, TestInteger33));
    EXPECT_EQ(meta_test.cpp_members[9].cpp_offset, offsetof(TestParams, TestInteger44));

    // Test HLSL offsets, which follow D3D constant buffer packing rules
    // Basic types (float, int) are 4 bytes and float2/int2 are 8 bytes (aligned to 8)
    // float3/int3 are 12 bytes but aligned to 16, float4/int4 are 16 bytes
    EXPECT_EQ(meta_test.members[0].offset, 0);  // in1 (pointer) to a struct takes 12 bytes on device
    EXPECT_EQ(meta_test.members[1].offset, 12);  // TestInteger1 (int, aligned to 4)
    EXPECT_EQ(meta_test.members[2].offset, 16); // TestInteger2_1 (int2, aligned to 8)
    EXPECT_EQ(meta_test.members[3].offset, 32); // TestFloat3 (float3, original alignment is 4. Buffer-row: aligned to 16)
    EXPECT_EQ(meta_test.members[4].offset, 48); // TestFloat3_1 (float3, Buffer-row: aligned to 16)
    EXPECT_EQ(meta_test.members[5].offset, 60); // TestInteger0 (int, aligned to 4)
    EXPECT_EQ(meta_test.members[6].offset, 64); // TestInteger2_0 (int2, aligned to 4)
    EXPECT_EQ(meta_test.members[7].offset, 80); // inner (struct of size 12, aligned to 16)
    EXPECT_EQ(meta_test.members[8].offset, 92); // TestInteger33 (int, aligned to 4)
    EXPECT_EQ(meta_test.members[9].offset, 96); // TestInteger44 (int4, aligned to 16)

    // Query members
    EXPECT_EQ(meta_test.GetMemberIndex("in1"), 0);
    EXPECT_EQ(meta_test.GetMemberIndex("TestInteger1"), 1);
    EXPECT_EQ(meta_test.GetMemberIndex("TestInteger2_1"), 2);
    EXPECT_EQ(meta_test.GetMemberIndex("TestFloat3"), 3);
    EXPECT_EQ(meta_test.GetMemberIndex("TestFloat3_1"), 4);
    EXPECT_EQ(meta_test.GetMemberIndex("TestInteger0"), 5);
    EXPECT_EQ(meta_test.GetMemberIndex("TestInteger2_0"), 6);
}

using namespace mi;
class TestShader1 : public RDGShader {
public:
    BEGIN_SHADER_PARAMETERS(Parameters)
        SHADER_PARAMETER(float4, TestFloat4)
    END_SHADER_PARAMETERS()
    RDG_SHADER_USE_PARAMETERS(Parameters)
    std::vector<std::string> GetDefaultMacros() {
        return {};
    }
};

IMPLEMENT_RDG_SHADER(TestShader1, "test_shader_1.hlsl", "Main", RHIPipelineType::kCompute);

TEST(RDGTest, RDGShaderLibrary) {
    using namespace mi;
    auto pwd = std::filesystem::current_path();
    auto resource_dir = pwd / "resources";
    TransferInfra(std::make_unique<MyInfra>(resource_dir.string()));
    GetInfra().Init();


}


int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}