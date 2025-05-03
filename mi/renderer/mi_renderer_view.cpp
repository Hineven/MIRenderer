/*
 * Created: 2025/4/19
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "renderer/mi_renderer_view.h"

#include "rdg/rdg_builder.h"
#include "rdg/rdg_pool.h"
#include "rdg/rdg_resource.h"
#include "renderer/mi_world.h"
#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_desc.h"

MI_NAMESPACE_BEGIN

RHIBufferSpan BatchedUploadContext::AllocateManualStagingBuffer(size_t size) {
    mi_assert(!fired_, "Allocating more staging buffer after upload.");
    bool dedicated = false;
    if (size + manual_staging_buffer_top_ > kStagingBufferDefaultSize) {
        if (size > kStagingBufferDefaultSize / 2) dedicated = true;
        else {
            manual_staging_memory_footprint_ += kStagingBufferDefaultSize;
            manual_staging_buffer_ = RHI::Get().CreateBuffer(kStagingBufferDefaultSize, RHIBufferUsageFlagBits::kStaging);
            manual_staging_buffer_top_ = 0;
        }
    }
    if (dedicated) {
        manual_staging_memory_footprint_ += size;
        return RHI::Get().CreateBuffer(size, RHIBufferUsageFlagBits::kStaging)->GetSpan();
    } else {
        auto buffer = manual_staging_buffer_;
        auto offset = manual_staging_buffer_top_;
        manual_staging_buffer_top_ += size;
        return {buffer.Raw(), offset};
    }
}

void BatchedUploadContext::Init() {
    manual_staging_buffer_ = {};
    manual_staging_buffer_top_ = {};
    manual_staging_memory_footprint_ = 0;
    fired_ = false;
}

void BatchedUploadContext::AddUnsafe(RHIBufferSpan buffer, const void *data, size_t size) {
    mi_assert(!fired_, "Adding uploads after upload.");
    PendingUpload upload = {};
    upload.dst_buffer = buffer;
    upload.data = std::span((const uint8_t*)data, size);
    pending_uploads_.emplace_back(upload);
}


void BatchedUploadContext::Add(RDGBuffer *buffer, const void *data, size_t size, size_t dst_offset) {
    mi_assert(!fired_, "Adding uploads after upload.");
    PendingRDGUpload upload = {};
    upload.dst_buffer = buffer;
    upload.data = std::span((const uint8_t*)data, size);
    upload.dst_offset = dst_offset;
    pending_rdg_uploads_.emplace_back(upload);
}

void BatchedUploadContext::Fire(RenderGraphBuilder &builder) {
    mi_assert(!fired_, "Batched double fire.");
    fired_ = true;
    std::sort(pending_rdg_uploads_.begin(), pending_rdg_uploads_.end(), [](const PendingRDGUpload a, const PendingUpload & b) {
        return a.dst_buffer < b.dst_buffer;
    });
    std::vector<RDGBuffer*> rdg_upload_buffers;
    for (auto & upload : pending_rdg_uploads_) {
        if (rdg_upload_buffers.empty() || rdg_upload_buffers.back() != upload.dst_buffer) {
            rdg_upload_buffers.push_back(upload.dst_buffer.Raw());
        }
    }

    size_t sum_upload_size = 0;
    for (auto & upload : pending_uploads_) {
        auto rounded_size = RoundUp(upload.data.size(), 16);
        sum_upload_size += rounded_size;
    }
    for (auto & upload : pending_rdg_uploads_) {
        auto rounded_size = RoundUp(upload.data.size(), 16);
        sum_upload_size += rounded_size;
    }
    auto staging_buffer = RHI::Get().CreateBuffer(sum_upload_size, RHIBufferUsageFlagBits::kStaging);
    auto staging_ptr = staging_buffer->Map();

    {
        size_t offset = 0;
        for (auto & upload : pending_uploads_) {
            auto rounded_size = RoundUp(upload.data.size(), 16);
            memcpy((std::byte*)staging_ptr + offset, upload.data.data(), upload.data.size());
            offset += rounded_size;
        }
        for (auto & upload : pending_rdg_uploads_) {
            auto rounded_size = RoundUp(upload.data.size(), 16);
            memcpy((std::byte*)staging_ptr + offset, upload.data.data(), upload.data.size());
            offset += rounded_size;
        }
    }

    auto pass = builder.AddPass("BatchedUploadBuffers", RDGPassType::kGeneric, {}, nullptr, nullptr,
        [staging = staging_buffer.Raw(), pending_rhi = std::move(pending_uploads_), pending_rdg = std::move(pending_rdg_uploads_)](
            RDGPass *pass, RHICommandQueueGraphics & queue
        ) {
            size_t offset = 0;
            for (const auto & e : pending_rhi) {
                auto size = e.data.size();
                auto rounded_size = RoundUp(size, 16);
                auto dst_span = e.dst_buffer;
                queue.CopyBuffer(RHIBufferSpan{staging, offset, size}, dst_span);
                offset += rounded_size;
            }
            for (const auto& e : pending_rdg) {
                auto size = e.data.size();
                auto rounded_size = RoundUp(size, 16);
                auto dst_span = e.dst_buffer->GetRHI();
                dst_span.offset += e.dst_offset;
                dst_span.size   = size;
                queue.CopyBuffer(RHIBufferSpan{staging, offset, size}, dst_span);
                offset += rounded_size;
            }
        }
    );
    // Add RDG buffer dependencies
    for (auto e : rdg_upload_buffers) {
        pass->AddBuffer(e, RHIGPUAccessFlagBits::kWrite);
    }
}

void RendererViewPersistentData::Init() {
    *this = {};
}

void RendererViewPersistentData::Update(RendererView *view) {
    prev_camera = view->camera_;

    prev_G_depth = view->G_depth_;
    prev_G_normal = view->G_normal_;
    prev_G_albedo = view->G_albedo_;
    prev_G_roughness = view->G_roughness_;

}

void RendererView::InitFrame() {

    static_mesh_draw_commands_ = {};
    static_mesh_geometry_material_indices_start_index = {};

    G_depth_ = RDGTexture::CreateTexture2D(
        film_width_, film_height_, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil);

    G_albedo_ = RDGTexture::CreateTexture2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    G_normal_ = RDGTexture::CreateTexture2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    G_roughness_ = RDGTexture::CreateTexture2D(film_width_, film_height_, PixelFormatType::kR8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    // Import output backbuffer as RDG resource. We dont care about its previous usage.
    output_ = RDGTexture::Import(RHI::Get().GetBackBuffer(), RDGTextureUsageType::kDontCare);

    if (world_) {
        mi_assert(world_->GetDevice(), "Device world is not initialized upon rendering.");
        auto device_world = world_->GetDevice();
        static_mesh_geometry_material_indices = RDGBuffer::Import(device_world->static_mesh_renderable_materials_.GetHeapBuffer(0));
    }

    // Initialize the upload context used for batching uploads
    upload_context_.Init();

    temp_allocator_.Reset();
}



MI_NAMESPACE_END