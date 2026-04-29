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
#include "renderer/r_persistent.h"
#include "renderer/r_view_common.h"
#include "renderer/r_volume_primitives.h"
#include "renderer/r_denoiser.h"
#include "renderer/r_world_radiance_cache.h"
#include "renderer/r_light_structure.h"
#include "renderer/r_geometry_buffer.h"
#include "renderer/r_debug.h"
#include "renderer/r_diffuse_direct_lighting.h"
#include "renderer/r_diffuse_indirect_lighting.h"
#include "renderer/r_volume_direct_lighting.h"
#include "renderer/r_volume_indirect_lighting.h"
#include "renderer/r_volume_grid_direct_lighting.h"
#include "renderer/r_gaussian_radiance_field.h"
#include "renderer/mi_noise.h"

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
    if (!extra_barriers_.empty() && buffer == extra_barriers_.back()) return;
    // Validation
    mi_assert(buffer != nullptr, "Buffer is null.");
    // Filter naive duplicates
    extra_barriers_.push_back(buffer);
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
        pass->AddBuffer(e, RHIGPUAccessFlagBits::kTransferWrite, RHIPipelineStageFlagBits::kTransfer);
    }
    // Add extra barriers
    std::sort(extra_barriers_.begin(), extra_barriers_.end());
    extra_barriers_.erase(std::unique(extra_barriers_.begin(), extra_barriers_.end()), extra_barriers_.end());
    for (auto e : extra_barriers_) {
        pass->AddBufferH(e, RHIGPUAccessFlagBits::kTransferWrite);
    }
}

RendererViewPersistentData::RendererViewPersistentData() {

}

RendererViewPersistentData::~RendererViewPersistentData() {

}

RendererView::RendererView() {

}

RendererView::~RendererView() {
    if (persistent_data_) {
        delete persistent_data_;
    }
}

void RendererViewPersistentData::Init() {
    *this = {};
}

void RendererViewPersistentData::FinalUpdate(RendererView *view) {
    prev_camera = view->camera_;
    prev_camera_parameters_ = view->view_common_params_->Camera;
    prev_camera_jitter_ = glm::vec2(view->view_common_params_->Camera.Jitter.x, view->view_common_params_->Camera.Jitter.y);

    prev_radiance_ = view->radiance_;
    prev_taa_radiance_ = view->taa_radiance_;
    prev_shaded_radiance_no_emission_ = view->shaded_radiance_no_emission_;
    prev_shaded_volume_radiance_ = view->shaded_volume_radiance_;
    if (prev_radiance_) prev_radiance_->SetExport();
    if (prev_taa_radiance_) prev_taa_radiance_->SetExport();
    if (prev_shaded_radiance_no_emission_) prev_shaded_radiance_no_emission_->SetExport();
    if (prev_shaded_volume_radiance_) prev_shaded_volume_radiance_->SetExport();

    prev_scene_ = view->scene_;

    if (g_buffer_data_)
        g_buffer_data_->FinalUpdate(view);
    if (volume_primitives_view_persistent_data_)
        volume_primitives_view_persistent_data_->FinalUpdate(view);
    if (denoiser_persistent_data_)
        denoiser_persistent_data_->FinalUpdate(view);
    if (diffuse_indirect_lighting_persistent_data_)
        diffuse_indirect_lighting_persistent_data_->FinalUpdate(view);
    if (volume_indirect_lighting_persistent_data_)
        volume_indirect_lighting_persistent_data_->FinalUpdate(view);
    if (light_structure_persistent_data_)
        light_structure_persistent_data_->FinalUpdate(view);
    if (hash_grid_persistent_data_)
        hash_grid_persistent_data_->FinalUpdate(view);

    frame_index_ ++;
}


void RendererView::InitFrame () {

    // Update persistent data first
    if (persistent_data_ == nullptr) {
        // Create persistent data and initialize it.
        persistent_data_ = new RendererViewPersistentData();
        persistent_data_->Init();
    }

    view_common_params_ = {};
    debug_common_params_ = {};

    forward_depth_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kTransferDst | RHITextureUsageFlagBits::kTransferSrc);
    forward_depth_->SetName("Forward Depth");

    shadow_map_moments_ = RDGTexture::Create2D(
        Renderer::kDefaultShadowMapResolution, Renderer::kDefaultShadowMapResolution,
        PixelFormatType::kR32G32_FLOAT,
        RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer
    );
    shadow_map_moments_->SetName("Shadow Map Moments");

    radiance_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    radiance_->SetName("Radiance");

    taa_radiance_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransfer);
    taa_radiance_->SetName("TAARadiance");

    overlay_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
    overlay_->SetName("Overlay");

    shaded_radiance_no_emission_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT
    );
    shaded_radiance_no_emission_->SetName("Shaded Radiance No Emission");

    shaded_volume_radiance_ = RDGTexture::Create2D(
        film_width_, film_height_, PixelFormatType::kR16G16B16A16_FLOAT
    );
    shaded_volume_radiance_->SetName("Shaded Volume Radiance");

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
    did_render_path_tracing_this_frame_ = false;
}


