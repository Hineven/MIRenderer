/*
 * Created: 2025/12/25
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MI_VIEWER_CONSOLE_H
#define MI_VIEWER_CONSOLE_H

#include <array>
#include <chrono>
#include <functional>
#include <utility>
#include <renderer/mi_cvar.h>
#include <core/infra.h>
#include <glm/glm.hpp>

struct ImGuiInputTextCallbackData;
MI_NAMESPACE_BEGIN
class ViewerImGuiConsole {
public:

    void Initialize ();
    void Destroy ();

    bool is_tab_pressed_once_ {false}; // 用于监测用户是否连按了两下Tab键（光标不在末尾时需要连按两下进入强制补全）

    // 命令历史
    std::vector<std::string> command_history_;
    int history_current_index_ {-1}; // 历史浏览模式中，当前选择的历史条目索引。-1表示最新输入（非历史条目）
    std::string saved_current_input_; // 历史浏览模式中，保存的当前输入内容。当退出历史浏览模式或滚动到最新输入时恢复。

    int TextEditCallback(ImGuiInputTextCallbackData* data);

    void DrawImGuiConsole();

    // Draw the console panel embedded into an existing window (uses child regions).
    // This is the same UI as DrawImGuiConsole but without Begin/End("Console").
    void DrawImGuiConsoleEmbedded(glm::vec2 size = {});


    enum class ConsoleLogType {
        kInfo,
        kWarning,
        kError,
        kRaw
    };
    static uint32_t GetConsoleTextColor(ConsoleLogType type);

    void PrintRaw(char const *fmt, ...);
    void Print(ConsoleLogType, const std::string & location, const char *fmt, ...);

    int  UpdateAutoCompletion(ImGuiInputTextCallbackData* data);
    int BrowseHistoryCommand(ImGuiInputTextCallbackData* data);

    typedef std::array<char, 4096> InputBuffer;
    void ExecConsoleCommandAndReset (InputBuffer);

    bool opened_ {};
    struct ConsoleLogEntry {
        std::string text;
        std::string location;
        ConsoleLogType type;
        bool expanded {false};
        std::chrono::system_clock::time_point timestamp {std::chrono::system_clock::now()};
    };

    static ConsoleLogType GetConsoleLogType (MIInfraLogType type) {
        switch (type) {
            case MIInfraLogType::kInfo:
                return ConsoleLogType::kInfo;
            case MIInfraLogType::kWarning:
                return ConsoleLogType::kWarning;
            case MIInfraLogType::kError:
                return ConsoleLogType::kError;
            default:
                return ConsoleLogType::kRaw;
        }
    }

    std::vector<ConsoleLogEntry> logs_;
    void ClearLog();
    void ClearHistory();

    bool show_info_ {true};
    bool show_warnings_ {true};
    bool show_errors_ {true};
    bool auto_scroll_ {true};
    bool scroll_to_bottom_ {false};


    InputBuffer input_buffer_ {};
    std::string last_suggestion_input_;

private:
};

MI_NAMESPACE_END

#endif //MI_VIEWER_CONSOLE_H
