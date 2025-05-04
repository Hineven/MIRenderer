/*
 * Created: 2025/5/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef RHI_AS_H
#define RHI_AS_H

#include <rhi/rhi_resource.h>

MI_NAMESPACE_BEGIN

class RHIAccelerationStructure : public RHIResource {
public:
    RHIAccelerationStructure() ;
    virtual ~RHIAccelerationStructure() ;
protected:
    // TODO
};

MI_NAMESPACE_END

#endif //RHI_AS_H
