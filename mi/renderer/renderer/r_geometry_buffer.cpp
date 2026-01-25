/*
 * Created: 2025/11/30
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <rdg/rdg_builder.h>
#include <renderer/mi_renderer_view.h>
#include "../include/renderer/r_geometry_buffer.h"
MI_NAMESPACE_BEGIN

GeometryBufferData::GeometryBufferData() {

}

GeometryBufferData::~GeometryBufferData() {

}


void GeometryBufferData::Allocate([[maybe_unused]] RenderGraphBuilder &builder, RendererView * view) {
    auto width = view->film_width_;
    auto height = view->film_height_;
    // Guard against zero-sized views to avoid creating invalid textures.
    if (width == 0 || height == 0) {
        mi_warning(true, "GeometryBufferData::Allocate received zero-sized view ({}x{}). Skipping allocation.", width, height);
        return;
    }

    G_depth_ = RDGTexture::Create2D(
        width, height, PixelFormatType::kD32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kDepthStencil | RHITextureUsageFlagBits::kTransfer);
    G_depth_->SetName("GBuffer Depth");

    G_visibility_ = RDGTexture::Create2D(width, height, PixelFormatType::kR32G32B32A32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferSrc);
    G_visibility_->SetName("GBuffer Visibility");

    G_albedo_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransfer);
    G_albedo_->SetName("GBuffer Albedo");

    G_normal_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8B8A8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_normal_->SetName("GBuffer Normal");

    G_geometry_normal_ = RDGTexture::Create2D(width, height, PixelFormatType::kR32_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_geometry_normal_->SetName("GBuffer GeometryNormal");

    G_emission_ = RDGTexture::Create2D(width, height, PixelFormatType::kR16G16B16A16_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_emission_->SetName("GBuffer Emission");

    G_metallic_roughness_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8G8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_metallic_roughness_->SetName("GBuffer Metallic Roughness");

    G_motion_vector_ = RDGTexture::Create2D(width, height, PixelFormatType::kR32G32_FLOAT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_motion_vector_->SetName("GBuffer MotionVector");

    G_flags_ = RDGTexture::Create2D(width, height, PixelFormatType::kR8_UINT,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        |RHITextureUsageFlagBits::kRenderTarget | RHITextureUsageFlagBits::kTransferDst);
    G_flags_->SetName("GBuffer Flags");

    G_transmittance_ = RDGTexture::Create2D(
        width, height, PixelFormatType::kR8_UNORM,
        RHITextureUsageFlagBits::kShaderResource | RHITextureUsageFlagBits::kUnorderedAccess
        | RHITextureUsageFlagBits::kTransferDst);
    G_transmittance_->SetName("GBuffer Transmittance");
}

GeometryBufferPersistentData::GeometryBufferPersistentData() {

}

GeometryBufferPersistentData::~GeometryBufferPersistentData() {

}

bool GeometryBufferPersistentData::MakeSureExists([[maybe_unused]] RendererView *view, [[maybe_unused]] RenderGraphBuilder &builder) {
    bool flag = false;
    // Not necessary to create prev buffers if they do not exist.
    // Noticing that passing null resources to shader fallbacks to a default and safe behavior.
    if (!prev_G_depth_) flag = true;
    if (!prev_G_normal_) flag = true;
    if (!prev_G_transmittance_) flag = true;
    if (!prev_G_motion_vector_) flag = true;
    return flag;
}

void GeometryBufferPersistentData::FinalUpdate(RendererView *view) {
    prev_G_depth_ = view->g_buffer_->G_depth_;
    prev_G_depth_->SetName("PrevGDepth");
    prev_G_depth_->SetExport();

    prev_G_normal_ = view->g_buffer_->G_normal_;
    prev_G_normal_->SetName("PrevGNormal");
    prev_G_normal_->SetExport();

    prev_G_geometry_normal_ = view->g_buffer_->G_geometry_normal_;
    prev_G_geometry_normal_->SetName("PrevGGeometryNormal");
    prev_G_geometry_normal_->SetExport();

    prev_G_transmittance_ = view->g_buffer_->G_transmittance_;
    prev_G_transmittance_->SetName("PrevGTransmittance");
    prev_G_transmittance_->SetExport();

    prev_G_motion_vector_ = view->g_buffer_->G_motion_vector_;
    prev_G_motion_vector_->SetName("PrevGMotionVector");
    prev_G_motion_vector_->SetExport();

}


MI_NAMESPACE_END