/*
 * Created: 2025/6/5
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef VOLPRIMS_LOADER_H
#define VOLPRIMS_LOADER_H

#include <renderer/mi_volume_primitives.h>

MI_NAMESPACE_BEGIN

class VolumePrimitivesLoader {
public:
    static bool LoadPLY (
        const std::filesystem::path& path, DeviceBindlessResourceAllocator & allocator,
        TRef<VolumePrimitives> & out_volprims,
        float percentage = 1.0f
    );
};

MI_NAMESPACE_END

#endif //VOLPRIMS_LOADER_H
