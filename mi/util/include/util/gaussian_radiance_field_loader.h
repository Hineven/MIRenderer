/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#ifndef MI_GAUSSIAN_RADIANCE_FIELD_LOADER_H
#define MI_GAUSSIAN_RADIANCE_FIELD_LOADER_H
#include <renderer/mi_gaussian_radiance_field.h>
MI_NAMESPACE_BEGIN
class GaussianRadianceFieldLoader {
public:
    static bool LoadPLY(
        const std::filesystem::path & path, DeviceBindlessResourceAllocator & alloc,
        TRef<GaussianRadianceField> & out_field, float percentage = 1.0f
    );
};
MI_NAMESPACE_END
#endif // MI_GAUSSIAN_RADIANCE_FIELD_LOADER_H

