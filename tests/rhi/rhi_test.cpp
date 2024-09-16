/*
 * Created: 2024/9/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rhi/rhi.h"

TEST(RHITest, RHIStart) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();

    RHI::InitializeSingleton(RHIType::kVulkan);
    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}