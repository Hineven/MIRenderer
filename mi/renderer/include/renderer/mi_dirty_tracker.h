/*
 * Created: 2025/7/18
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_DIRTY_TRACKER_H
#define MI_DIRTY_TRACKER_H

#include <vector>
#include <core/base.h>
#include <core/refcounted.h>
#include <renderer/mi_renderer_fwd.h>

#include "core/util/queue.h"

MI_NAMESPACE_BEGIN
// A class to track dirty state of non-renderer owned host resources.
// Geometries, materials, static meshes can be associated with the tracker.
// Thread safe for multiple callers on turn dirty.
template<typename T>
class DirtyTracker : public NonCopyable, public NonMovable {
public:
    FORCEINLINE void OnObjectTurnedDirty(T * object) {
        dirty_objects_.Push(object);
    }

    FORCEINLINE std::vector<TRef<T>> ConsumeDirtyGeometries() {
        return dirty_objects_.ConsumeAll();
    }
protected:
    TConsumeAllQueue<TRef<T>> dirty_objects_;
};

MI_NAMESPACE_END

#endif //MI_DIRTY_TRACKER_H
