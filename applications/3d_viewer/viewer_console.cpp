/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <imgui.h>
#include <cstdarg>
#include "viewer_console.h"

#include "core/util/command_line.h"
#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN
void ViewerImGuiConsole::Initialize(const nlohmann::json &config) {
    auto & infra = GetInfra();
    infra.SetLogCallback([this](MIInfraLogType type, const std::string & msg, const std::string & location) {
        this->Print(GetConsoleLogType(type), location, "%s", msg.c_str());
    });

    // Load persisted history
    if (config.contains("console_history") && config["console_history"].is_array()) {
        command_history_.clear();
        for (const auto &entry : config["console_history"]) {
            if (entry.is_string()) {
                command_history_.push_back(entry.get<std::string>());
                if (command_history_.size() >= 128) break;
            }
        }
    }

    // Commands are registered by ViewerApp (owner of app state like pinned cvars and camera).
}

void ViewerImGuiConsole::Destroy(nlohmann::json &config) {
    auto & infra = GetInfra();
    infra.SetLogCallback({});

    // Persist last up-to-128 commands
    nlohmann::json history = nlohmann::json::array();
    const size_t start = command_history_.size() > 128 ? command_history_.size() - 128 : 0;
    for (size_t i = start; i < command_history_.size(); ++i) {
        history.push_back(command_history_[i]);
    }
    config["console_history"] = history;
}

uint32_t ViewerImGuiConsole::GetConsoleTextColor(ConsoleLogType type) {
    switch (type) {
        case ConsoleLogType::kInfo:
            return IM_COL32(255, 255, 255, 255);
        case ConsoleLogType::kWarning:
            return IM_COL32(255, 255, 0, 255);
        case ConsoleLogType::kError:
            return IM_COL32(255, 100, 100, 255);
        case ConsoleLogType::kRaw:
            return IM_COL32(200, 200, 200, 255);
        default:
            return IM_COL32(200, 200, 200, 255);
    }
}

void ViewerImGuiConsole::ClearLog() {
    logs_.clear();
}

void ViewerImGuiConsole::ClearHistory() {
}

int ViewerImGuiConsole::TextEditCallback(ImGuiInputTextCallbackData *data) {
    switch (data->EventFlag)
    {
        case ImGuiInputTextFlags_CallbackCompletion: // Tab key
            if (is_tab_pressed_once_) {
                // 强制补全
                is_tab_pressed_once_ = false;
                return UpdateAutoCompletion(data);
            }
            // 如果光标在末尾，直接补全模式，否则设置标记等待下一次Tab按下时强制补全
            if (data->CursorPos == data->BufTextLen) {
                return UpdateAutoCompletion(data);
            }
            is_tab_pressed_once_ = true;
            return 0;
        case ImGuiInputTextFlags_CallbackHistory: // Up/Down arrow key
            return BrowseHistoryCommand(data);
        default:
            return 0;
    }
}

void ViewerImGuiConsole::ExecConsoleCommandAndReset (InputBuffer input) {
    PrintRaw("> %s", input.data());

    // Store to history
    std::string command_str = input.data();
    if (!command_str.empty()) {
        command_history_.push_back(command_str);
    }
    // Reset history browsing state
    history_current_index_ = -1;
    saved_current_input_.clear();

    // Execute command
    auto match = CommandRegistry::Get().Match(command_str);
    if (match.has_value()) {
        if (match->kind == CommandMatchKind::kFull) {
            match->command->Execute(match.value());
        } else if (match->kind == CommandMatchKind::kPartial) {
            PrintRaw("Ambiguous command: %s", command_str.c_str());
            PrintRaw("Did you mean:");
            PrintRaw(" %s", match->command->ToString().c_str());
        } else if (match->kind == CommandMatchKind::kBadMatch) {
            PrintRaw("Bad arguments: %s", command_str.c_str());
            PrintRaw("Usage:");
            PrintRaw(" %s", match->command->ToString().c_str());
        } else {
            Print(ConsoleLogType::kError, "", "Unknown command: %s", command_str.c_str());
        }
    } else {
        Print(ConsoleLogType::kError, "", "Unknown command: %s", command_str.c_str());
    }

    scroll_to_bottom_ = true;
}

void ViewerImGuiConsole::DrawImGuiConsole() {
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console", &opened_, ImGuiWindowFlags_MenuBar))
    {
        ImGui::End();
        return;
    }

    auto content_avail = ImGui::GetContentRegionAvail();
    DrawImGuiConsoleEmbedded({content_avail.x, content_avail.y});

    ImGui::End();
}

