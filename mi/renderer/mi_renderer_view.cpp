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
#include "renderer/mi_cvar.h"
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

    auto pass = builder.AddPass("BatchedUploadBuffers", RDGPassType::kGeneric, {}, {}, nullptr, nullptr,
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
        pass->AddBufferH(e, RHIGPUAccessFlagBits::kWrite);
    }
    // Add extra barriers
    std::sort(extra_barriers_.begin(), extra_barriers_.end());
    extra_barriers_.erase(std::unique(extra_barriers_.begin(), extra_barriers_.end()), extra_barriers_.end());
    for (auto e : extra_barriers_) {
        pass->AddBufferH(e, RHIGPUAccessFlagBits::kWrite);
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

    prev_radiance_ = view->radiance_;

    prev_scene_ = view->scene_;

    frame_index_ ++;
}


void RendererView::InitFrame () {

    // Update persistent data first
    if (persistent_data_ == nullptr) {
        // Create persistent data and initialize it.
        persistent_data_ = std::make_unique<RendererViewPersistentData>();
        persistent_data_->Init();
    }

    view_common_params_ = {};
    debug_common_params_ = {};

    static_mesh_geometry_material_indices_start_index = {};

    G_depth_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kTransferDst);
    G_depth_->SetName("GBuffer Depth");
    // Keep history for next frame
    G_depth_->SetExport();

    forward_depth_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kTransferDst | RHITextureUsageFlagBits::kTransferSrc);
    forward_depth_->SetName("Forward Depth");

    G_visibility_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR32G32B32A32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferSrc);
    G_visibility_->SetName("GBuffer Visibility");

    G_albedo_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
    G_albedo_->SetName("GBuffer Albedo");

    G_normal_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_normal_->SetName("GBuffer Normal");
    // Keep history for next frame
    G_normal_->SetExport(true);

    G_emission_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_emission_->SetName("GBuffer Emission");

    G_metallic_roughness_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8G8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_metallic_roughness_->SetName("GBuffer Metallic Roughness");

    G_flags_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR8_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);

    G_volume_min_max_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_min_max_->SetName("GBuffer Volume Min Max");
    G_volume_density_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_density_->SetName("GBuffer Volume Density");
    G_volume_color_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_color_->SetName("GBuffer Volume Color");
    G_volume_density_fourier_ = RDGTexture::Create2DArray(
        film_width_, film_height_, 3, PixelFormatType::kR32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_density_fourier_->SetName("GBuffer Volume Density Fourier");
    G_volume_weighted_color_fourier_ = RDGTexture::Create2DArray(
        film_width_, film_height_, 3, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_weighted_color_fourier_->SetName("GBuffer Volume Weighted Color Fourier");
    G_volume_cdf_attenuation_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    G_volume_cdf_attenuation_->SetName("GBuffer Volume CDF Attenuation");

    G_transmittance_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransferDst);
    G_transmittance_->SetName("GBuffer Transmittance");

    shadow_map_moments_ = RDGTexture::Create2D(
        Renderer::kDefaultShadowMapResolution, Renderer::kDefaultShadowMapResolution,
        PixelFormatType::kR32G32_FLOAT,
        RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer
    );
    shadow_map_moments_->SetName("Shadow Map Moments");

    volume_sample_color_and_linear_depth_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    volume_sample_color_and_linear_depth_->SetName("Volume Sample Color and Linear Depth");
    volume_sample_transmittance_and_pdf_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess);
    volume_sample_transmittance_and_pdf_->SetName("Volume Sample Transmittance and PDF");

    radiance_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    radiance_->SetName("Radiance");
    // Keep history for next frame
    radiance_->SetExport();

    diffuse_direct_lighting_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    diffuse_direct_lighting_->SetName("Diffuse Direct Lighting");
    volume_direct_lighting_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    volume_direct_lighting_->SetName("Volume Direct Lighting");

    debug_output_ = RDGTexture::Create2D(film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kUnorderedAccess | RHITextureUsageFlagBits::kRenderTarget
        | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kTransfer);
    debug_output_->SetName("Debug Output");

    // Clear hzb, this is later created
    hzb_ = {};
    or_flags_ = {};

    // clear debug buffers. They are created on demand.
    debug_buffers_ = {};
    // Also clear debug views
    debug_views_ = {};

    // bool world_changed = world_ != persistent_data_->prev_world_;
    // Initialize the upload context used for batching uploads
    upload_context_.Init();

    temp_allocator_.Reset();
}


void RendererView::UpdatePersistentData () {
    // Roll states for the next frame
    persistent_data_->Update(this);
}

