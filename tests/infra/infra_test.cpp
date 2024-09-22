/*
 * Created: 2024/9/16
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <gtest/gtest.h>
#include <infra_impl/infra.h>
#include <core/blobres.h>

TEST(InfraTest, InfraCreate) {
    using namespace mi;
    auto infra = std::make_unique<MyInfra>();
}

TEST(InfraTest, InfraCompileShader) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    std::string error;
    std::vector<uint32_t> spirv = GetInfra().CompileHLSLToSPIRV(L"", "main", "cs_6_3", "void main() { }", {}, error);
    EXPECT_TRUE(spirv.size() > 0);
    GetInfra().Shutdown();
    DestroyInfra();
}

TEST(InfraTest, InfraResourceIO) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    auto resource = GetInfra().RIO_Open(
            "resources/test.txt",
            MIInfraResourceHintType::kBlob,
            BlobResourceAccessFlagBits::kAll);
    {
        int data[64];
        for (int i = 0; i < 64; i++) {
            data[i] = i;
        }
        resource->WriteBlob(0, 64 * 4, data);
    }
    {
        int data2[64];
        auto fut = resource->Async_ReadBlob(0, 64 * 4, data2);
        fut.wait();
        for (int i = 0; i < 64; i++) {
            EXPECT_EQ(data2[i], i);
        }
    }
    {
        int data3[64];
        resource->ReadBlob(0, 64 * 4, data3);
        for(int i = 0; i < 64; i++) {
            data3[i] = 1023 - i;
        }
        resource->Async_WriteBlob(0, 64 * 4, data3).wait();
        resource->ReadBlob(0, 64 * 4, data3);
        for(int i = 0; i < 64; i++) {
            EXPECT_EQ(data3[i], 1023 - i);
        }
    }
    GetInfra().Shutdown();
    DestroyInfra();
}

TEST(InfraTest, InfraThreadLaunch) {
    using namespace mi;
    // Test the thread launch function
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();
    volatile int v = 0;
    auto thread = GetInfra().LaunchThread(ThreadPerformanceType::kHigh, [&v]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        v = 1;
    });
    EXPECT_TRUE(thread);
    if(thread) {
        thread.value()->join();
        EXPECT_TRUE(v == 1);
    }
    GetInfra().Shutdown();
    DestroyInfra();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}