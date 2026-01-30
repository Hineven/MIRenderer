/*
 * Created: 2026/1/6
 * Author:  hineven
 * See LICENSE for licensing.
 */
#include "core/platform.h"

#include "core/thr.h"

#ifdef _WIN32
#include "combaseapi.h"
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
