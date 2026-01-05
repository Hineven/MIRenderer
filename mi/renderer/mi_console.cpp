/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include <sstream>
#include <algorithm>
#include <glm/glm.hpp>
#include <core/infra.h>
#include <core/util/command_line.h>
#include <renderer/mi_console.h>
#include <renderer/mi_cvar.h>

MI_NAMESPACE_BEGIN

static inline std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> CompleteCVarId(std::string_view prefix) {
    auto &reg = CVarRegistry::GetInstance();
    auto list = reg.GetAllCVars();

    std::vector<std::string> out;
    out.reserve(list.size());
    for (auto *e : list) {
        const auto &id = e->GetId();
        if (prefix.empty() || id.rfind(std::string(prefix), 0) == 0) {
            out.push_back(id);
        }
    }

    std::sort(out.begin(), out.end());
    return out;
}

void Console::RegisterCommands() {
    CommandRegistry::Get().MakeAndRegister(
        "set_cvar",
        {
            CommandTokenSpec::KeywordSet({"s"}),
            CommandTokenSpec::Free({}, "cvar", &CompleteCVarId),
            CommandTokenSpec::Free({}, "value"),
        },
        [](const CommandMatchResult &match) {
            // args: ["s", <cvar>, <value>]
            if (match.args.size() < 2) {
                MI_WARN("Console: expected 's <cvar> <value>'");
                return;
            }

            const std::string &name = match.args[1];

            auto& reg = CVarRegistry::GetInstance();
            CVarBase* base = reg.GetCVar(name);
            if (!base) {
                MI_WARN("Console: unknown cvar '{}'", name);
                return;
            }

            if (match.args.size() < 3) {
                MI_LOG(MIInfraLogType::kInfo, "{}\n{}", base->ToString(), base->GetDescription());
                return;
            }

            const std::string &value = match.args[2];
            // If no value provided, print current value and type
            if (value.empty()) {
                MI_LOG(MIInfraLogType::kInfo, "{}\n{}", base->ToString(), base->GetDescription());
                return;
            }

            if (!base->FromString(value)) {
                MI_WARN("Console: invalid value '{}' for cvar '{}'", value, name);
            }
        }
    );
}

MI_NAMESPACE_END
