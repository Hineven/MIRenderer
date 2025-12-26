/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <sstream>
#include <algorithm>
#include <glm/glm.hpp>
#include <core/infra.h>
#include <renderer/mi_console.h>
#include <renderer/mi_cvar.h>

MI_NAMESPACE_BEGIN

static inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void Console::ExecuteCommand(const std::string& cmd) {
    auto line = trim(cmd);
    if (line.empty()) return;

    // Parse: <cvar_name> [<cvar_value>]
    std::istringstream iss(line);
    std::string name;
    std::string value;
    iss >> name;
    std::getline(iss, value);
    value = trim(value);
    if (name.empty()) {
        MI_WARN("Console: expected '<cvar_name> [<cvar_value>]' ");
        return;
    }

    auto& reg = CVarRegistry::GetInstance();
    CVarBase* base = reg.GetCVar(name);
    if (!base) {
        MI_WARN("Console: unknown cvar '{}'", name.c_str());
        return;
    }

    // If no value provided, print current value and type
    if (value.empty()) {
        const char* type_str = "unknown";
        std::string val_str;
        switch (base->GetType()) {
            case CVarType::kBool: {
                type_str = "bool";
                auto* c = static_cast<CVar<bool>*>(base);
                val_str = c->Get() ? "true" : "false";
                break;
            }
            case CVarType::kInt: {
                type_str = "int";
                auto* c = static_cast<CVar<int>*>(base);
                val_str = std::to_string(c->Get());
                break;
            }
            case CVarType::kFloat: {
                type_str = "float";
                auto* c = static_cast<CVar<float>*>(base);
                val_str = std::to_string(c->Get());
                break;
            }
            case CVarType::kFloat2: {
                type_str = "float2";
                auto* c = static_cast<CVar<glm::vec2>*>(base);
                auto v = c->Get();
                val_str = std::to_string(v.x) + ", " + std::to_string(v.y);
                break;
            }
            case CVarType::kFloat3: {
                type_str = "float3";
                auto* c = static_cast<CVar<glm::vec3>*>(base);
                auto v = c->Get();
                val_str = std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z);
                break;
            }
            case CVarType::kFloat4: {
                type_str = "float4";
                auto* c = static_cast<CVar<glm::vec4>*>(base);
                auto v = c->Get();
                val_str = std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ", " + std::to_string(v.w);
                break;
            }
            case CVarType::kString: {
                type_str = "string";
                auto* c = static_cast<CVar<std::string>*>(base);
                val_str = c->Get();
                break;
            }
            default: break;
        }
        MI_LOG(MIInfraLogType::kInfo, "{}\n{}", val_str.c_str(), base->GetDescription());
        return;
    }

    switch (base->GetType()) {
        case CVarType::kBool: {
            auto* c = static_cast<CVar<bool>*>(base);
            if (value == "1" || value == "true" || value == "True") c->Set(true);
            else if (value == "0" || value == "false" || value == "False") c->Set(false);
            else MI_WARN("Console: invalid bool value '{}'", value.c_str());
            break;
        }
        case CVarType::kInt: {
            auto* c = static_cast<CVar<int>*>(base);
            try { c->Set(std::stoi(value)); }
            catch (...) { MI_WARN("Console: invalid int value '{}'", value.c_str()); }
            break;
        }
        case CVarType::kFloat: {
            auto* c = static_cast<CVar<float>*>(base);
            try { c->Set(std::stof(value)); }
            catch (...) { MI_WARN("Console: invalid float value '{}'", value.c_str()); }
            break;
        }
        case CVarType::kFloat2: {
            auto* c = static_cast<CVar<glm::vec2>*>(base);
            float x, y; char comma;
            std::istringstream vs(value);
            if ((vs >> x >> comma >> y) || (vs.clear(), vs.str(value), vs >> x >> y)) {
                c->Set(glm::vec2{x, y});
            } else MI_WARN("Console: invalid float2 value '{}'", value.c_str());
            break;
        }
        case CVarType::kFloat3: {
            auto* c = static_cast<CVar<glm::vec3>*>(base);
            float x, y, z; char c1, c2;
            std::istringstream vs(value);
            if ((vs >> x >> c1 >> y >> c2 >> z) || (vs.clear(), vs.str(value), vs >> x >> y >> z)) {
                c->Set(glm::vec3{x, y, z});
            } else MI_WARN("Console: invalid float3 value '{}'", value.c_str());
            break;
        }
        case CVarType::kFloat4: {
            auto* c = static_cast<CVar<glm::vec4>*>(base);
            float x, y, z, w; char c1, c2, c3;
            std::istringstream vs(value);
            if ((vs >> x >> c1 >> y >> c2 >> z >> c3 >> w) || (vs.clear(), vs.str(value), vs >> x >> y >> z >> w)) {
                c->Set(glm::vec4{x, y, z, w});
            } else MI_WARN("Console: invalid float4 value '{}'", value.c_str());
            break;
        }
        case CVarType::kString: {
            auto* c = static_cast<CVar<std::string>*>(base);
            c->Set(value);
            break;
        }
        default:
            MI_WARN("Console: unsupported cvar type for '{}'", name.c_str());
            break;
    }
}

std::vector<std::string> Console::GetCompletions(const std::string& prefix) const {
    auto& reg = CVarRegistry::GetInstance();
    auto list = reg.GetAllCVars();
    std::vector<std::string> comps;
    comps.reserve(list.size());
    for (auto* e : list) {
        auto id = e->GetId();
        if (id.rfind(prefix, 0) == 0) { // startswith
            comps.push_back(id);
        }
    }
    std::sort(comps.begin(), comps.end());
    return comps;
}

MI_NAMESPACE_END
