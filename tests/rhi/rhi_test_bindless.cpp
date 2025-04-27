/*
 * Created: 2025/4/27
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
#include "rhi/rhi_bindless.h"
#include "rhi/rhi_bindlesskeeper.h"

#include <exception>
#include <cpptrace/from_current.hpp>

#include <rhi/rhi_buffer.h>

TEST(RHITest, RHIBindlessBasics) {
    using namespace mi;
    
    CPPTRACE_TRY {
        // 设置基础环境
        TransferInfra(std::make_unique<MyInfra>());
        GetInfra().Init();
        SetCurrentThreadType(ThreadType::kRenderThread);
        RHI::InitializeSingleton(RHIType::kVulkan);
        
        // 创建测试用的资源
        // 1. 创建buffer资源
        auto buffer = RHI::Get().CreateBuffer(
            1024, 
            RHIBufferUsageFlagBits::kStorage | RHIBufferUsageFlagBits::kReadback
        );
        EXPECT_TRUE(buffer);
        
        // 写入一些数据供测试
        uint32_t* buffer_data = reinterpret_cast<uint32_t*>(buffer->Map());
        buffer_data[0] = 0xDEADBEEF;
        buffer_data[1] = 0x12345678;
        
        // 2. 创建纹理资源
        auto texture = RHI::Get().CreateTexture(
            RHITextureType::k2D, 
            RHITextureDimensions{64, 64}, 
            PixelFormatType::kR8G8B8A8_UNORM,
            RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransferDst
        );
        EXPECT_TRUE(texture);
        
        // 测试 Bindless 资源槽创建
        MI_LOG(MIInfraLogType::kInfo, "创建 Bindless 资源槽");
        
        // 1. 创建 Buffer 的 Bindless 槽
        auto buffer_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHIBuffer>();
        EXPECT_TRUE(buffer_slot);
        EXPECT_TRUE(buffer_slot->GetType() == RHIBindlessResourceType::kReadOnlyStorageBuffer);
        
        // 2. 创建 Texture 的 Bindless 槽
        auto texture_slot = RHI::Get().GetBindlessManager().AllocateResourceSlot<RHITexture>();
        EXPECT_TRUE(texture_slot);
        EXPECT_TRUE(texture_slot->GetType() == RHIBindlessResourceType::kSRV);
        
        // 3. 记录初始槽索引
        uint32_t buffer_slot_index = buffer_slot->GetSlot();
        uint32_t texture_slot_index = texture_slot->GetSlot();
        
        MI_LOG(MIInfraLogType::kInfo, "分配的 Buffer Slot: {}", buffer_slot_index);
        MI_LOG(MIInfraLogType::kInfo, "分配的 Texture Slot: {}", texture_slot_index);
        
        // 设置资源到槽位并提交
        MI_LOG(MIInfraLogType::kInfo, "设置资源到 Bindless 槽位");
        buffer_slot->SetAndCommit(buffer.Raw());
        texture_slot->SetAndCommit(texture.Raw());
        
        // 验证 Get 方法能正确获取到设置的资源
        EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
        EXPECT_EQ(texture_slot->Get(), texture.Raw());
        
        // 推进一帧，测试资源的持久性
        RHI::Get().AdvanceFrame();
        
        // 重新检查资源是否仍然存在
        EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
        EXPECT_EQ(texture_slot->Get(), texture.Raw());
        
        // 重置槽位资源后测试
        MI_LOG(MIInfraLogType::kInfo, "重置资源并测试空槽位");
        buffer_slot->SetAndCommit(nullptr);
        texture_slot->SetAndCommit(nullptr);
        
        EXPECT_EQ(buffer_slot->Get(), nullptr);
        EXPECT_EQ(texture_slot->Get(), nullptr);
        
        // 测试批量更新
        MI_LOG(MIInfraLogType::kInfo, "测试批量更新");
        buffer_slot->Set(buffer.Raw());
        texture_slot->Set(texture.Raw());
        
        // 批量提交更新
        RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(buffer_slot.Raw());
        RHI::Get().GetBindlessManager().CommitResourceSlotUpdate(texture_slot.Raw());
        
        // 验证更新后的资源
        EXPECT_EQ(buffer_slot->Get(), buffer.Raw());
        EXPECT_EQ(texture_slot->Get(), texture.Raw());
        
        // 释放资源槽
        MI_LOG(MIInfraLogType::kInfo, "释放资源槽");
        buffer_slot = nullptr;  // 引用计数归零，应该会触发资源槽释放
        texture_slot = nullptr;
        
        // 推进一帧，使延迟释放的槽位真正释放
        RHI::Get().AdvanceFrame();
        
        // 清理
        RHI::DestroySingleton();
        GetInfra().Shutdown();
        DestroyInfra();
        
        MI_LOG(MIInfraLogType::kInfo, "Bindless 测试完成");
    } CPPTRACE_CATCH (const std::exception &e) {
        cpptrace::from_current_exception().print();
        FAIL() << e.what();
    }
}
