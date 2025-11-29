/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#ifndef MI_GAUSSIAN_RADIANCE_FIELD_H
#define MI_GAUSSIAN_RADIANCE_FIELD_H

#include <vector>
#include <span>
#include "core/refcounted.h"
#include "renderer/mi_renderable.h"
#include "renderer/mi_renderer_fwd.h"
#include "renderer/mi_renderer_types.h"
#include "renderer/mi_buffer_heap.h"
#include "renderer/mi_cvar.h"
#include "renderer/mi_dirty_tracker.h"
#include "../shaders/shared/SharedGaussianRadianceField.hlsl"

MI_NAMESPACE_BEGIN

class DeviceGaussianRadianceField : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    FORCEINLINE bool IsValid() const { return index_ != UINT32_MAX; }
    FORCEINLINE uint32_t GetIndex() const { return index_; }
    FORCEINLINE uint32_t GetPointOffset() const { return (uint32_t)(point_buffer_->GetOffset() / sizeof(PackedGaussian3D)); }
    FORCEINLINE RHIAccelerationStructure * GetBLAS() const { return BLAS_.Raw(); }
protected:
    DeviceGaussianRadianceField(DeviceBindlessResourceAllocator * alloc);
    ~DeviceGaussianRadianceField();
    uint32_t index_ {UINT32_MAX};
    TRef<DeviceUberBufferAllocation> point_buffer_;
    TRef<DeviceUberBufferAllocation> sh_coeff_buffer_;
    TRef<RHIAccelerationStructure> BLAS_;
    friend class GaussianRadianceField;
};

class GaussianRadianceField : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    friend class Renderer;
    static TRef<GaussianRadianceField> Create();
    void SetPoints(const std::vector<PackedGaussian3D> & points);
    void UpdateOnDevice_Async(DeviceBindlessResourceAllocator * alloc, RHICommandQueueGraphics & queue);
    void UpdateOnDevice(DeviceBindlessResourceAllocator * alloc);

    FORCEINLINE DeviceGaussianRadianceField * GetDeviceField() const { return device_field_.Raw(); }
    FORCEINLINE uint32_t GetNumPoints() const { return (uint32_t)points_.size(); }
    FORCEINLINE bool IsEmpty() const { return points_.empty(); }

    FORCEINLINE AABB GetAABB() const { return aabb_; }

    FORCEINLINE bool IsDirty() const { return dirty_; }
    void SetDirty(bool dirty = true);

    FORCEINLINE bool IsRayTraced() const { return ray_traced_; }
    void SetRayTraced(bool rt);

    void SetSHCoefficients(const std::vector<glm::vec3> & coeffs) { sh_coeffs_ = coeffs; SetDirty(); }
    const std::vector<glm::vec3> & GetSHCoefficients() const { return sh_coeffs_; }

    constexpr static uint32_t kGaussianRadianceAllocatorUberBufferIndex = 1; // Distinct from volume primitives
    constexpr static uint32_t kGaussianRadianceSHAllocatorUberBufferIndex = 2; // SH coefficients uber buffer
protected:
    static void SetupAllocatorUberBuffer(DeviceBindlessResourceAllocator * alloc);
    TRef<DeviceGaussianRadianceField> device_field_;
    std::vector<PackedGaussian3D> points_;
    AABB aabb_ {};
    bool dirty_ {true};
    bool ray_traced_ {true}; // Default: enable ray tracing for 3D Gaussian radiance fields
    bool dynamic_ {false};
    DirtyTracker<GaussianRadianceField> * tracker_ {};
    // 4th-order SH (l=0..3): 16 coefficients per point, RGB each
    std::vector<glm::vec3> sh_coeffs_;
};

class GaussianRadianceFieldInstance : public Renderable {
public:
    static TRef<GaussianRadianceFieldInstance> Create(Scene * scene, GaussianRadianceField * field, Transform transform = {});
    RenderableHeader GetDeviceRenderableHeader() const override;
    constexpr static RenderableType kRenderableType = RenderableType::kGaussianRadianceFieldInstance;
    FORCEINLINE GaussianRadianceField * GetField() const { return field_.Raw(); }
    void Update(RendererView * view, RenderGraphBuilder & builder) override;
    RHIAccelerationStructure * GetBLAS() const override;
    uint32_t GetInstanceCustomIndex() const override; // If ray traced one day, add a flag
    bool IsEmpty() const override;
protected:
    GaussianRadianceFieldInstance(Scene * scene);
    ~GaussianRadianceFieldInstance() override;
    TRef<GaussianRadianceField> field_;
};

MI_NAMESPACE_END
#endif // MI_GAUSSIAN_RADIANCE_FIELD_H
