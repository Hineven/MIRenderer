/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_AS_H
#define RHI_AS_H

#include <rhi/rhi_resource.h>
#include <rhi/rhi_buffer.h>
#include <rhi/rhi_types.h>
#include <rhi/rhi_as_types.h>
#include <vector>

MI_NAMESPACE_BEGIN

class RHIAccelerationStructure : public RHIResource {
public:
    RHIAccelerationStructure(RHIAccelerationStructureType type);
    virtual ~RHIAccelerationStructure();

    RHIAccelerationStructureType GetType() const { return type_; }

    // Get the device address of this acceleration structure (for instance references)
    virtual uint64_t GetDeviceAddress() const = 0;

    // Get the build sizes information for the given build info
    // If you are building a TLAS, you should not fill the geometries field.
    virtual RHIAccelerationStructureBuildSizesInfo GetBuildSizes(
        const RHIAccelerationStructureBuildGeometryInfo& build_info) const = 0;

    // Create acceleration structure with given size (which you should firt query with GetBuildSizes)
    virtual bool Create(size_t size) = 0;

protected:
    RHIAccelerationStructureType type_;
};

MI_NAMESPACE_END

#endif //RHI_AS_H
