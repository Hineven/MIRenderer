/*
 * Created: 2026/4/22
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MI_RENDERER_EXPORT_H
#define MI_MI_RENDERER_EXPORT_H

#include <core/common.h>
#include <rdg/rdg_fwd.h>
#include <rdg/rdg_resource.h>
#include <renderer/mi_renderer_fwd.h>

MI_NAMESPACE_BEGIN

// Created each frame. All exportable resources are registered when building the frame graph.
// External application can request for exportation before RDG compilation & execution via this registry.
//
// Export ID table (registered by Renderer::Render):
//   "radiance"              - Final composed radiance (R16G16B16A16_FLOAT)
//   "overlay"               - Linear color overlay for GRF / forward passes (R8G8B8A8_UNORM)
//   "depth"                 - G-buffer depth (D32_FLOAT)
//   "transmittance"         - G-buffer transmittance (R8_UNORM)
//   "visibility"            - Visibility buffer (R32G32B32A32_UINT)
//   "albedo"                - G-buffer albedo (R8G8B8A8_UNORM)
//   "normal"                - G-buffer normal (R8G8B8A8_UNORM)
//   "geometry_normal"       - G-buffer geometry normal (R32_UINT)
//   "motion_vector"         - Motion vectors (R32G32_FLOAT)
//   "volume_sample_color"   - Volume primitive sample color (R8G8B8A8_UNORM)
//   "volume_sample_linear_depth" - Volume primitive linear depth (R32_FLOAT)
//   "volume_density"        - Volume density grid (R32_FLOAT)
//   "volume_color"          - Volume color grid (R8G8B8A8_UNORM)
//   "diffuse_direct"        - Diffuse direct lighting (R16G16B16A16_FLOAT)
//   "diffuse_indirect"      - Diffuse indirect lighting (R16G16B16A16_FLOAT)
//   "volume_direct"         - Volume direct lighting (R16G16B16A16_FLOAT)
//   "volume_indirect"       - Volume indirect lighting (R16G16B16A16_FLOAT)
//   "denoised_diffuse_direct"   - Denoised diffuse direct (R16G16B16A16_FLOAT)
//   "denoised_diffuse_indirect" - Denoised diffuse indirect (R16G16B16A16_FLOAT)
//   "grf_depth"             - Gaussian radiance field depth (D32_FLOAT)
//   "grf_opacity"           - Gaussian radiance field opacity (R8_UNORM)
//   "path_tracing_film"     - Path tracing accumulation film (R32G32B32A32_FLOAT)
//
// Note: Not all IDs are guaranteed to be present every frame. Some resources are only
// created when their corresponding rendering features are enabled (e.g. volume primitives,
// denoiser, GRF, path tracing). Callers should check for nullptr returns.
class RendererExports : public RefCounted<> {
public:
    ~RendererExports() override;

    RDGResource * Get (const std::string & export_id) const;
    FORCEINLINE RDGTexture * GetTexture (const std::string & export_id) const {
        return dynamic_cast<RDGTexture*>(Get(export_id));
    }
    FORCEINLINE RDGBuffer * GetBuffer (const std::string & export_id) const {
        return dynamic_cast<RDGBuffer*>(Get(export_id));
    }

    bool RequestExport (const std::string & export_id) const;

protected:
    friend class Renderer;
    RendererExports();
    void RegisterResource (const std::string & export_id, RDGResource * resource);

    std::map<std::string, TRef<RDGResource>> exportable_resources_;
};

MI_NAMESPACE_END

#endif //MI_MI_RENDERER_EXPORT_H
