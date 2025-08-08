/*
 * Created: 2025/6/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <happly.h>
#include "util/volprims_loader.h"
#include "renderer/mi_volume_primitives.h"

MI_NAMESPACE_BEGIN
struct PackedVolumePrimitive;

bool VolumePrimitivesLoader::LoadPLY(const std::filesystem::path& path, [[maybe_unused]] DeviceBindlessResourceAllocator &allocator, TRef<VolumePrimitives> &out_volprims) {
    if (path.extension() != ".ply") {
        MI_WARN("VolumePrimitivesLoader: Not a PLY file: {}", path.string());
        return false;
    }
    if(out_volprims != nullptr) {
        MI_WARN("VolumePrimitivesLoader: out_volprims is not null, loading cancelled.");
        return false;
    }

    happly::PLYData plyIn(path.string());
    auto& element = plyIn.getElement("vertex");
    // No larger gaussian models is supported.
    assert(element.count < (1 << 24));
    int num_prims = (int)element.count;

    std::vector<PackedVolumePrimitive> data;
    data.resize(num_prims);

    // Positions
    {
        auto x = element.getProperty<float>("x");
        auto y = element.getProperty<float>("y");
        auto z = element.getProperty<float>("z");
        for (int i = 0; i < num_prims; i++) {
            data[i].Position = {x[i], y[i], z[i]};
        }
    }

    // Scales
    {
        auto scale_x = element.getProperty<float>("scale_0");
        auto scale_y = element.getProperty<float>("scale_1");
        auto scale_z = element.getProperty<float>("scale_2");
        for (int i = 0; i < num_prims; i++) {
            data[i].Scales = {scale_x[i], scale_y[i], scale_z[i]};
        }
    }
    // Rotations
    {
        // W first!
        auto rotation_w = element.getProperty<float>("rot_0");
        auto rotation_x = element.getProperty<float>("rot_1");
        auto rotation_y = element.getProperty<float>("rot_2");
        auto rotation_z = element.getProperty<float>("rot_3");
        auto opacities = element.getProperty<float>("opacity");
        for (int i = 0; i < num_prims; i++) {
            auto q = glm::vec4(rotation_x[i], rotation_y[i], rotation_z[i], rotation_w[i]);
            if (q.w < 0) q = -q;
            float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q /= len;
			// Pack quaternion to 4xunorm8
			uint32_t packed = (uint32_t)(glm::packSnorm4x8(glm::vec4(q.x, q.y, q.z, q.w)));
            auto opacity = glm::packHalf2x16({opacities[i], 0});
			data[i].PackedRotation_OpacityHi = (packed & 0x00FFFFFFu) | ((opacity & 0xFF00) << 16);
        }
    }
    // Opacity & color
    {
        auto color_0 = element.getProperty<float>("color_0");
        auto color_1 = element.getProperty<float>("color_1");
        auto color_2 = element.getProperty<float>("color_2");
        auto opacities = element.getProperty<float>("opacity");
        for (int i = 0; i < num_prims; i++) {
            auto opacity = glm::packHalf2x16({opacities[i], 0});
            data[i].PackedColor_OpacityLo = (glm::packUnorm4x8(
            {color_0[i], color_1[i], color_2[i], 0}
            ) & 0x00FFFFFFu) | ((opacity & 0x00FF) << 24);
        }

    }
    // Create the volume primitives object
    out_volprims = VolumePrimitives::Create();
    out_volprims->SetPrimitives(data);
    return true;
}


MI_NAMESPACE_END