void ViewerImGuiConsole::DrawImGuiConsoleEmbedded(glm::vec2 size) {
    // If caller passes (0,0), make it fill available region.
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = ImGui::GetContentRegionAvail().y;

    // Render the original Console window body into a child region.
    // (We intentionally keep the same IDs so behavior remains identical.)
    ImGui::BeginChild("ConsoleEmbeddedRoot", ImVec2{size.x, size.y}, false);

    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Close Console"))
            opened_ = false;
        ImGui::EndPopup();
    }

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("Edit"))
        {
            bool clearLog = ImGui::MenuItem("Clear Log");
            bool clearHistory = ImGui::MenuItem("Clear History");
            bool clearAll = ImGui::MenuItem("Clear All");

            if (clearLog || clearAll)
                ClearLog();
            if (clearHistory || clearAll)
                ClearHistory();
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const float footer_height = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("Log panel", ImVec2(0, -footer_height), false, ImGuiWindowFlags_HorizontalScrollbar);

    if (ImGui::BeginPopupContextWindow())
    {
        if (ImGui::Selectable("Clear"))
            ClearLog();
        ImGui::EndPopup();
    }

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

    auto format_time = [](std::chrono::system_clock::time_point tp) {
        using namespace std::chrono;
        auto diff = duration_cast<milliseconds>(tp.time_since_epoch());
        auto s = diff.count() / 1000;
        auto ms = diff.count() % 1000;
        std::time_t t = static_cast<std::time_t>(s);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)ms);
        return std::string(buf);
    };

    for (auto & item : logs_)
    {

        bool should_show_item = true;
        switch (item.type)
        {
            case ConsoleLogType::kInfo: should_show_item = show_info_; break;
            case ConsoleLogType::kWarning: should_show_item = show_warnings_; break;
            case ConsoleLogType::kError: should_show_item = show_errors_; break;
            default: break;
        }

        if (!should_show_item)
        {
            continue;
        }
        bool styled = item.type != ConsoleLogType::kRaw;
        if (styled) {
            ImGui::PushID(&item);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(35, 35, 35, 255));
            ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(70, 70, 70, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

            const float line_height = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            ImVec2 text_size = ImGui::CalcTextSize(item.text.c_str(), nullptr, false, avail.x);
            float lines = std::max(1.0f, text_size.y / line_height);
            float child_height = lines * line_height + ImGui::GetStyle().FramePadding.y * 2;
            if (item.expanded) {
                child_height += line_height + ImGui::GetStyle().ItemSpacing.y; // room for meta line
            }

            ImGui::BeginChild("log_item", ImVec2(0, child_height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            // Expand/collapse toggle
            if (styled) {
                if (ImGui::SmallButton(item.expanded ? "-" : "+")) {
                    item.expanded = !item.expanded;
                }
                ImGui::SameLine();
            } else {
                item.expanded = true;
                ImGui::TextUnformatted(" ");
                ImGui::SameLine();
            }

            auto color = GetConsoleTextColor(item.type);
            ImGui::PushStyleColor(ImGuiCol_Text, color);

            if (!item.expanded) {
                // collapsed: single line with ellipsis if needed
                std::string line = item.text;
                const float wrap_width = ImGui::GetContentRegionAvail().x;
                ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopTextWrapPos();
            } else {
                ImGui::TextUnformatted(item.text.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160,160,160,255));
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]", item.location.empty() ? "" : item.location.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("%s", format_time(item.timestamp).c_str());
            }

            ImGui::PopStyleColor();
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        } else {
            // Raw log, no styling
            ImGui::TextUnformatted(item.text.c_str());
        }
    }

    if (scroll_to_bottom_ || (auto_scroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()))
    {
        ImGui::SetScrollHereY(1.f);
    }

    scroll_to_bottom_ = false;
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::Separator();

    // Focus command box when pressing the tilde key
    bool focus_command = ImGui::IsKeyPressed(ImGuiKey_GraveAccent);

    bool reclaim_focus = false;
    auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
    if (focus_command) {
        ImGui::SetKeyboardFocusHere();
    }
    if (ImGui::InputText("##Command", input_buffer_.data(), input_buffer_.size(), flags,
        [](ImGuiInputTextCallbackData* data)
        {
            auto console = static_cast<ViewerImGuiConsole*>(data->UserData);
            return console->TextEditCallback(data);
        }, (void*)this))
    {
        if (input_buffer_[0] != '\0')
        {
            ExecConsoleCommandAndReset(input_buffer_);
            input_buffer_[0] = '\0';
        }
        reclaim_focus = true;
    }

    ImGui::SetItemDefaultFocus();
    if (reclaim_focus || focus_command)
        ImGui::SetKeyboardFocusHere(-1);

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Filters : "); ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    auto filterButton = [](char const* label, bool* value, MIInfraLogType type) {
        ImGui::PushStyleColor(ImGuiCol_Border, GetConsoleTextColor(GetConsoleLogType(type)));
        ImGui::Checkbox(label, value);
        ImGui::PopStyleColor();
    };
    filterButton("Error", &show_errors_, MIInfraLogType::kError); ImGui::SameLine();
    filterButton("Warning", &show_warnings_, MIInfraLogType::kWarning); ImGui::SameLine();
    filterButton("Info", &show_info_, MIInfraLogType::kInfo);
    ImGui::PopStyleVar();

    ImGui::EndChild();
}


