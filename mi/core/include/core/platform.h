/*
 * Created: 2024/7/4
 * Author:  hineven
 * See LICENSE for licensing.
 */

#ifndef MIRENDERER_CORE_PLATFORM_H
#define MIRENDERER_CORE_PLATFORM_H

#ifdef MI_COMPILER_MSVC
#define FORCEINLINE __forceinline
#endif

#ifdef MI_COMPILER_GNU
#ifndef NDEBUG
	#define FORCEINLINE inline 											/* Don't force code to be inline, or you'll run into -Wignored-attributes */
#else
	#define FORCEINLINE inline __attribute__ ((always_inline))			/* Force code to be inline */
#endif
#endif

#ifdef MI_COMPILER_GCC
#define FORCEINLINE inline __attribute__ ((always_inline))
#endif

#ifndef FORCEINLINE
#define FORCEINLINE inline
#warning "FORCEINLINE is not defined for this compiler"
#endif

// Called on background threads to initialize platform-specific context.
void InitializePlatformBackgroundThreadContext_Worker ();

// Called on background threads to destroy platform-specific context.
void DestroyPlatformBackgroundThreadContext_Worker ();

// Called on main thread to initialize platform-specific context.
// E.g., on Windows, initialize COM for the main thread.
void InitializePlatformMainThreadContext ();
void DestroyPlatformMainThreadContext ();

// Set the name of a thread
void RenameThread (const wchar_t* name);

// Result of a blocking popup dialog
enum class PlatformPopupResult {
    kRetry,
    kCancel,
    kOpenInEditor,  // Open the error source file in the default editor
};

// Show a blocking system dialog with Retry / Open in Editor / Cancel buttons.
// Blocks the calling thread until the user responds.
// file_path / line_number: if non-null / > 0, enables the "Open in Editor" button.
// Windows: TaskDialogIndirect (comctl32 v6)
// Linux:   stderr prompt + stdin input (r/o/c)
PlatformPopupResult ShowPlatformBlockingPopup (
    const char* title, const char* message,
    const char* file_path = nullptr, int line_number = 0);

// Terminate the program immediately with an error message.
[[noreturn]] void PlatformFatalAbort (const char* message);

#endif //MIRENDERER_CORE_PLATFORM_H
