/*
 * Created: 2025/6/5
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <random>
#include <happly.h>
#include "util/volprims_loader.h"
#include "renderer/mi_volume_primitives.h"

MI_NAMESPACE_BEGIN
struct PackedVolumePrimitive;

bool VolumePrimitivesLoader::LoadPLY(
    const std::filesystem::path& path, [[maybe_unused]] DeviceBindlessResourceAllocator &allocator,
    TRef<VolumePrimitives> &out_volprims,
    float percentage
) {
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
    mi_check(element.count < (1 << 24), "A PLY file with too many volume primitives is not supported: {}", path.string());
    int num_prims = (int)element.count;
    if (num_prims <= 0) {
        MI_WARN("VolumePrimitivesLoader: No volume primitive found in PLY file: {}", path.string());
        return false;
    }
    std::vector<uint32_t> shuffle_indices (num_prims);
    for (uint32_t i = 0; i < (uint32_t)num_prims; i++) {
        shuffle_indices[i] = i;
    }
    std::shuffle(shuffle_indices.begin(), shuffle_indices.end(), std::mt19937{std::random_device{}()});
    if (percentage < 1.0f) {
        num_prims = std::max(static_cast<int>(num_prims * percentage), 1);
        MI_INFO("VolumePrimitivesLoader: Loading {}% of the volume primitives, total {}.", percentage * 100.f, num_prims);
    }


    std::vector<PackedVolumePrimitive> data;
    data.resize(num_prims);

    // Positions
    {
        auto x = element.getProperty<float>("x");
        auto y = element.getProperty<float>("y");
        auto z = element.getProperty<float>("z");
        for (int i = 0; i < num_prims; i++) {
            auto src = shuffle_indices[i];
            data[i].Position = {x[src], y[src], z[src]};
        }
    }

    bool should_solidify = false;
    // Opacity & color & rotations
    {
        // color_
        // base_color_
        // f_dc_
        std::vector<float> color_0, color_1, color_2;
        bool with_activation = false;
        bool scaled_sigmoid_activation = false;
        if (element.hasProperty("color_0")) {
            // Hand made data. No any activation needed.
            color_0 = element.getProperty<float>("color_0");
            color_1 = element.getProperty<float>("color_1");
            color_2 = element.getProperty<float>("color_2");
        } else if (element.hasProperty("base_color_0")) {
            // [ECCV2024] Relightable 3D Gaussian: Real-time Point Cloud Relighting with BRDF Decomposition and Ray Tracing
            with_activation = true;
            scaled_sigmoid_activation = true;
            should_solidify = true;
            color_0 = element.getProperty<float>("base_color_0");
            color_1 = element.getProperty<float>("base_color_1");
            color_2 = element.getProperty<float>("base_color_2");
        } else if (element.hasProperty("diffuse_color_0")) {
            // GS-IR
            with_activation = true;
            scaled_sigmoid_activation = false;
            should_solidify = true;
            color_0 = element.getProperty<float>("diffuse_color_0");
            color_1 = element.getProperty<float>("diffuse_color_1");
            color_2 = element.getProperty<float>("diffuse_color_2");
        } else if (element.hasProperty("f_dc_0")) {
            // Vanilla 3D Gaussian data
            MI_WARN("VolumePrimitivesLoader: Non-PBR-ready PLY detected, resorting to 0 order SH as albedo.");
            should_solidify = true;
            color_0 = element.getProperty<float>("f_dc_0");
            color_1 = element.getProperty<float>("f_dc_1");
            color_2 = element.getProperty<float>("f_dc_2");
        } else {
            MI_WARN("VolumePrimitivesLoader: No color property found in PLY file: {}. Loading failed.", path.string());
            return false;
        }
        if (should_solidify) {
            MI_WARN("VolumePrimitivesLoader: Solidifying volume primitives for 3D Gaussians.");
        }

        auto opacities = element.getProperty<float>("opacity");
        for (int i = 0; i < num_prims; i++) {
            auto src = shuffle_indices[i];
            float in_opacity = opacities[src];
            if (with_activation) {
                // Apply sigmoid activation for 3D Gaussian data
                in_opacity = 1.f / (1.f + exp(-in_opacity));
            }
            if (should_solidify) in_opacity *= 50;
            glm::vec3 in_color = {color_0[src], color_1[src], color_2[src]};
            if (with_activation) {
                if (scaled_sigmoid_activation) {
                    in_color = 0.03f + 0.77f / (1.f + exp(-in_color));
                } else {
                    in_color = 1.f / (1.f + exp(-in_color));
                }
            }
            in_color = glm::clamp(in_color, 0.f, 1.f);
            if (should_solidify) in_color = glm::pow(in_color, glm::vec3{0.33f}); // Empirically making albedo lighter for volume scattering
            auto opacity = glm::packHalf2x16({in_opacity, 0});
            data[i].PackedColor_OpacityLo = (glm::packUnorm4x8(
            {in_color.x, in_color.y, in_color.z, 0}
            ) & 0x00FFFFFFu) | ((opacity & 0x00FF) << 24);
        }
        // Rotations & opacity hi
        {
            // W first!
            auto rotation_w = element.getProperty<float>("rot_0");
            auto rotation_x = element.getProperty<float>("rot_1");
            auto rotation_y = element.getProperty<float>("rot_2");
            auto rotation_z = element.getProperty<float>("rot_3");
            for (int i = 0; i < num_prims; i++) {
                auto src = shuffle_indices[i];
                auto q = glm::vec4(rotation_x[src], rotation_y[src], rotation_z[src], rotation_w[src]);
                if (q.w < 0) q = -q;
                float len = sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                q /= len;
                // q = {0, 0, 0, 1};
                // Pack quaternion to 4xunorm8
                uint32_t packed = (uint32_t)(glm::packSnorm4x8(glm::vec4(q.x, q.y, q.z, q.w)));
                float in_opacity = opacities[src];
                if (with_activation) {
                    // Apply sigmoid activation for 3D Gaussian data
                    in_opacity = 1.f / (1.f + exp(-in_opacity));
                }
                if (should_solidify) in_opacity *= 50;
                auto opacity = glm::packHalf2x16({in_opacity, 0});
                data[i].PackedRotation_OpacityHi = (packed & 0x00FFFFFFu) | ((opacity & 0xFF00) << 16);
            }
        }
    }
    // Scales
    {
        bool with_activation = false;
        if (element.hasProperty("base_color_0")
            || element.hasProperty("diffuse_color_0")
            || element.hasProperty("f_dc_0")) {
            // 3D Gaussian data has log-scale stored in the scale property.
            with_activation = true;
        }
        auto scale_x = element.getProperty<float>("scale_0");
        auto scale_y = element.getProperty<float>("scale_1");
        auto scale_z = element.getProperty<float>("scale_2");
        for (int i = 0; i < num_prims; i++) {
            auto src = shuffle_indices[i];
            glm::vec3 in_scale = {scale_x[src], scale_y[src], scale_z[src]};
            if (with_activation) in_scale = exp(in_scale);
            if (should_solidify) in_scale *= 2.0f * sqrt(1.f / percentage);
            // Too small scales will cause precision issues in primitive ray intersecting
            in_scale = glm::max(in_scale, glm::vec3(0.005f));
            data[i].Scales = in_scale;
        }
    }
    // Create the volume primitives object
    out_volprims = VolumePrimitives::Create();
    out_volprims->SetPrimitives(data);
    return true;
}


MI_NAMESPACE_END