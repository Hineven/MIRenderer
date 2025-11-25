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

MI_NAMESPACE_BEGIN

// Packed representation for a single 3D Gaussian radiance point.
// Layout matches SharedGaussianRadianceField.hlsl.
struct PackedGaussianRadiancePoint {
    glm::vec3 Position; // World-space center
    uint32_t  PackedRotation_Opacity; // xyz quaternion (snorm3x8) + unorm1x8
    glm::vec3 Scales; // Principal axes scaling (pre-exponential activation applied)
};

struct GaussianRadiancePoint {
    glm::vec3 Position;
    glm::vec4 Rotation;
    glm::vec3 Scales;
    float     Opacity;
};

struct GaussianRadianceFieldHeader {
    uint32_t NumPoints;
    uint32_t PointOffset; // Offset in the uber buffer
    glm::uvec2 Padding;
};

class DeviceGaussianRadianceField : public NonCopyable, public NonMovable, public RefCounted<> {
public:
    FORCEINLINE bool IsValid() const { return index_ != UINT32_MAX; }
    FORCEINLINE uint32_t GetIndex() const { return index_; }
    FORCEINLINE uint32_t GetPointOffset() const { return (uint32_t)(point_buffer_->GetOffset() / sizeof(PackedGaussianRadiancePoint)); }
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
    void SetPoints(const std::vector<PackedGaussianRadiancePoint> & points);
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
    std::vector<PackedGaussianRadiancePoint> points_;
    AABB aabb_ {};
    bool dirty_ {true};
    bool ray_traced_ {false}; // Usually not participating in lighting; raster only by default
    bool dynamic_ {false};
    DirtyTracker<GaussianRadianceField> * tracker_ {};
    std::vector<glm::vec3> sh_coeffs_; // 16 * NumPoints (RGB) fourth-order SH coefficients (bands l=0..3)
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
