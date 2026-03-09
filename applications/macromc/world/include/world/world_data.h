/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MACROMC_WORLD_WORLD_DATA_H
#define MACROMC_WORLD_WORLD_DATA_H

#include "world/common.h"
#include "world/types.h"
#include "world/shell_data.h"
#include "core/refcounted.h"
#include <unordered_map>

MACROMC_WORLD_NAMESPACE_BEGIN

// WorldData: Top-level container for all shells
class WorldData : public mi::RefCounted {
public:
    WorldData();
    ~WorldData() override;
    
    // Non-copyable
    WorldData(const WorldData&) = delete;
    WorldData& operator=(const WorldData&) = delete;
    
    // Shell management
    mi::TRef<WorldShellData> CreateShell(ShellCategory category);
    mi::TRef<WorldShellData> GetShell(ShellId id);
    const mi::TRef<WorldShellData> GetShell(ShellId id) const;
    void RemoveShell(ShellId id);
    bool HasShell(ShellId id) const;
    
    // Iteration
    const auto& GetAllShells() const { return shells_; }
    size_t GetShellCount() const { return shells_.size(); }
    
    // Primary shell convenience access (usually for terrain)
    WorldShellData* GetPrimaryShell() { return primary_shell_; }
    const WorldShellData* GetPrimaryShell() const { return primary_shell_; }
    void SetPrimaryShell(ShellId id);
    
private:
    std::unordered_map<ShellId, mi::TRef<WorldShellData>> shells_;
    ShellId next_shell_id_ = 1;  // 0 is invalid
    WorldShellData* primary_shell_ = nullptr;  // Non-owning pointer
};

MACROMC_WORLD_NAMESPACE_END

#endif // MACROMC_WORLD_WORLD_DATA_H
