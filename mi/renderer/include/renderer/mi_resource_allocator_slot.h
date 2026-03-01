/*
 * Created: 2026/1/30
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_MI_RESOURCE_ALLOCATOR_SLOT_H
#define MI_MI_RESOURCE_ALLOCATOR_SLOT_H

#include <renderer/mi_renderer_fwd.h>
#include <renderer/mi_delayed_destruction.h>
MI_NAMESPACE_BEGIN

// Keeper for bindless resource slots allocated by DeviceBindlessResourceAllocator.
class DeviceBindlessResourceSlotKeeper : public TDelayedReleaseKeeper<DeviceBindlessResourceAllocator> {
    using TBase = TDelayedReleaseKeeper<DeviceBindlessResourceAllocator>;
public:
    using TBase::TBase;
};

MI_NAMESPACE_END
#endif //MI_MI_RESOURCE_ALLOCATOR_SLOT_H