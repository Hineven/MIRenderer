/*
 * Created: 2026/1/6
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>
#include <string>

#include "core/thr.h"

#ifdef _WIN32
#include <windows.h>
#include "combaseapi.h"
#include <commctrl.h>
#include <shellapi.h>
#pragma comment(lib, "Comctl32.lib")
#endif

namespace {
#ifdef _WIN32
    // Track per-thread COM init result to avoid unbalancing CoInitializeEx/CoUninitialize.
    thread_local bool g_com_initialized = false;
#endif
}

void InitializePlatformBackgroundThreadContext_Worker() {
#ifdef _WIN32
    // Worker/background threads should generally use MTA.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_OK / S_FALSE mean COM is initialized for this thread (S_FALSE = already initialized).
    // RPC_E_CHANGED_MODE means the thread is already STA; we must not uninitialize in that case.
    if (hr == S_OK || hr == S_FALSE) {
        g_com_initialized = true;
    } else {
        g_com_initialized = false;
    }
#endif
}

void DestroyPlatformBackgroundThreadContext_Worker() {
#ifdef _WIN32
    if (g_com_initialized) {
        CoUninitialize();
        g_com_initialized = false;
    }
#endif
}

void InitializePlatformMainThreadContext() {
#ifdef _WIN32
    // Main thread is typically STA for UI.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        g_com_initialized = true;
    } else {
        g_com_initialized = false;
    }
#endif
}

void DestroyPlatformMainThreadContext() {
#ifdef _WIN32
    if (g_com_initialized) {
        CoUninitialize();
        g_com_initialized = false;
    }
#endif
}

void RenameThread(const wchar_t* name) {
#ifdef _WIN32
    SetThreadDescription(GetCurrentThread(), name);
#endif
}

PlatformPopupResult ShowPlatformBlockingPopup(const char* title, const char* message,
                                               const char* file_path, int line_number) {
#ifdef _WIN32
    // --- Helper: parse first file:line from a DXC-style error message ---
    std::string parsed_file;
    int parsed_line = 0;
    if (file_path && file_path[0]) {
        parsed_file = file_path;
        parsed_line = line_number;
    } else {
        // Try to extract from message: pattern like "path/file.hlsl:123:"
        std::regex file_line_re(R"(([A-Za-z]:[^:\n]+\.\w+):(\d+):)");
        std::cmatch m;
        if (std::regex_search(message, m, file_line_re)) {
            parsed_file = m[1].str();
            parsed_line = std::stoi(m[2].str());
        }
    }

    const bool has_editor_info = !parsed_file.empty();

    // --- Helper: UTF-8 to wide string ---
    auto to_wide = [](const char* s) -> std::wstring {
        if (!s) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring ws(len > 0 ? len - 1 : 0, L'\0');
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, ws.data(), len);
        return ws;
    };

    // --- Per-callback data ---
    struct CallbackData {
        std::string file_path;
        int line_number;
    };
    CallbackData cb_data{parsed_file, parsed_line};

    // --- TaskDialog callback ---
    auto callback = [](HWND hwnd, UINT notification, WPARAM wp, LPARAM lp, LONG_PTR data) -> HRESULT {
        if (notification == TDN_BUTTON_CLICKED) {
            int btn_id = static_cast<int>(wp);
            if (btn_id == 1002) { // Open in VSCode
                auto* d = reinterpret_cast<CallbackData*>(data);
                // Construct vscode://file/{path}:{line} URI
                std::string uri = "vscode://file/" + d->file_path;
                if (d->line_number > 0) uri += ":" + std::to_string(d->line_number);
                auto w_uri = [](const std::string& s) -> std::wstring {
                    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
                    std::wstring ws(len > 0 ? len - 1 : 0, L'\0');
                    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), len);
                    return ws;
                }(uri);
                INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w_uri.c_str(),
                                                                    nullptr, nullptr, SW_SHOWNORMAL));
                if (r <= 32) {
                    // VSCode not found, fallback to notepad
                    std::string cmd = "notepad " + d->file_path;
                    system(cmd.c_str());
                }
                return S_FALSE; // Keep dialog open
            }
        }
        return S_OK;
    };

    // Convert strings to wide
    std::wstring w_title = to_wide(title);
    std::wstring w_message = to_wide(message);
    std::wstring w_footer;
    if (has_editor_info) {
        w_footer = to_wide(("File: " + parsed_file +
                            (parsed_line > 0 ? ":" + std::to_string(parsed_line) : std::string())).c_str());
    }

    // Configure buttons
    TASKDIALOG_BUTTON buttons[3];
    int num_buttons = 0;
    buttons[num_buttons++] = {1000, L"Retry"};
    if (has_editor_info) {
        buttons[num_buttons++] = {1002, L"Open in VSCode"};
    }
    buttons[num_buttons++] = {1001, L"Cancel"};

    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.hwndParent = NULL;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS_NO_ICON;
    config.dwCommonButtons = 0;
    config.pszWindowTitle = w_title.c_str();
    config.pszMainIcon = TD_ERROR_ICON;
    config.pszMainInstruction = w_message.c_str();
    config.cButtons = num_buttons;
    config.pButtons = buttons;
    config.nDefaultButton = 1000; // Retry
    config.pfCallback = callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&cb_data);
    if (has_editor_info) {
        config.pszFooter = w_footer.c_str();
    }

    int button_id = 0;
    TaskDialogIndirect(&config, &button_id, nullptr, nullptr);

    switch (button_id) {
        case 1000: return PlatformPopupResult::kRetry;
        case 1002: return PlatformPopupResult::kOpenInEditor;
        default:   return PlatformPopupResult::kCancel;
    }
#else
    fprintf(stderr, "\n=== %s ===\n%s\n", title, message);
    if (file_path && file_path[0]) {
        fprintf(stderr, "Source: %s", file_path);
        if (line_number > 0) fprintf(stderr, ":%d", line_number);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "Enter 'r' to Retry%s or 'c' to Cancel: ",
            (file_path && file_path[0]) ? ", 'o' to Open in editor" : "");
    fflush(stderr);
    char input = 'c';
    if (scanf(" %c", &input) == 1) {
        if (input == 'r' || input == 'R') return PlatformPopupResult::kRetry;
        if ((input == 'o' || input == 'O') && file_path && file_path[0]) {
            std::string cmd = std::string("xdg-open \"") + file_path + "\"";
            system(cmd.c_str());
            return PlatformPopupResult::kOpenInEditor;
        }
    }
    return PlatformPopupResult::kCancel;
#endif
}

[[noreturn]] void PlatformFatalAbort(const char* message) {
    fprintf(stderr, "FATAL: %s\n", message);
    fflush(stderr);
#ifdef _WIN32
    MessageBoxA(NULL, message, "Fatal Error", MB_OK | MB_ICONERROR | MB_TASKMODAL);
#endif
    std::abort();
}
