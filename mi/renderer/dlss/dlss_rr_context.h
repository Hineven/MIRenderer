/*
 * Created: 2026/05/11
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_DLSS_RR_CONTEXT_H
#define MI_DLSS_RR_CONTEXT_H

#include "core/common.h"
#include "core/refcounted.h"
#include "rhi/rhi_fwd.h"
#include "rhi/rhi_desc.h"
#include "rdg/rdg_fwd.h"

MI_NAMESPACE_BEGIN

class NGXContext;

// Lightweight snapshot of Vulkan resource handles for DLSS-RR Evaluate.
// Must be extracted on the Render Thread (where RDG resources are accessible),
// then passed into the RHI thread lambda for Evaluate.
struct DLSSRRResourceSnapshot {
    struct Res {
        void * image_view = nullptr;
        void * image = nullptr;
        uint32_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };
    Res color, depth, mv, albedo, spec_albedo, normal, roughness, alpha, output;
};

// Per-view DLSS Ray Reconstruction feature context.
// Bound to RendererViewPersistentData so that each view owns its own
// NGX feature handle, parameters, and auxiliary textures.
class DLSSRRContext : public RefCounted<> {
public:
    DLSSRRContext();
    ~DLSSRRContext();

    // Initialize the per-view feature. Requires the global NGXContext to be initialized.
    bool Initialize(NGXContext * ngx, uint32_t width, uint32_t height);
    void Shutdown();

    bool SetResolution(uint32_t width, uint32_t height);
    void ReleaseResolution();

    FORCEINLINE bool IsAvailable() const { return is_available_; }

    // Build a snapshot of Vulkan resource handles on the Render Thread.
    // The snapshot is safe to pass into RHI thread lambdas.
    DLSSRRResourceSnapshot BuildResourceSnapshot(
        RDGTexture * noisy_radiance,
        RDGTexture * depth,
        RDGTexture * normal,
        RDGTexture * motion_vector,
        RDGTexture * albedo,
        RDGTexture * specular_albedo,
        RDGTexture * roughness,
        RDGTexture * alpha,
        RDGTexture * output);

    // IMPORTANT: This function runs on the RHI thread (inside RHIExecute).
    // Resource handles must be pre-extracted on the Render Thread via BuildResourceSnapshot.
    void Evaluate_RHI(const DLSSRRResourceSnapshot & resources,
                  float camera_jitter_x,
                  float camera_jitter_y,
                  bool reset_history,
                  const float * world_to_view_matrix = nullptr,
                  const float * view_to_clip_matrix = nullptr);

    void OnResolutionChanged(uint32_t width, uint32_t height);
    FORCEINLINE bool IsResolutionChanged(uint32_t width, uint32_t height) const {
        return width != render_width_ || height != render_height_;
    }

    FORCEINLINE RDGTexture * GetDLSSOutput() const { return dlss_output_.Raw(); }
    FORCEINLINE RDGTexture * GetDepthBuffer() const { return pt_depth_.Raw(); }
    FORCEINLINE RDGTexture * GetNormalBuffer() const { return pt_normal_.Raw(); }
    FORCEINLINE RDGTexture * GetMotionVectorBuffer() const { return pt_motion_vector_.Raw(); }
    FORCEINLINE RDGTexture * GetAlbedoBuffer() const { return pt_albedo_.Raw(); }
    FORCEINLINE RDGTexture * GetSpecularAlbedoBuffer() const { return pt_specular_albedo_.Raw(); }
    FORCEINLINE RDGTexture * GetRoughnessBuffer() const { return pt_roughness_.Raw(); }
    FORCEINLINE RDGTexture * GetAlphaBuffer() const { return pt_alpha_.Raw(); }

private:
    void CreateAuxiliaryTextures(uint32_t width, uint32_t height);

    bool is_available_ = false;
    uint32_t render_width_ = 0;
    uint32_t render_height_ = 0;

    void * ngx_feature_ = nullptr;
    void * ngx_parameters_ = nullptr;

    TRef<RDGTexture> dlss_output_;
    TRef<RDGTexture> pt_depth_;
    TRef<RDGTexture> pt_normal_;
    TRef<RDGTexture> pt_motion_vector_;
    TRef<RDGTexture> pt_albedo_;
    TRef<RDGTexture> pt_specular_albedo_;
    TRef<RDGTexture> pt_roughness_;
    TRef<RDGTexture> pt_alpha_;
};

MI_NAMESPACE_END

#endif
