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
#include "util/radix_sort.h"
#include "rdg/rdg_pool.h"

#include <exception>
#include <cpptrace/from_current.hpp>

#include "rdg/rdg_helper.h"
#include "rhi/rhi_buffer.h"

TEST(UtilTest, UtilRadixSort) {
    using namespace mi;
    TransferInfra(std::make_unique<MyInfra>());
    GetInfra().Init();

    // Hack: we need to pretend that we're a render thread to pass the assertions
    SetCurrentThreadType(ThreadType::kRenderThread);

    RHI::InitializeSingleton(RHIType::kVulkan);

    {
        RenderGraphBuilder builder;
        auto pool = RDGResourcePool::Create();
        uint32_t num_elements = 632812;
        auto src_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        auto src_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        auto dst_keys = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));
        auto dst_values = builder.CreateBuffer(RHIBufferUsageFlagBits::kStorage, num_elements * sizeof(uint32_t));

        std::vector<uint32_t> host_keys(num_elements);
        std::vector<uint32_t> host_values(num_elements);
        Helpers::UploadWithRDG(builder, src_keys.Raw(), host_keys.data(), host_keys.size() * sizeof(uint32_t));
        Helpers::UploadWithRDG(builder, src_values.Raw(), host_values.data(), host_values.size() * sizeof(uint32_t));
        RadixSort::AddRadixSort32BitsPass(builder, num_elements, src_keys.Raw(), src_values.Raw(),
                                          dst_keys.Raw(), dst_values.Raw());
        auto readback_values = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        auto readback_keys = RHI::Get().CreateBuffer(num_elements * sizeof(uint32_t), RHIBufferUsageFlagBits::kReadback);
        Helpers::ReadbackWithRDG(builder, dst_keys.Raw(), 0, readback_keys->GetSpan());
        Helpers::ReadbackWithRDG(builder, dst_values.Raw(), 0, readback_keys->GetSpan());
        builder.Compile()->Execute(pool.Raw());

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
        std::sort(pairs.begin(), pairs.end());
        for (uint32_t i = 0; i < num_elements; ++i) {
            sorted_keys[i] = pairs[i].first;
            sorted_values[i] = pairs[i].second;
        }
        uint32_t mismatch_index = UINT32_MAX;
        for (uint32_t i = 0; i < num_elements; ++i) {
            if (readback_keys_data[i] != sorted_keys[i]) mismatch_index = i;
            if (readback_values_data[i] != sorted_values[i]) mismatch_index = i;
        }
        if (mismatch_index != UINT32_MAX) {
            std::cout << "Mismatch at index: " << mismatch_index << std::endl;
            std::cout << "Expected key: " << sorted_keys[mismatch_index] << ", got: " << readback_keys_data[mismatch_index] << std::endl;
            std::cout << "Expected value: " << sorted_values[mismatch_index] << ", got: " << readback_values_data[mismatch_index] << std::endl;
        }
        ASSERT_EQ(mismatch_index, UINT32_MAX) << "Radix sort failed to sort the data correctly.";
    }

    RHI::DestroySingleton();

    GetInfra().Shutdown();
    DestroyInfra();
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