void ViewerImGuiConsole::PrintRaw(char const* fmt, ...)
{
    InputBuffer buf;
    std::va_list args;

    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    buf.back() = 0;
    va_end(args);

    ConsoleLogEntry item;
    item.text = buf.data();
    item.location = {};
    item.type = ConsoleLogType::kRaw;
    item.expanded = true; // raw logs always expanded
    item.timestamp = std::chrono::system_clock::now();
    logs_.push_back(item);
}

void ViewerImGuiConsole::Print(ConsoleLogType type, const std::string & location, char const* fmt, ...)
{
    InputBuffer buf;
    std::va_list args;

    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    buf.back() = 0;
    va_end(args);

    ConsoleLogEntry item;
    item.text = buf.data();
    item.location = location;
    item.type = type;
    item.expanded = false;
    item.timestamp = std::chrono::system_clock::now();
    logs_.push_back(item);
}

// Copy-pasted from Donut ImGui console implementation
// XXXX mk: we should probably use the columns features instead ?
static void printColumns(ViewerImGuiConsole& console, std::vector<std::string> const& items)
{
    if (items.empty()) return;
    auto computeLineWidth = []() {
        float width = ImGui::GetContentRegionAvail().x;
        ImVec2 charWidth = ImGui::CalcTextSize("A");
        return (size_t)(width / charWidth.x);
    };

    size_t max_len = 0;
    for (auto const& candidate : items)
        max_len = std::max(max_len, candidate.size());
    size_t line_width = computeLineWidth();
    size_t ncolumns = std::max<size_t>(1, line_width / (max_len + 2));

    std::string line;
    size_t col = 0;
    for (auto const& candidate : items)
    {
        line += candidate;
        if (++col == ncolumns) {
            console.PrintRaw("%s", line.c_str());
            line.clear();
            col = 0;
        } else {
            // pad to fixed width
            if (candidate.size() < max_len + 2) line.append(max_len + 2 - candidate.size(), ' ');
        }
    }
    if (!line.empty())
        console.PrintRaw("%s", line.c_str());
}

static bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static std::vector<std::string> foldSuggestionsByCategory(const std::vector<std::string>& items, const std::string& prefix, size_t limit) {
    // Start from full list and progressively fold groups
    std::vector<std::string> display = items;
    if (display.size() <= limit) return display;

    // Build groups keyed by category up to next '.' after prefix
    struct Group { std::string key; std::vector<size_t> indices; };
    std::vector<Group> groups;
    // Map from key to group index
    std::unordered_map<std::string, size_t> key_to_group;

    for (size_t i = 0; i < display.size(); ++i) {
        const std::string& e = display[i];
        if (!starts_with(e, prefix)) continue;
        size_t pos = prefix.size();
        // must have a next segment, find next dot
        size_t next_dot = e.find('.', pos);
        if (next_dot == std::string::npos) continue; // no deeper category
        std::string key = e.substr(0, next_dot); // category key e.g. r.alpha
        auto it = key_to_group.find(key);
        if (it == key_to_group.end()) {
            groups.push_back({key, {i}});
            key_to_group[key] = groups.size() - 1;
        } else {
            groups[it->second].indices.push_back(i);
        }
    }

    // Only consider groups with 2+ items (folding reduces count)
    std::vector<Group> foldable;
    foldable.reserve(groups.size());
    for (auto& g : groups) {
        if (g.indices.size() >= 2) foldable.push_back(std::move(g));
    }
    // Sort groups lexicographically by key (fold lexicographically early first)
    std::sort(foldable.begin(), foldable.end(), [](const Group& a, const Group& b){ return a.key < b.key; });

    // Attempt folding until we fit or no more folds
    for (auto& g : foldable) {
        if (display.size() <= limit) break;
        // Folding this group reduces size by (count-1)
        const size_t count = g.indices.size();
        if (count < 2) continue;
        // Find first occurrence index in current display for this key (may have shifted)
        size_t first_idx = SIZE_MAX;
        std::vector<size_t> curr_indices;
        curr_indices.reserve(count);
        for (size_t j = 0; j < display.size(); ++j) {
            if (starts_with(display[j], g.key) && display[j].size() > g.key.size() && display[j][g.key.size()] == '.') {
                curr_indices.push_back(j);
            }
        }
        if (curr_indices.size() < 2) continue; // nothing to fold now
        first_idx = curr_indices.front();
        // Construct summary token "<key>...(N more)"
        std::string summary = g.key + "...(" + std::to_string(curr_indices.size()) + " more)";
        // Replace first, and erase the rest (from end to avoid shifting)
        display[first_idx] = summary;
        for (size_t k = curr_indices.size(); k-- > 1;) {
            display.erase(display.begin() + curr_indices[k]);
        }
    }

    return display;
}

