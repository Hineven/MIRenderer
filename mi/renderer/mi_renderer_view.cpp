/*
 * Created: 2025/4/19
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <ranges>
#include "renderer/mi_renderer_view.h"

#include "rdg/rdg_builder.h"
#include "rdg/rdg_pool.h"
#include "rdg/rdg_resource.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_scene.h"
#include "renderer/mi_texture.h"
#include "rhi/rhi.h"
#include "rhi/rhi_buffer.h"
#include "rhi/rhi_desc.h"
#include "renderer/r_view_common.h"

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
        manual_staging_buffer_top_ += (uint32_t)size;
        return {buffer.Raw(), offset};
    }
}

void BatchedUploadContext::Init() {
    manual_staging_buffer_ = {};
    manual_staging_buffer_top_ = {};
    manual_staging_memory_footprint_ = 0;

    pending_uploads_.clear();
    pending_rdg_uploads_.clear();
    extra_barriers_.clear();

    fired_ = false;
}

void BatchedUploadContext::AddUnsafe(RHIBufferSpan buffer, const void *data, size_t size) {
    mi_assert(!fired_, "Adding uploads after BatchedUploadContext::Fire().");
    mi_assert(buffer.size >= size, "Overflowing the destination buffer in upload.");
    PendingUpload upload = {};
    upload.dst_buffer = buffer;
    upload.data = std::span((const uint8_t*)data, size);
    pending_uploads_.emplace_back(upload);
}


void BatchedUploadContext::Add(RDGBuffer *buffer, const void *data, size_t size, size_t dst_offset) {
    mi_assert(!fired_, "Adding uploads after upload.");
    if (!size) return ;
    if (!buffer || !data) {
        mi_assert(false, "Buffer / data is null.");
    }
    PendingRDGUpload upload = {};
    upload.dst_buffer = buffer;
    upload.data = std::span((const uint8_t*)data, size);
    upload.dst_offset = dst_offset;
    pending_rdg_uploads_.emplace_back(upload);
}

void BatchedUploadContext::AddExtraBarrier(RDGBuffer *buffer) {
    // Filter naive duplicates
    if (!extra_barriers_.empty() && buffer == extra_barriers_.back()) return;
    extra_barriers_.push_back(buffer);
    // Validation
    mi_assert(buffer != nullptr, "Buffer is null.");
}


void BatchedUploadContext::Fire(RenderGraphBuilder &builder) {
    mi_assert(!fired_, "Batched double fire.");
    fired_ = true;
    std::sort(pending_rdg_uploads_.begin(), pending_rdg_uploads_.end(),
        [](const PendingRDGUpload & a, const PendingRDGUpload & b) {
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

    auto staging_buffer = sum_upload_size ? RHI::Get().CreateBuffer(sum_upload_size, RHIBufferUsageFlagBits::kStaging) : nullptr;
    auto staging_ptr = staging_buffer ? staging_buffer->Map() : nullptr;

    if (staging_ptr) {
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

#ifndef NDEBUG
    {
        // Validate that there are no overlapping uploads
        std::sort(pending_uploads_.begin(), pending_uploads_.end(),
            [](const PendingUpload & a, const PendingUpload & b) {
                if (a.dst_buffer.buffer == b.dst_buffer.buffer) {
                    if (a.dst_buffer.offset == b.dst_buffer.offset)
                        return a.data.size() < b.data.size();
                    return a.dst_buffer.offset < b.dst_buffer.offset;
                }
                return a.dst_buffer.buffer < b.dst_buffer.buffer;
            });
        int furthest_index = -1;
        RHIBufferSpan buffer {};
        for (auto [i, e] : std::views::enumerate(pending_uploads_)) {
            if (e.dst_buffer.buffer != buffer.buffer) {
                buffer = e.dst_buffer;
                buffer.size = e.data.size();
                furthest_index = (int)i;
            } else {
                if (e.dst_buffer.size > 0 && buffer.offset + buffer.size > e.dst_buffer.offset) {
                    // Overlapping uploads detected.

                    mi_assert(false, "Overlapping RHI buffer uploads detected at index {} vs {}: {} ({}, {}) vs {} ({}, {}).",
                        furthest_index, i,
                        e.dst_buffer.buffer->GetName(), e.dst_buffer.offset, e.dst_buffer.size,
                        buffer.buffer->GetName(), buffer.offset, buffer.size);
                }
                if (buffer.offset + buffer.size < e.dst_buffer.offset + e.data.size()) {
                    buffer = e.dst_buffer;
                    buffer.size = e.data.size();
                    furthest_index = (int)i;
                }
            }
        }
    }
    {
        // Validate that there are no overlapping uploads
        std::sort(pending_rdg_uploads_.begin(), pending_rdg_uploads_.end(),
            [](const PendingRDGUpload & a, const PendingRDGUpload & b) {
                if (a.dst_buffer == b.dst_buffer) {
                    if (a.dst_offset == b.dst_offset)
                        return a.data.size() < b.data.size();
                    return a.dst_offset < b.dst_offset;
                }
                return a.dst_buffer < b.dst_buffer;
            });
        int furthest_index = -1;
        RDGBuffer * buffer {};
        size_t offset = 0, size = 0;
        for (auto [i, e] : std::views::enumerate(pending_rdg_uploads_)) {
            if (e.dst_buffer != buffer) buffer = e.dst_buffer.Raw(), offset = e.dst_offset, size = e.data.size(), furthest_index = (int)i;
            else {
                if (e.data.size() > 0 && offset + size >= e.dst_offset) {
                    // Overlapping uploads detected.

                    mi_assert(false, "Overlapping RDG buffer uploads detected at index {} vs {}: {} ({}, {}) vs {} ({}, {}).",
                        furthest_index, i,
                        e.dst_buffer->GetName(), e.dst_offset, e.data.size(),
                        buffer->GetName(), offset, size);
                }
                if (offset + size < e.dst_offset + e.data.size()) {
                    buffer = e.dst_buffer.Raw();
                    offset = e.dst_offset;
                    size = e.data.size();
                    furthest_index = (int)i;
                }
            }
        }
    }
#endif

    auto pass = builder.AddPass("BatchedUploadBuffers", RDGPassType::kGeneric, {}, nullptr, nullptr,
        [staging = staging_buffer.Raw(), pending_rhi = std::move(pending_uploads_), pending_rdg = std::move(pending_rdg_uploads_)](
            [[maybe_unused]] RDGPass *pass, RHICommandQueueGraphics & queue
        ) {
            size_t offset = 0;
            for (const auto & e : pending_rhi) {
                auto size = e.data.size();
                auto rounded_size = RoundUp(size, 16);
                auto dst_span = RHIBufferSpan{e.dst_buffer.buffer, e.dst_buffer.offset, size};
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
    // Add extra barriers
    std::sort(extra_barriers_.begin(), extra_barriers_.end());
    extra_barriers_.erase(std::unique(extra_barriers_.begin(), extra_barriers_.end()), extra_barriers_.end());
    for (auto e : extra_barriers_) {
        pass->AddBuffer(e, RHIGPUAccessFlagBits::kWrite);
    }
}

RendererViewPersistentData::RendererViewPersistentData() {

}

RendererViewPersistentData::~RendererViewPersistentData() {

}



RendererView::RendererView() {

}

RendererView::~RendererView() {

}


void RendererViewPersistentData::Init() {
    *this = {};
}

void RendererViewPersistentData::Update(RendererView *view) {
    prev_camera = view->camera_;

    prev_G_depth = view->G_depth_;
    prev_G_normal = view->G_normal_;
    prev_G_albedo = view->G_albedo_;
    prev_G_roughness = view->G_metallic_roughness_;

    prev_scene_ = view->scene_;

    frame_index_ ++;
}


void RendererView::InitFrame () {

    // Update persistent data first
    if (persistent_data_ == nullptr) {
        // Create persistent data and initialize it.
        auto persistent = new RendererViewPersistentData();
        persistent_data_.reset(persistent);
        persistent->Init();
    }

    static_mesh_geometry_material_indices_start_index = {};

    G_depth_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kTransferDst);

    G_albedo_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    G_normal_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    G_metallic_roughness_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget);

    G_volume_density_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_min_max_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_color_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);

    // Clear debug output texture
    debug_output_ = {};

    // bool world_changed = world_ != persistent_data_->prev_world_;
    // Initialize the upload context used for batching uploads
    upload_context_.Init();

    temp_allocator_.Reset();
}


void RendererView::UpdatePersistentData () {
    // Roll states for the next frame
    persistent_data_->Update(this);
}

void RendererView::SetViewCommonShaderParameters(RenderGraphBuilder &builder) {
    view_common_params_ = builder.Allocate<ViewCommonShaderParameters>();
    auto & camera = view_common_params_->Camera;

    camera.Position = camera_.position;
    {
        glm::vec3 camera_right = camera_.GetRight();
        glm::vec3 camera_up = glm::normalize(glm::cross(camera_right, camera_.direction));
        auto aspect = (double)film_width_ / film_height_;
        auto tan_fov_y = tan(camera_.fov_Y / 2.0);
        // auto two_tan_fov_y = float(tan_fov_y * 2.0);

        glm::vec3 axis_forward = glm::normalize(camera_.direction);
        // Camera forward is -z axis
        glm::vec3 axis_right = glm::normalize(glm::cross(axis_forward, camera_.up));
        glm::vec3 axis_up = glm::normalize(glm::cross(axis_right, axis_forward));
        // Thus, normalize(axis_forward + axis_right * ndc.x + axis_up * ndc.y) is the camera ray direction
        axis_up    *= tan_fov_y;
        axis_right *= tan_fov_y * aspect;

        camera.Direction = glm::normalize(camera_.direction);
        camera.Right = axis_right;
        camera.Up = axis_up;
    }
    camera.NearPlane = camera_.near_plane;
    camera.FarPlane = camera_.far_plane;
    camera.FoVY = camera_.fov_Y;
    camera.FilmDimensions = {film_width_, film_height_};

    float aspect_ratio = float(film_width_) / float(film_height_);
    camera.FilmAspectRatioAndInvAspectRatio = {aspect_ratio, 1.0f / aspect_ratio};

    glm::mat4 view_matrix = glm::lookAt(
        camera_.position, camera_.position + camera_.direction, camera_.up
    );
    glm::mat4 proj_matrix = glm::perspective(
        camera_.fov_Y, float(film_width_) / float(film_height_), camera_.near_plane, camera_.far_plane
    );
    camera.WorldToNDC = proj_matrix * view_matrix;
    camera.WorldToView = view_matrix;
    camera.ViewToNDC = proj_matrix;
}

MI_NAMESPACE_END