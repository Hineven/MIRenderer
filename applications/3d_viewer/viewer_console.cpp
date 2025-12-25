/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#include <imgui.h>
#include <cstdarg>
#include "viewer_console.h"

#include "renderer/mi_renderer.h"
MI_NAMESPACE_BEGIN
void ViewerImGuiConsle::Initialize() {
    auto & infra = GetInfra();
    infra.SetLogCallback([this](MIInfraLogType type, const std::string & msg) {
        this->Print(GetConsoleLogType(type), "%s", msg.c_str());
    });
}

void ViewerImGuiConsle::Destroy() {
    auto & infra = GetInfra();
    infra.SetLogCallback({});
}

uint32_t ViewerImGuiConsle::GetConsoleTextColor(ConsoleLogType type) {
    switch (type) {
        case ConsoleLogType::kInfo:
            return IM_COL32(255, 255, 255, 255);
        case ConsoleLogType::kWarning:
            return IM_COL32(255, 255, 0, 255);
        case ConsoleLogType::kError:
            return IM_COL32(255, 0, 0, 255);
        case ConsoleLogType::kRaw:
            return IM_COL32(200, 200, 200, 255);
        default:
            return IM_COL32(200, 200, 200, 255);
    }
}

void ViewerImGuiConsle::ClearLog() {
    logs_.clear();
}

void ViewerImGuiConsle::ClearHistory() {
}

int ViewerImGuiConsle::TextEditCallback(ImGuiInputTextCallbackData *data) {
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

void ViewerImGuiConsle::ExecConsoleCommandAndReset (InputBuffer input) {
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
    Renderer::Get().GetConsole().ExecuteCommand(command_str);

    scroll_to_bottom_ = true;
}

void ViewerImGuiConsle::DrawImGuiConsole() {
	ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Console", &opened_, ImGuiWindowFlags_MenuBar))
	{
		ImGui::End();
		return;
	}

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

	for (auto const& item : logs_)
	{

		bool should_show_item = true;
		switch (item.type)
		{
		    case ConsoleLogType::kInfo: should_show_item = show_info_; break;
		    case ConsoleLogType::kWarning: should_show_item = show_warnings_; break;
		    case ConsoleLogType::kError: should_show_item = show_errors_; break;
		    default: break;
		}

		if (should_show_item)
		{
            auto color = GetConsoleTextColor(item.type);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextUnformatted(item.text.c_str());
			ImGui::PopStyleColor();
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

	bool reclaim_focus = false;
	auto flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
	if (ImGui::InputText("##Command", input_buffer_.data(), input_buffer_.size(), flags,
		[](ImGuiInputTextCallbackData* data)
		{
		    auto console = static_cast<ViewerImGuiConsle*>(data->UserData);
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
	if (reclaim_focus)
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

	ImGui::End();
}


void ViewerImGuiConsle::PrintRaw(char const* fmt, ...)
{
    InputBuffer buf;
    std::va_list args;

    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    buf.back() = 0;
    va_end(args);

    ConsoleLogEntry item;
    item.text = buf.data();
    item.type = ConsoleLogType::kRaw;
    logs_.push_back(item);
}

void ViewerImGuiConsle::Print(ConsoleLogType type, char const* fmt, ...)
{
    InputBuffer buf;
    std::va_list args;

    va_start(args, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args);
    buf.back() = 0;
    va_end(args);

    ConsoleLogEntry item;
    item.text = buf.data();
    item.type = type;
    logs_.push_back(item);
}

// Copy-pasted from Donut ImGui console implementation

static void printLines(ViewerImGuiConsle& console, std::string const& output)
{
    if (output.empty())
        return;

    std::string line;
    for (int start = 0, curr = 0; curr < (int)output.size(); ++curr)
    {
        if ((output[curr] == '\r') || (output[curr] == '\n'))
        {
            console.PrintRaw("%s", std::string_view(&output[start], curr - start));
            start = ++curr;
        }
    }
}
// XXXX mk: we should probably use the columns features instead ?
static void printColumns(ViewerImGuiConsle& console, std::vector<std::string> const& items)
{
    auto computeLineWidth = []() {
        // XXXX mk: this only works if the font is monospace !
        float width = ImGui::CalcItemWidth();
        ImVec2 charWidth = ImGui::CalcTextSize("A");
        return (size_t)(width / charWidth.x);
    };

    size_t max_len = 0;
    for (auto const& candidate : items)
        max_len = std::max(max_len, candidate.size());

    size_t line_width = computeLineWidth();
    size_t ncolumns = line_width / max_len;

    std::string line; int col = 1;
    for (auto const& candidate : items)
    {
        line += candidate;
        if ((col % ncolumns) != 0)
        {
            line += ' ';
            ++col;
        }
        else
        {
            console.PrintRaw("%s", line.c_str());
            line.clear();
            col = 1;
        }
    }
    if (!line.empty())
        console.PrintRaw("%s", line.c_str());
}

int ViewerImGuiConsle::UpdateAutoCompletion(ImGuiInputTextCallbackData *data) {
    // Called each tick. Browse the possible completions and update the current completion.
    auto cvars = CVarRegistry::GetInstance().GetAllCVars();
    // Find possible completions
    std::string current_input(data->Buf, data->BufTextLen);
    std::string prefix_to_cursor = std::string(data->Buf, data->CursorPos);
    std::vector<std::string> possible_completions = Renderer::Get().GetConsole().GetCompletions(prefix_to_cursor);
    if (possible_completions.empty()) return 0;
    // Find common prefix among possible completions
    std::string common_prefix = possible_completions[0];;
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
    if (common_prefix.size() > current_input.size()) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, common_prefix.c_str());
        // Move cursor to the end
        data->CursorPos = (int)common_prefix.size();
    } else {
        // Show possible completions in console log if there are multiple & user pressed Tab again
        if (possible_completions.size() > 1) {
            // print all candidates in columns
            if (possible_completions.size() < 30)
                printColumns(*this, possible_completions);
            else {
                auto output = std::vector<std::string>(possible_completions.begin(), possible_completions.begin() + 30);
                output.push_back("..." + std::to_string(possible_completions.size() - 30) + " more");
                printColumns(*this, output);
            }
        }

    }
    return 0;
}

int ViewerImGuiConsle::BrowseHistoryCommand(ImGuiInputTextCallbackData *data) {
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