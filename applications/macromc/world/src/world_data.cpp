/*
 * Created: 2026/03/08
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include "world/world_data.h"

MACROMC_WORLD_NAMESPACE_BEGIN

WorldData::WorldData() = default;

WorldData::~WorldData() = default;

mi::TRef<WorldShellData> WorldData::CreateShell(ShellCategory category) {
    ShellId id = next_shell_id_++;
    auto shell = mi::Create<WorldShellData>(id, category);
    shells_[id] = shell;
    
    // First terrain shell becomes primary
    if (category == ShellCategory::kTerrain && primary_shell_ == nullptr) {
        primary_shell_ = shell.Raw();
    }
    
    return shell;
}

mi::TRef<WorldShellData> WorldData::GetShell(ShellId id) {
    auto it = shells_.find(id);
    if (it != shells_.end()) {
        return it->second;
    }
    return nullptr;
}

const mi::TRef<WorldShellData> WorldData::GetShell(ShellId id) const {
    auto it = shells_.find(id);
    if (it != shells_.end()) {
        return it->second;
    }
    return nullptr;
}

void WorldData::RemoveShell(ShellId id) {
    auto it = shells_.find(id);
    if (it != shells_.end()) {
        if (primary_shell_ == it->second.Raw()) {
            primary_shell_ = nullptr;
        }
        shells_.erase(it);
    }
}

bool WorldData::HasShell(ShellId id) const {
    return shells_.find(id) != shells_.end();
}

void WorldData::SetPrimaryShell(ShellId id) {
    auto it = shells_.find(id);
    if (it != shells_.end()) {
        primary_shell_ = it->second.Raw();
    }
}

MACROMC_WORLD_NAMESPACE_END