int ViewerImGuiConsole::UpdateAutoCompletion(ImGuiInputTextCallbackData *data) {
    // Called each tick. Browse the possible completions and update the current completion.
    auto cvars = CVarRegistry::GetInstance().GetAllCVars();
    // Find possible completions
    std::string current_input(data->Buf, data->BufTextLen);
    std::string prefix_to_cursor = std::string(data->Buf, data->CursorPos);
    auto completion_result = CommandRegistry::Get().Complete(prefix_to_cursor);
    std::vector<std::string> possible_completions;
    for (auto e : completion_result.items) {
        possible_completions.push_back(e.text);
    }
    if (possible_completions.empty()) return 0;
    // Avoid spamming suggestions if input unchanged since last print
    bool input_dirty = (prefix_to_cursor != last_suggestion_input_);
    last_suggestion_input_ = prefix_to_cursor;
    // Find common prefix among possible completions
    std::string common_prefix = possible_completions[0];
    for (const auto& comp : possible_completions) {
        size_t min_length = std::min(common_prefix.size(), comp.size());
        size_t j = 0;
        for (; j < min_length; j++) {
            if (common_prefix[j] != comp[j]) {
                break;
            }
        }
        common_prefix = common_prefix.substr(0, j);
        if (common_prefix.empty()) {
            break;
        }
    }

    // If common prefix is longer than current input, update the input
    if (completion_result.replace_begin + common_prefix.size() > current_input.size()) {
        data->DeleteChars((int)completion_result.replace_begin, (int)(data->BufTextLen) - (int)completion_result.replace_begin);
        data->InsertChars((int)completion_result.replace_begin, common_prefix.c_str());
        // Move cursor to the end
        data->CursorPos = (int)common_prefix.size() + (int)completion_result.replace_begin;
    } else {
        // Show possible completions only if input changed or first time
        if (input_dirty && possible_completions.size() > 1) {
            // Try to fold overflow by category first
            size_t max_show = 30;
            std::vector<std::string> to_show;
            if (possible_completions.size() > max_show) {
                to_show = foldSuggestionsByCategory(possible_completions, prefix_to_cursor, max_show);
            } else {
                to_show = possible_completions;
            }
            // If still overflow, cap and add tail marker
            if (to_show.size() > max_show) {
                auto output = std::vector<std::string>(to_show.begin(), to_show.begin() + max_show);
                output.push_back("..." + std::to_string(to_show.size() - max_show) + " more");
                printColumns(*this, output);
            } else {
                printColumns(*this, to_show);
            }
            last_suggestion_input_ = prefix_to_cursor;
        }
    }
    return 0;
}

int ViewerImGuiConsole::BrowseHistoryCommand(ImGuiInputTextCallbackData *data) {
    if (data->EventKey == ImGuiKey_UpArrow) {
        if (history_current_index_ == -1) {
            // Enter history browsing mode
            saved_current_input_ = std::string(data->Buf, data->BufTextLen);
            history_current_index_ = (int)command_history_.size() - 1;
        } else if (history_current_index_ > 0) {
            history_current_index_--;
        }
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (history_current_index_ != -1) {
            history_current_index_++;
            if (history_current_index_ >= (int)command_history_.size()) {
                // Exit history browsing mode
                history_current_index_ = -1;
            }
        }
    }
    // Update input buffer
    data->DeleteChars(0, data->BufTextLen);
    if (history_current_index_ == -1) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, saved_current_input_.c_str());
    } else {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, command_history_[history_current_index_].c_str());
    }
    // Move cursor to the end
    data->CursorPos = data->BufTextLen;
    return 0;
}

MI_NAMESPACE_END

