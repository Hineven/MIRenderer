/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "core/base.h"

MI_NAMESPACE_BEGIN

class Console {
public:
    // Execute a command. For now, only supports: "<cvar_name> <value>"
    void ExecuteCommand(const std::string& cmd);

    // Return possible completions given a prefix. Based on registered CVars.
    std::vector<std::string> GetCompletions(const std::string& prefix) const;
};

MI_NAMESPACE_END

