/*
 * Created: 2025/7/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <sstream>
#include <iostream>
#include <glm/glm.hpp>
#include <renderer/mi_cvar.h>

MI_NAMESPACE_BEGIN

// CVarBase implementation
CVarBase::CVarBase(const std::string& id, const std::string& description, CVarType type)
    : id_(id), description_(description), type_(type), dirty_(false) {
}

template<typename T>
static CVarType GetType () {
    if constexpr (std::is_same_v<T, int>) {
        return CVarType::kInt;
    } else if constexpr (std::is_same_v<T, float>) {
        return CVarType::kFloat;
    } else if constexpr (std::is_same_v<T, glm::vec2>) {
        return CVarType::kFloat2;
    } else if constexpr (std::is_same_v<T, glm::vec3>) {
        return CVarType::kFloat3;
    } else if constexpr (std::is_same_v<T, glm::vec4>) {
        return CVarType::kFloat4;
    } else if constexpr (std::is_same_v<T, bool>) {
        return CVarType::kBool;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return CVarType::kString;
    } else {
        static_assert(false, "Unsupported type for CVar");
    }
    // return CVarType::kUnknown; // Fallback, should never be reached
}

// CVar template implementation
template<typename T>
CVar<T>::CVar(const std::string& id, const std::string& description, const T& default_value)
    : CVarBase(id, description, ::MI_NAMESPACE::GetType<T>()), value_(default_value), default_value_(default_value) {

    // Auto-register to global registry on construction
    CVarRegistry::GetInstance().RegisterCVar(this);
}

template<typename T>
void CVar<T>::Set(const T& value) {
    if (value_ != value) {
        value_ = value;
        MarkDirty();
    }
}

template<typename T>
void CVar<T>::ResetToDefault() {
    Set(default_value_);
}

// Type specializations for string conversion
template<>
std::string CVar<int>::ToString() const {
    return std::to_string(value_);
}

template<>
bool CVar<int>::FromString(const std::string& value) {
    try {
        int newValue = std::stoi(value);
        Set(newValue);
        return true;
    } catch (...) {
        return false;
    }
}

template<>
std::string CVar<int>::GetTypeName() const {
    return "int";
}

template<>
std::string CVar<float>::ToString() const {
    return std::to_string(value_);
}

template<>
std::string CVar<float>::GetTypeName() const {
    return "float";
}

template<>
std::string CVar<glm::vec2>::GetTypeName() const {
    return "float2";
}

template<>
std::string CVar<glm::vec3>::GetTypeName() const {
    return "float3";
}

template<>
std::string CVar<glm::vec4>::GetTypeName() const {
    return "float4";
}

template<>
bool CVar<float>::FromString(const std::string& value) {
    try {
        float newValue = std::stof(value);
        Set(newValue);
        return true;
    } catch (...) {
        return false;
    }
}

template<>
bool CVar<glm::vec2>::FromString(const std::string& value) {
    std::istringstream iss(value);
    std::string token;
    std::vector<float> components;

    while (std::getline(iss, token, ',') || std::getline(iss, token, ' ')) {
        if (!token.empty()) {
            try {
                components.push_back(std::stof(token));
            } catch (...) {
                return false;
            }
        }
    }

    if (components.size() == 2) {
        Set(glm::vec2(components[0], components[1]));
        return true;
    }
    return false;
}

template<>
bool CVar<glm::vec3>::FromString(const std::string& value) {
    std::istringstream iss(value);
    std::string token;
    std::vector<float> components;

    while (std::getline(iss, token, ',') || std::getline(iss, token, ' ')) {
        if (!token.empty()) {
            try {
                components.push_back(std::stof(token));
            } catch (...) {
                return false;
            }
        }
    }

    if (components.size() == 3) {
        Set(glm::vec3(components[0], components[1], components[2]));
        return true;
    }
    return false;
}

template<>
bool CVar<glm::vec4>::FromString(const std::string& value) {
    std::istringstream iss(value);
    std::string token;
    std::vector<float> components;

    while (std::getline(iss, token, ',') || std::getline(iss, token, ' ')) {
        if (!token.empty()) {
            try {
                components.push_back(std::stof(token));
            } catch (...) {
                return false;
            }
        }
    }

    if (components.size() == 4) {
        Set(glm::vec4(components[0], components[1], components[2], components[3]));
        return true;
    }
    return false;
}

template<>
std::string CVar<glm::vec2>::ToString() const {
    std::ostringstream oss;
    oss << value_.x << "," << value_.y;
    return oss.str();
}

template<>
std::string CVar<glm::vec3>::ToString() const {
    std::ostringstream oss;
    oss << value_.x << "," << value_.y << "," << value_.z;
    return oss.str();
}

template<>
std::string CVar<glm::vec4>::ToString() const {
    std::ostringstream oss;
    oss << value_.x << "," << value_.y << "," << value_.z << "," << value_.w;
    return oss.str();
}

template<>
std::string CVar<bool>::ToString() const {
    return value_ ? "true" : "false";
}

template<>
bool CVar<bool>::FromString(const std::string& value) {
    if (value == "true" || value == "1") {
        Set(true);
        return true;
    } else if (value == "false" || value == "0") {
        Set(false);
        return true;
    }
    return false;
}

template<>
std::string CVar<bool>::GetTypeName() const {
    return "bool";
}

template<>
std::string CVar<std::string>::ToString() const {
    return value_;
}

template<>
bool CVar<std::string>::FromString(const std::string& value) {
    Set(value);
    return true;
}

template<>
std::string CVar<std::string>::GetTypeName() const {
    return "string";
}

// CVarRegistry implementation
CVarRegistry& CVarRegistry::GetInstance() {
    static std::unique_ptr<CVarRegistry> instance_ptr;
    if (instance_ptr == nullptr) {
        instance_ptr.reset(new CVarRegistry());
    }
    return *instance_ptr;
}

void CVarRegistry::RegisterCVar(CVarBase * cvar) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string& id = cvar->GetId();

    if (cvars_.find(id) != cvars_.end()) {
        std::cerr << "Warning: CVar with id '" << id << "' already exists. Overwriting." << std::endl;
    }

    cvars_[id] = cvar;
}

CVarBase * CVarRegistry::GetCVar(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cvars_.find(id);
    return (it != cvars_.end()) ? it->second : nullptr;
}

std::vector<CVarBase*> CVarRegistry::GetAllCVars() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVarBase*> result;
    for (const auto& pair : cvars_) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<CVarBase*> CVarRegistry::GetDirtyCVars() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVarBase*> result;
    for (const auto& pair : cvars_) {
        if (pair.second->IsDirty()) {
            result.push_back(pair.second);
        }
    }
    return result;
}

void CVarRegistry::ClearAllDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& pair : cvars_) {
        pair.second->ClearDirty();
    }
}

std::string CVarRegistry::SerializeToString() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;

    for (const auto& pair : cvars_) {
        const auto& cvar = pair.second;
        oss << cvar->GetId() << "=" << cvar->ToString() << "\n";
    }

    return oss.str();
}

bool CVarRegistry::DeserializeFromString(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::istringstream iss(data);
    std::string line;

    while (std::getline(iss, line)) {
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string id = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        auto it = cvars_.find(id);
        if (it != cvars_.end()) {
            if (!it->second->FromString(value)) {
                std::cerr << "Failed to set CVar '" << id << "' to value '" << value << "'" << std::endl;
                return false;
            }
        }
    }

    return true;
}

// Explicit template instantiation for common types
template class CVar<int>;
template class CVar<float>;
template class CVar<glm::vec2>;
template class CVar<glm::vec3>;
template class CVar<glm::vec4>;
template class CVar<bool>;
template class CVar<std::string>;

MI_NAMESPACE_END
