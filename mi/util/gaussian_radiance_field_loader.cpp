/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <random>
#include <happly.h>
#include "util/gaussian_radiance_field_loader.h"
MI_NAMESPACE_BEGIN

bool GaussianRadianceFieldLoader::LoadPLY(const std::filesystem::path & path, DeviceBindlessResourceAllocator & alloc, TRef<GaussianRadianceField> & out_field, float percentage) {
    if (path.extension() != ".ply") {
        MI_WARN("GaussianRadianceFieldLoader: Not a PLY file: {}", path.string());
        return false;
    }
    if (out_field) {
        MI_WARN("GaussianRadianceFieldLoader: out_field already set.");
        return false;
    }
    happly::PLYData ply(path.string());
    if (!ply.hasElement("vertex")) {
        MI_WARN("GaussianRadianceFieldLoader: No vertex element.");
        return false;
    }
    auto & element = ply.getElement("vertex");
    mi_check(element.count < (1 << 24), "Too many points: {}", path.string());
    int num_pts = (int)element.count;
    if (num_pts <= 0) {
        MI_WARN("GaussianRadianceFieldLoader: Empty file: {}", path.string());
        return false;
    }
    std::vector<uint32_t> shuffle(num_pts);
    std::iota(shuffle.begin(), shuffle.end(), 0);
    std::shuffle(shuffle.begin(), shuffle.end(), std::mt19937{std::random_device{}()});
    if (percentage < 1.f) {
        num_pts = std::max((int)(num_pts * percentage), 1);
        MI_INFO("GaussianRadianceFieldLoader: Loading {}% -> {} points.", percentage * 100.f, num_pts);
    }
    std::vector<Packed3DGaussian> data;
    data.resize(num_pts);
    // Positions
    auto x = element.getProperty<float>("x");
    auto y = element.getProperty<float>("y");
    auto z = element.getProperty<float>("z");
    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        data[i].Position = {x[src], y[src], z[src]};
    }
    // Radiance / color channels
    std::vector<float> c0, c1, c2;
    if (element.hasProperty("f_dc_0")) {
        c0 = element.getProperty<float>("f_dc_0");
        c1 = element.getProperty<float>("f_dc_1");
        c2 = element.getProperty<float>("f_dc_2");
    } else if (element.hasProperty("color_0")) {
        c0 = element.getProperty<float>("color_0");
        c1 = element.getProperty<float>("color_1");
        c2 = element.getProperty<float>("color_2");
    } else {
        MI_WARN("GaussianRadianceFieldLoader: No radiance/color property. Fallback to white.");
        c0.assign(element.count, 1.f);
        c1.assign(element.count, 1.f);
        c2.assign(element.count, 1.f);
    }
    // Opacity (optional)
    std::vector<float> opacities;
    if (element.hasProperty("opacity")) {
        opacities = element.getProperty<float>("opacity");
    } else {
        opacities.assign(element.count, 0.f); // Non participating default
    }
    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        glm::vec3 col = {c0[src], c1[src], c2[src]};
        col = glm::clamp(col, 0.f, 32.f); // Allow HDR local radiance; will be tone mapped later
        // Pack color (clamp to [0,1] for now to reuse packing path)
        glm::vec3 pack_col = glm::clamp(col, 0.f, 1.f);
        auto opacity_half = glm::packHalf2x16({opacities[src], 0});
        data[i].PackedColor_OpacityLo = (glm::packUnorm4x8({pack_col.x, pack_col.y, pack_col.z, 0}) & 0x00FFFFFFu) | ((opacity_half & 0x00FF) << 24);
    }
    // Rotation (optional quaternion rot_0..rot_3 with w first or fallback identity) & opacity high bits
    std::vector<float> rot_w, rot_x, rot_y, rot_z;
    bool has_rot = element.hasProperty("rot_0");
    if (has_rot) {
        rot_w = element.getProperty<float>("rot_0");
        rot_x = element.getProperty<float>("rot_1");
        rot_y = element.getProperty<float>("rot_2");
        rot_z = element.getProperty<float>("rot_3");
    }
    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        glm::vec4 q = has_rot ? glm::vec4(rot_x[src], rot_y[src], rot_z[src], rot_w[src]) : glm::vec4(0,0,0,1);
        if (q.w < 0) q = -q;
        q = glm::normalize(q);
        uint32_t packed = (uint32_t)glm::packSnorm4x8(q);
        auto opacity_half = glm::packHalf2x16({opacities[src], 0});
        data[i].PackedRotation_OpacityHi = (packed & 0x00FFFFFFu) | ((opacity_half & 0xFF00) << 16);
    }
    // Scales (log-scale optional)
    auto sx = element.getProperty<float>("scale_0");
    auto sy = element.getProperty<float>("scale_1");
    auto sz = element.getProperty<float>("scale_2");
    bool log_scale = false; // Keep simple for now
    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        glm::vec3 sc = {sx[src], sy[src], sz[src]};
        if (log_scale) sc = exp(sc);
        sc = glm::max(sc, glm::vec3(0.001f));
        data[i].Scales = sc;
    }
    out_field = GaussianRadianceField::Create();
    out_field->SetPoints(data);
    return true;
}

MI_NAMESPACE_END

