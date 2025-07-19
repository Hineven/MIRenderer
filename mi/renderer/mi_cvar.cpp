/*
 * Created: 2025/7/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "include/renderer/mi_cvar.h"
#include <sstream>
#include <iostream>

MI_NAMESPACE_BEGIN

// CVarBase implementation
CVarBase::CVarBase(const std::string& id, const std::string& description)
    : id_(id), description_(description), dirty_(false) {
}

// CVar template implementation
template<typename T>
CVar<T>::CVar(const std::string& id, const std::string& description, const T& default_value)
    : CVarBase(id, description), value_(default_value), default_value_(default_value) {
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
std::string CVar<float>::GetTypeName() const {
    return "float";
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
    static CVarRegistry * instance_ptr;
    if (instance_ptr == nullptr) {
        instance_ptr = new CVarRegistry();
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
    return (it != cvars_.end()) ? it->second.Raw() : nullptr;
}

std::vector<CVarBase*> CVarRegistry::GetAllCVars() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVarBase*> result;
    for (const auto& pair : cvars_) {
        result.push_back(pair.second.Raw());
    }
    return result;
}

std::vector<CVarBase*> CVarRegistry::GetDirtyCVars() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CVarBase*> result;
    for (const auto& pair : cvars_) {
        if (pair.second->IsDirty()) {
            result.push_back(pair.second.Raw());
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
template class CVar<bool>;
template class CVar<std::string>;

MI_NAMESPACE_END
