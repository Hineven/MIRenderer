/*
 * Created: 2024/9/15
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_RDG_RESOURCE_H
#define MI_RDG_RESOURCE_H
#include "rdg/rdg.h"
#include "rhi/rhi_resource.h"
#include "core/pixel_format.h"

MI_NAMESPACE_BEGIN

struct RDGTextureDesc {
    int width, height, depth;
    int mip_levels;
    int array_layers;
    PixelFormatType format;
};

class RDGTextureResource : public RDGResource {
public:
    RDGTextureResource (RDGTextureDesc desc) : RDGResource(nullptr), desc_(desc) {}
    ~RDGTextureResource () override = default;

    RDGTextureDesc GetDesc () const { return desc_; }
protected:
    RDGTextureDesc desc_;
};

MI_NAMESPACE_END
#endif //MI_RDG_RESOURCE_H
