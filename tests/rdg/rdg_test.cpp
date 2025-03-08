/*
 * Created: 2025/3/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"
#include "rdg/rdg_param.h"

MI_NAMESPACE_BEGIN

BEGIN_SHADER_PARAMETERS(TestParamInnerStruct)
    SHADER_PARAMETER(int, TestInteger1)
    SHADER_PARAMETER(int2, TestInteger2_1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestParamsInnerStructRef)
    SHADER_PARAMETER_INCLUDE(TestParamInnerStruct, inner2)
    SHADER_PARAMETER(float3, TestFloat3)
    SHADER_PARAMETER(float3, TestFloat3_1)
END_SHADER_PARAMETERS()

BEGIN_SHADER_PARAMETERS(TestParams)
    SHADER_PARAMETER_STRUCT_REF(TestParamInnerStruct, in1)
    SHADER_PARAMETER_STRUCT_INCLUDE(TestParamsInnerStructRef, in2)
    SHADER_PARAMETER(int, TestInteger0)
    SHADER_PARAMETER(int2, TestInteger2_0)
    SHADER_PARAMETER_STRUCT(TestParamInnerStruct, inner)
    SHADER_PARAMETER(int, TestInteger33)
    SHADER_PARAMETER(int4, TestInteger44)
END_SHADER_PARAMETERS()

MI_NAMESPACE_END

TEST(RHITest, RHIShaderParams) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    TestParams t {};
    auto meta = TestParams::GetParamsMetaData();
    EXPECT_EQ(meta.members.size(), 5);
    EXPECT_EQ(meta.members[0].name, "TestInteger0");
    EXPECT_EQ(meta.members[1].offset, 4);
    EXPECT_EQ(meta.members[2].offset, 16);
    EXPECT_EQ(meta.members[3].offset, 28);
    EXPECT_EQ(meta.members[4].offset, 32);
    EXPECT_EQ(meta.members[4].size, 16);

    // EXPECT_EQ(offsetof(TestParams, TestInteger33), 28);

    EXPECT_EQ(meta.members[2].type, RHIParamType::kStruct);
    auto inner_meta = meta.members[2].struct_info;
    EXPECT_EQ(inner_meta->members.size(), 2);
    EXPECT_EQ(inner_meta->members[0].name, "TestInteger1");
    EXPECT_EQ(inner_meta->members[1].name, "TestInteger2_1");
    EXPECT_EQ(inner_meta->members[1].size, 8);
    EXPECT_EQ(inner_meta->members[1].offset, 4);
    EXPECT_EQ(inner_meta->members[1].type, RHIParamType::kBasic);
    EXPECT_EQ(inner_meta->members[1].basic_type, RHIBasicParamType::kInt2);
}


int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}