void RendererView::SetupViewCommonShaderParameters(RenderGraphBuilder &builder) {

    view_common_params_ = builder.Allocate<ViewCommonShaderParameters>();
    view_common_params_->PreviousCamera = persistent_data_->prev_camera_parameters_;

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
    camera.FilmPixelWorldSize = {FilmViewportWorldWidth / float(film_width_),
        FilmViewportWorldHeight / float(film_height_)};

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

    // Generate subpixel jitter using a small Halton sequence and wrap by film dimensions.
    bool taa_enabled = CVarRegistry::GetInstance().GetCVar<bool>("r.postprocessing.enable_taa")->Get(); // <-- Very freestyle coding. Fix later. (I should gather all shared CVars in one header)
    if (taa_enabled){
        // Halton bases 2,3 with frame index offset to avoid 0.
        auto halton = NoiseHelpers::GenerateHaltonSequence2D(8, 2, 3)[persistent_data_->frame_index_ % 8];
        float jitter_x = 2 * (halton.x - 0.5f) / float(film_width_);
        float jitter_y = 2 * (halton.y - 0.5f) / float(film_height_);
        camera_jitter_ = glm::vec2{jitter_x, jitter_y};
    } else {
        camera_jitter_ = {};
    }

    glm::mat4 view_matrix = glm::lookAt(
        camera_.position, camera_.position + camera_.direction, camera_.up
    );
    glm::mat4 proj_matrix = glm::perspectiveRH_ZO(
        camera_.fov_Y, float(film_width_) / float(film_height_), camera_.near_plane, camera_.far_plane
    );
    // Apply jitter to projection (shift the projection center).
    proj_matrix[2][0] += - camera_jitter_.x;
    proj_matrix[2][1] += - camera_jitter_.y;
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
        // Apply prev jitter to projection (shift the projection center).
        prev_camera_proj_matrix[2][0] += - persistent_data_->prev_camera_jitter_.x;
        prev_camera_proj_matrix[2][1] += - persistent_data_->prev_camera_jitter_.y;
        auto PrevWorldToNDC = prev_camera_proj_matrix * prev_camera_view_matrix;
        camera.Reprojection = glm::mat4(PrevWorldToNDC * glm::inverse(glm::dmat4(camera.WorldToNDC)));
    }
    // Reverse the Z axis ([0, 1] -> [1, 0]) in the projection matrix
    auto proj_matrix_reversed_z = proj_matrix;
    proj_matrix_reversed_z[2][2] = camera_.near_plane / (camera_.far_plane - camera_.near_plane);
    proj_matrix_reversed_z[3][2] = camera_.far_plane * camera_.near_plane / (camera_.far_plane - camera_.near_plane);

    camera.WorldToNDC_ReversedZ = proj_matrix_reversed_z * view_matrix;
    camera.ViewToNDC_ReversedZ = proj_matrix_reversed_z;

    // Store jitter values in camera params for shaders.
    camera.Jitter = camera_jitter_;
    camera.PrevJitter = persistent_data_->prev_camera_jitter_;

    view_common_params_->FrameIndex = persistent_data_->frame_index_;
}

CVar<int> CVar_DebugCursorScreenCoordsX("r.debug.cursor_screen_coords_x", "Debug cursor X in screen coords.", 0);
CVar<int> CVar_DebugCursorScreenCoordsY("r.debug.cursor_screen_coords_y", "Debug cursor Y in screen coords.", 0);

void RendererView::SetupDebugCommonShaderParameters(RenderGraphBuilder &builder) {
    debug_common_params_ = builder.Allocate<DebugCommonShaderParameters>();
    debug_common_params_->CursorScreenCoords.x = CVar_DebugCursorScreenCoordsX.Get();
    debug_common_params_->CursorScreenCoords.y = CVar_DebugCursorScreenCoordsY.Get();
    // TODO
    debug_common_params_->CursorButtonState = 0;
}

void RendererView::DebugBuffers::CreateTracedRayBuffers(RenderGraphBuilder &builder, uint32_t max_num_rays) {
    traced_ray_count = builder.CreateBuffer<uint32_t>(RHIBufferUsageFlagBits::kStorage);
    traced_ray_count->SetName("Debug_TracedRaysCount");
    traced_ray_origins = builder.CreateBuffer<glm::vec3>(RHIBufferUsageFlagBits::kStorage, max_num_rays);
    traced_ray_directions = builder.CreateBuffer<glm::vec3>(RHIBufferUsageFlagBits::kStorage, max_num_rays);
    traced_ray_directions->SetName("Debug_TracedRayDirections");
    traced_ray_states = builder.CreateBuffer<uint32_t>(RHIBufferUsageFlagBits::kStorage, max_num_rays);
    traced_ray_states->SetName("Debug_TracedRayStates");
}

void RendererView::DebugBuffers::CreateVisualizeSpatialPositionsBuffers(RenderGraphBuilder &builder, uint32_t max_num_positions) {
    visualize_spatial_positions_count = builder.CreateBuffer<uint32_t>();
    visualize_spatial_positions_count->SetName("VisualizeSpatialPositionsCount");
    visualize_spatial_positions = builder.CreateBuffer<glm::vec3>(RHIBufferUsageFlagBits::kStorage, max_num_positions);
    visualize_spatial_positions->SetName("VisualizeSpatialPositions");
}

MI_NAMESPACE_END