void RendererView::SetupViewCommonShaderParameters(RenderGraphBuilder &builder) {
    view_common_params_ = builder.Allocate<ViewCommonShaderParameters>();
    auto & camera = view_common_params_->Camera;

    camera.Position = camera_.position;
    float aspect_ratio = float(film_width_) / float(film_height_);
    {
        glm::vec3 camera_right = camera_.GetRight();
        glm::vec3 camera_up = glm::normalize(glm::cross(camera_right, camera_.direction));

        camera.Direction = glm::normalize(camera_.direction);
        camera.Right = camera_.GetScaledRight(aspect_ratio);
        camera.Up = camera_.GetScaledUp();

        camera.NormalizedRight = glm::normalize(camera_right);
        camera.NormalizedUp = glm::normalize(camera_up);
    }

    camera.NearPlane = camera_.near_plane;
    camera.FarPlane = camera_.far_plane;
    camera.FoVY = camera_.fov_Y;
    camera.TanFoVY_2 = tan(camera_.fov_Y / 2.0f);
    camera.TanFoVY = camera.TanFoVY_2 * 2.f;

    camera.Type = 0;

    camera.FilmDimensions = {film_width_, film_height_};

    camera.FilmAspectRatioAndInvAspectRatio = {aspect_ratio, 1.0f / aspect_ratio};

    uint32_t hzb_size = 1;
    while (hzb_size < film_width_ || hzb_size < film_height_) {
        hzb_size *= 2;
    }
    hzb_size /= 2;
    camera.HZBDimensions = glm::uvec2(hzb_size);
    float FilmViewportWorldHeight = 1;
    float FilmViewportWorldWidth = FilmViewportWorldHeight * aspect_ratio;
    camera.FilmPixelWorldSize = {1.0f / (FilmViewportWorldWidth * film_width_),
        1.0f / (FilmViewportWorldHeight * film_height_)};

    camera.InvFilmDimensions = {1.0f / float(film_width_), 1.0f / float(film_height_)};
    camera.UVToHZBScale = {
        (float)film_width_ / (2 * (float)hzb_size), (float)film_height_ / (2 * (float)hzb_size)
    };

    camera.HZBBaseTexelSize = {
        1.0f / (float)hzb_size, 1.0f / (float)hzb_size
    };
    camera.HZBToUVScale = {
        2 * (float)hzb_size / (float)film_width_, 2 * (float)hzb_size / (float)film_height_
    };

    glm::mat4 view_matrix = glm::lookAt(
        camera_.position, camera_.position + camera_.direction, camera_.up
    );
    glm::mat4 proj_matrix = glm::perspectiveRH_ZO(
        camera_.fov_Y, float(film_width_) / float(film_height_), camera_.near_plane, camera_.far_plane
    );
    camera.WorldToNDC = proj_matrix * view_matrix;
    camera.WorldToView = view_matrix;
    camera.ViewToNDC = proj_matrix;


    {
        glm::dmat4 prev_camera_view_matrix = glm::lookAt(
            persistent_data_->prev_camera.position, persistent_data_->prev_camera.position + persistent_data_->prev_camera.direction,
            persistent_data_->prev_camera.up
        );
        glm::dmat4 prev_camera_proj_matrix = glm::perspectiveRH_ZO(
            persistent_data_->prev_camera.fov_Y, float(film_width_) / float(film_height_),
            persistent_data_->prev_camera.near_plane, persistent_data_->prev_camera.far_plane
        );
        auto PrevWorldToNDC = prev_camera_proj_matrix * prev_camera_view_matrix;
        camera.Reprojection = glm::mat4(PrevWorldToNDC * glm::inverse(glm::dmat4(camera.WorldToNDC)));
    }
    // Reverse the Z axis ([0, 1] -> [1, 0]) in the projection matrix
    auto proj_matrix_reversed_z = proj_matrix;
    proj_matrix_reversed_z[2][2] = camera_.near_plane / (camera_.far_plane - camera_.near_plane);
    proj_matrix_reversed_z[3][2] = camera_.far_plane * camera_.near_plane / (camera_.far_plane - camera_.near_plane);

    camera.WorldToNDC_ReversedZ = proj_matrix_reversed_z * view_matrix;
    camera.ViewToNDC_ReversedZ = proj_matrix_reversed_z;
}

CVar<int> CVar_DebugCursorScreenCoordsX("debug.cursor_screen_coords_x", "Debug cursor X in screen coords.", 0);
CVar<int> CVar_DebugCursorScreenCoordsY("debug.cursor_screen_coords_y", "Debug cursor Y in screen coords.", 0);

void RendererView::SetupDebugCommonShaderParameters(RenderGraphBuilder &builder) {
    debug_common_params_ = builder.Allocate<DebugCommonShaderParameters>();
    debug_common_params_->CursorScreenCoords.x = CVar_DebugCursorScreenCoordsX.Get();
    debug_common_params_->CursorScreenCoords.y = CVar_DebugCursorScreenCoordsY.Get();
    // TODO
    debug_common_params_->CursorButtonState = 0;
}

MI_NAMESPACE_END