/*
 * Created: 2025/6/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <happly.h>
#include "util/volprims_loader.h"

MI_NAMESPACE_BEGIN

bool VolumePrimitivesLoader::LoadPLY(const std::filesystem::path& path, [[maybe_unused]] DeviceBindlessResourceAllocator &allocator, Scene &scene, TRef<VolumePrimitives> &out_volprims) {
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

    std::vector<VolumePrimitive> data;

    // Positions
    {
        auto x = element.getProperty<float>("x");
        auto y = element.getProperty<float>("y");
        auto z = element.getProperty<float>("z");
        data.resize(num_prims);
        for (int i = 0; i < num_prims; i++) {
            data[i].position = {x[i], y[i], z[i]};
        }
    }

    // Scales
    {
        auto scale_x = element.getProperty<float>("scale_0");
        auto scale_y = element.getProperty<float>("scale_1");
        auto scale_z = element.getProperty<float>("scale_2");
        for (int i = 0; i < num_prims; i++) {
            data[i].scale = {scale_x[i], scale_y[i], scale_z[i]};
        }
    }
    // Rotations
    {
        // W first!
        auto rotation_w = element.getProperty<float>("rot_0");
        auto rotation_x = element.getProperty<float>("rot_1");
        auto rotation_y = element.getProperty<float>("rot_2");
        auto rotation_z = element.getProperty<float>("rot_3");
        for (int i = 0; i < num_prims; i++) {
            auto q = glm::vec4(rotation_x[i], rotation_y[i], rotation_z[i], rotation_w[i]);
            float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q /= len;
			// Pack quaternion to 4xunorm8
			uint32_t packed = (uint32_t)(glm::packSnorm4x8(glm::vec4(q.x, q.y, q.z, q.w)));
			data[i].packed_rotation = packed;
        }
    }
    // Opacity & color
    {
        auto alphas = element.getProperty<float>("opacity");
        auto color_0 = element.getProperty<float>("color_0");
        auto color_1 = element.getProperty<float>("color_1");
        auto color_2 = element.getProperty<float>("color_2");
        for (int i = 0; i < num_prims; i++) {
            data[i].packed_color_opacity = glm::packUnorm4x8(
            {color_0[i], color_1[i], color_2[i], alphas[i]}
            );
        }

    }
    // Create the volume primitives object
    out_volprims = VolumePrimitives::Create(&scene);
    out_volprims->SetPrimitives(data);
    return true;
}


MI_NAMESPACE_END