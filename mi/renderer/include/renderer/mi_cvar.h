/*
 * Created: 2025/7/19
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_CVAR_H
#define MI_CVAR_H

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include "core/common.h"
#include "core/refcounted.h"

MI_NAMESPACE_BEGIN

enum class CVarType : unsigned {
    kUnknown = 0,
    kInt,
    kFloat,
    kFloat2,
    kFloat3,
    kFloat4,
    kBool,
    kString,
    kMax
};

// Base class for all CVars
class CVarBase : public RefCounted<true> {
public:
    CVarBase(const std::string& id, const std::string& description, CVarType type);
    virtual ~CVarBase() = default;

    const std::string& GetId() const { return id_; }
    const std::string& GetDescription() const { return description_; }

    bool IsDirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

    virtual std::string ToString() const = 0;
    virtual bool FromString(const std::string& value) = 0;
    FORCEINLINE CVarType GetType () const {return type_;}
    virtual std::string GetTypeName() const = 0;

protected:
    void MarkDirty() { dirty_ = true; }

private:
    std::string id_;
    std::string description_;
    CVarType type_;
    bool dirty_ = false;
};

// Template CVar class for specific types
template<typename T>
class CVar : public CVarBase {
public:
    CVar(
        const std::string& id, const std::string& description,
        const T& default_value
    );

    const T& Get() const { return value_; }
    void Set(const T& value);

    const T& GetDefault() const { return default_value_; }
    void ResetToDefault();

    std::string ToString() const override;
    bool FromString(const std::string& value) override;
    std::string GetTypeName() const override;

private:
    T value_;
    T default_value_;
};

// Global CVar registry for managing all CVars
class CVarRegistry {
public:
    static CVarRegistry& GetInstance();

    void RegisterCVar(CVarBase * cvar);
    CVarBase * GetCVar(const std::string& id);

    std::vector<CVarBase*> GetAllCVars();
    std::vector<CVarBase*> GetDirtyCVars();
    void ClearAllDirty();

    // Serialization for saving/loading configuration
    std::string SerializeToString();
    bool DeserializeFromString(const std::string& data);

private:
    CVarRegistry() = default;
    std::map<std::string, TRef<CVarBase>> cvars_;
    std::mutex mutex_;
};

// Common CVar type aliases
using IntCVar = CVar<int>;
using FloatCVar = CVar<float>;
using BoolCVar = CVar<bool>;
using StringCVar = CVar<std::string>;

MI_NAMESPACE_END

#endif //MI_CVAR_H
