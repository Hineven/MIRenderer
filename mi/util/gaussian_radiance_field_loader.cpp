/*
 * Created: 2025/11/22
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <random>
#include <happly.h>
#include "util/gaussian_radiance_field_loader.h"

#include <numeric>
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

    // Use default activations for 3d gaussian radiance field data (consistent with relating paper implementations)
    bool use_activation = true;

    std::vector<PackedGaussianRadiancePoint> data;
    data.resize(num_pts);
    // Positions
    auto x = element.getProperty<float>("x");
    auto y = element.getProperty<float>("y");
    auto z = element.getProperty<float>("z");
    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        data[i].Position = {x[src], y[src], z[src]};
    }
    // Opacity
    std::vector<float> opacities;
    if (element.hasProperty("opacity")) {
        opacities = element.getProperty<float>("opacity");
    } else {
        MI_WARN("GaussianRadianceFieldLoader: No opacity property. Fallback to opaque.");
        opacities.assign(element.count, 1.f); // Defaults to opaque
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
        auto opacity = opacities[src];
        if (use_activation) {
            // Apply sigmoid activation for 3D Gaussian data
            opacity = 1.f / (1.f + exp(-opacity));
        }
        q.w = opacity;
        auto packed = glm::packSnorm4x8(q) & 0x00FFFFFFu; // Clear highest 8 bits for opacity
        packed |= glm::packUnorm4x8(q) & 0xFF000000u; // Store opacity in highest 8 bits
        data[i].PackedRotation_Opacity = packed;
    }
    // Scales (log-scale optional)
    auto sx = element.getProperty<float>("scale_0");
    auto sy = element.getProperty<float>("scale_1");
    auto sz = element.getProperty<float>("scale_2");

    for (int i=0;i<num_pts;i++) {
        auto src = shuffle[i];
        glm::vec3 sc = {sx[src], sy[src], sz[src]};
        if (use_activation) sc = exp(sc);
        sc = glm::max(sc, glm::vec3(0.001f));
        data[i].Scales = sc;
    }
    // Attempt to load SH radiance coefficients for 4th-order (16 coefficients). If absent use DC color.
    std::vector<glm::vec3> sh_coeffs; sh_coeffs.resize(num_pts * 16, glm::vec3(0));
    bool has_color_rgb = element.hasProperty("red") && element.hasProperty("green") && element.hasProperty("blue");
    bool has_color = has_color_rgb || (element.hasProperty("r") && element.hasProperty("g") && element.hasProperty("b"));
    std::vector<float> cr, cg, cb;
    if (has_color_rgb) {
        cr = element.getProperty<float>("red"); cg = element.getProperty<float>("green"); cb = element.getProperty<float>("blue");
    } else if (has_color) {
        cr = element.getProperty<float>("r"); cg = element.getProperty<float>("g"); cb = element.getProperty<float>("b");
    }
    // If SH per-coefficient properties exist (sh0_r,...), try gather them; else just fill DC and zero others.
    bool has_sh = true;
    for (int c = 0; c < 15 * 3; c++) {
        std::string base = "f_rest_" + std::to_string(c) + "_"; // expecting sh0_r etc
        bool present = element.hasProperty(base);
        if (!present) { has_sh = false; break; }
    }
    if (has_sh) {asfdasdfasd
        for (int c = 0;c < 15; c++) {
            auto rr = element.getProperty<float>("f_rest_" + std::to_string(0 + c));
            auto gg = element.getProperty<float>("f_rest_" + std::to_string(15 + c));
            auto bb = element.getProperty<float>("f_rest_" + std::to_string(30 + c));
            for (int i=0;i<num_pts;i++) {
                auto src = shuffle[i];
                sh_coeffs[i * 16 + c] = glm::vec3(rr[src], gg[src], bb[src]);
            }
        }
    } else if (has_color) {
        for (int i=0;i<num_pts;i++) {
            auto src = shuffle[i];
            // DC coefficient only
            sh_coeffs[i*16 + 0] = glm::vec3(cr[src], cg[src], cb[src]);
        }
    } else {
        MI_WARN("GaussianRadianceFieldLoader: No SH or color properties found; radiance defaults to zero.");
    }
    out_field = GaussianRadianceField::Create();
    out_field->SetPoints(data);
    out_field->SetSHCoefficients(sh_coeffs);
    return true;
}

MI_NAMESPACE_END
