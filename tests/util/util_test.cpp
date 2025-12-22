/*
 * Created: 2025/5/23
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <gtest/gtest.h>
#include "core/infra.h"
#include "infra_impl/infra.h"
#include "rdg/rdg.h"
#include "rdg/rdg_builder.h"
#include "rdg/rdg_pool.h"

#include <exception>
#include <random>
#include <cpptrace/from_current.hpp>
#include "core/task.h"
#include "rdg/rdg_helper.h"
#include "rdg/rdg_shader.h"
#include "rhi/rhi_buffer.h"
#include "renderer/util/radix_sort.h"
#include "renderer/util/scan_sum.h"

TEST(UtilTest, UtilRadixSort) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>(true));
    GetInfra().Init();

    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);

    RHI::InitializeSingleton(RHIType::kVulkan);
    TaskGraph::InitializeSingleton(2, 2); // 2 low perf and 2 high perf threads

    RDGShaderLibrary::Get().Init();

    {
        RenderGraphBuilder builder;
        auto pool = RDGResourcePool::Create();
        uint32_t num_elements = 1819103;
        auto src_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        src_keys->SetName("SrcKeysBuffer");
        auto src_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        src_values->SetName("SrcValuesBuffer");
        auto dst_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        dst_keys->SetName("DstKeysBuffer");
        auto dst_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        dst_values->SetName("DstValuesBuffer");

        std::vector<uint32_t> host_keys(num_elements);
        std::vector<uint32_t> host_values(num_elements);

        // Initialize host data with random values
        {
            std::mt19937 rng(312);
            for (uint32_t i = 0; i < num_elements; ++i) {
                host_keys[i] = rng(); // Random keys
                host_values[i] = rng(); // Random values
            }
            // Manually select some keys and duplicate them to test stability
            for (int i = 0; i < 7; i++) {
                int index = rng() % num_elements;
                for (int j = 0; j < 800; j++) {
                    int k = rng() % num_elements;
                    host_keys[k] = host_keys[index];
                }
            }
        }

        Helpers::UploadWithRDG(builder, src_keys.Raw(), host_keys.data(), host_keys.size() * sizeof(uint32_t));
        Helpers::UploadWithRDG(builder, src_values.Raw(), host_values.data(), host_values.size() * sizeof(uint32_t));

        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, src_keys.Raw(), dst_keys.Raw(), src_values.Raw(), dst_values.Raw());
        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, dst_keys.Raw(), src_keys.Raw(), dst_values.Raw(), src_values.Raw());
        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, src_keys.Raw(), dst_keys.Raw(), src_values.Raw(), dst_values.Raw());



        auto readback_values = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        auto readback_keys = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        Helpers::ReadbackWithRDG(builder, dst_keys.Raw(), 0, readback_keys->GetSpan());
        Helpers::ReadbackWithRDG(builder, dst_values.Raw(), 0, readback_values->GetSpan());
        builder.Compile()->Execute(pool.Raw());

        RHI::Get().AdvanceFrame();

        RHI::Get().WaitForIdle();

        std::vector<uint32_t> readback_values_data(num_elements);
        std::vector<uint32_t> readback_keys_data(num_elements);
        std::memcpy(readback_keys_data.data(), readback_keys->Map(), num_elements * sizeof(uint32_t));
        std::memcpy(readback_values_data.data(), readback_values->Map(), num_elements * sizeof(uint32_t));

        // Validate using std::sort
        std::vector<uint32_t> sorted_keys = host_keys;
        std::vector<uint32_t> sorted_values = host_values;
        std::vector<std::pair<uint32_t, uint32_t>> pairs(num_elements);
        for (uint32_t i = 0; i < num_elements; ++i) {
            pairs[i] = {host_keys[i], host_values[i]};
        }
        // Stable sorting according to the key
        struct cmp {
            bool operator () (const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) const {
                return a.first < b.first;
            }
        };
        std::stable_sort(pairs.begin(), pairs.end(), cmp());
        for (uint32_t i = 0; i < num_elements; ++i) {
            sorted_keys[i] = pairs[i].first;
            sorted_values[i] = pairs[i].second;
        }
        uint32_t mismatch_index = UINT32_MAX;
        for (uint32_t i = 0; i < num_elements; ++i) {
            if (readback_keys_data[i] != sorted_keys[i]) mismatch_index = std::min(mismatch_index, i);
            if (readback_values_data[i] != sorted_values[i]) mismatch_index = std::min(mismatch_index, i);
        }
        if (mismatch_index != UINT32_MAX) {
            std::cout << "Mismatch at index: " << mismatch_index << std::endl;
            std::cout << "Expected key: " << sorted_keys[mismatch_index] << ", got: " << readback_keys_data[mismatch_index] << std::endl;
            std::cout << "Expected value: " << sorted_values[mismatch_index] << ", got: " << readback_values_data[mismatch_index] << std::endl;
        }
        ASSERT_EQ(mismatch_index, UINT32_MAX) << "Radix sort failed to sort the data correctly.";
    }


    RDGShaderLibrary::Get().Deinit();

    RHI::DestroySingleton();
    TaskGraph::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}


TEST(UtilTest, UtilRadixSortIndirect) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>(true));
    GetInfra().Init();

    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);

    RHI::InitializeSingleton(RHIType::kVulkan);
    TaskGraph::InitializeSingleton(2, 2);

    RDGShaderLibrary::Get().Init();

    {
        RenderGraphBuilder builder;
        auto pool = RDGResourcePool::Create();
        uint32_t num_elements = 819103;
        uint32_t num_sort_elements = 582911; // Number of elements to sort, must be less than num_elements
        auto src_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        src_keys->SetName("SrcKeysBuffer");
        auto src_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        src_values->SetName("SrcValuesBuffer");
        auto dst_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        dst_keys->SetName("DstKeysBuffer");
        auto dst_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        dst_values->SetName("DstValuesBuffer");
        auto count_buffer = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, sizeof(uint32_t));
        count_buffer->SetName("CountBuffer");

        std::vector<uint32_t> host_keys(num_elements);
        std::vector<uint32_t> host_values(num_elements);

        // Initialize host data with random values
        {
            std::mt19937 rng(312);
            for (uint32_t i = 0; i < num_elements; ++i) {
                host_keys[i] = rng(); // Random keys
                host_values[i] = rng(); // Random values
            }
            // Manually select some keys and duplicate them to test stability
            for (int i = 0; i < 7; i++) {
                int index = rng() % num_elements;
                for (int j = 0; j < 800; j++) {
                    int k = rng() % num_elements;
                    host_keys[k] = host_keys[index];
                }
            }
        }

        Helpers::UploadWithRDG(builder, src_keys.Raw(), host_keys.data(), host_keys.size() * sizeof(uint32_t));
        Helpers::UploadWithRDG(builder, src_values.Raw(), host_values.data(), host_values.size() * sizeof(uint32_t));
        Helpers::UploadWithRDG(builder, count_buffer.Raw(), &num_sort_elements, sizeof(uint32_t));

        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, src_keys.Raw(), dst_keys.Raw(), src_values.Raw(), dst_values.Raw(), count_buffer.Raw());
        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, dst_keys.Raw(), src_keys.Raw(), dst_values.Raw(), src_values.Raw(), count_buffer.Raw());
        DeviceRadixSort::AddRadixSort32BitsPass(builder, num_elements, src_keys.Raw(), dst_keys.Raw(), src_values.Raw(), dst_values.Raw(), count_buffer.Raw());

        auto readback_values = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        auto readback_keys = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        Helpers::ReadbackWithRDG(builder, dst_keys.Raw(), 0, readback_keys->GetSpan());
        Helpers::ReadbackWithRDG(builder, dst_values.Raw(), 0, readback_values->GetSpan());
        builder.Compile()->Execute(pool.Raw());

        RHI::Get().AdvanceFrame();

        RHI::Get().WaitForIdle();

        std::vector<uint32_t> readback_values_data(num_elements);
        std::vector<uint32_t> readback_keys_data(num_elements);
        std::memcpy(readback_keys_data.data(), readback_keys->Map(), num_elements * sizeof(uint32_t));
        std::memcpy(readback_values_data.data(), readback_values->Map(), num_elements * sizeof(uint32_t));

        // Validate using std::sort
        std::vector<uint32_t> sorted_keys = host_keys;
        std::vector<uint32_t> sorted_values = host_values;
        std::vector<std::pair<uint32_t, uint32_t>> pairs(num_elements);
        for (uint32_t i = 0; i < num_elements; ++i) {
            pairs[i] = {host_keys[i], host_values[i]};
        }
        // Stable sorting according to the key
        struct cmp {
            bool operator () (const std::pair<uint32_t, uint32_t> &a, const std::pair<uint32_t, uint32_t> &b) const {
                return a.first < b.first;
            }
        };
        std::stable_sort(pairs.begin(), pairs.begin() + num_sort_elements, cmp());
        for (uint32_t i = 0; i < num_elements; ++i) {
            sorted_keys[i] = pairs[i].first;
            sorted_values[i] = pairs[i].second;
        }
        uint32_t mismatch_index = UINT32_MAX;
        for (uint32_t i = 0; i < num_sort_elements; ++i) {
            if (readback_keys_data[i] != sorted_keys[i]) mismatch_index = std::min(mismatch_index, i);
            if (readback_values_data[i] != sorted_values[i]) mismatch_index = std::min(mismatch_index, i);
        }
        if (mismatch_index != UINT32_MAX) {
            std::cout << "Mismatch at index: " << mismatch_index << std::endl;
            std::cout << "Expected key: " << sorted_keys[mismatch_index] << ", got: " << readback_keys_data[mismatch_index] << std::endl;
            std::cout << "Expected value: " << sorted_values[mismatch_index] << ", got: " << readback_values_data[mismatch_index] << std::endl;
        }
        ASSERT_EQ(mismatch_index, UINT32_MAX) << "Radix sort failed to sort the data correctly.";
    }

    RDGShaderLibrary::Get().Deinit();
    TaskGraph::DestroySingleton();
    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}

TEST(UtilTest, UtilScanSum) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>(true));
    GetInfra().Init();

    // Pretend to be render thread to pass assertions in builder/helpers
    SetCurrentThreadType(ThreadType::kRenderThread);

    RHI::InitializeSingleton(RHIType::kVulkan);
    TaskGraph::InitializeSingleton(2, 2);

    RDGShaderLibrary::Get().Init();

    {
        RenderGraphBuilder builder;
        auto pool = RDGResourcePool::Create();

        // 使用一个适中规模的测试数组
        uint32_t num_elements = 91738; // 可调整
        std::vector<uint32_t> host_values(num_elements);

        // 生成确定性随机数据
        std::mt19937 rng(12345);
        for (uint32_t i = 0; i < num_elements; ++i) {
            host_values[i] = rng();
        }

        // 创建源/目标 buffer
        auto src_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        src_values->SetName("ScanSum_SrcValues");
        auto dst_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        dst_values->SetName("ScanSum_DstValues");

        // 上传输入数据
        Helpers::UploadWithRDG(builder, src_values.Raw(), host_values.data(), host_values.size() * sizeof(uint32_t));

        // 调用 DeviceScanSum 非间接版本
        DeviceScanSum::AddScanSum32BitsPass(builder, num_elements, src_values.Raw(), dst_values.Raw(), nullptr, "ScanSumTest");

        // 读回结果
        auto readback = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        Helpers::ReadbackWithRDG(builder, dst_values.Raw(), 0, readback->GetSpan());

        builder.Compile()->Execute(pool.Raw());

        RHI::Get().AdvanceFrame();
        RHI::Get().WaitForIdle();

        std::vector<uint32_t> gpu_out(num_elements);
        std::memcpy(gpu_out.data(), readback->Map(), num_elements * sizeof(uint32_t));

        // 在 CPU 上计算 32 位前缀和（使用 64 位累加然后截断为 uint32_t，以匹配 GPU 的 32 位行为）
        std::vector<uint32_t> cpu_out(num_elements);
        uint64_t acc = 0;
        for (uint32_t i = 0; i < num_elements; ++i) {
            acc += static_cast<uint64_t>(host_values[i]);
            cpu_out[i] = static_cast<uint32_t>(acc & 0xFFFFFFFFu);
        }

        // 比较
        uint32_t mismatch_index = UINT32_MAX;
        for (uint32_t i = 0; i < num_elements; ++i) {
            if (gpu_out[i] != cpu_out[i]) {
                mismatch_index = i;
                break;
            }
        }

        if (mismatch_index != UINT32_MAX) {
            std::cout << "ScanSum mismatch at index " << mismatch_index << "\n";
            std::cout << "Expected: " << cpu_out[mismatch_index] << " Got: " << gpu_out[mismatch_index] << "\n";
        }

        ASSERT_EQ(mismatch_index, UINT32_MAX) << "ScanSum GPU result differs from CPU reference.";
    }

    RDGShaderLibrary::Get().Deinit();
    TaskGraph::DestroySingleton();
    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
