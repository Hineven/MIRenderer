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
        const std::filesystem::path& path, CommonGroupedDeviceResourceAllocator & allocator,
        RendererScene & world,
        TRef<VolumePrimitives> & out_volprims
    );
};

MI_NAMESPACE_END

#endif //VOLPRIMS_LOADER_H
