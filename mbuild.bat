@echo off
:: =============================================================================
:: mbuild.bat — reusable build helper using CLion's bundled CMake + vcpkg.
::
:: Mirrors the exact toolchain CLion uses (same CMake binary, same vcpkg
:: toolchain file, same build dir naming), so what builds here is what you'd
:: get pressing Build in CLion. Usable from any shell without CLion running.
::
:: Usage:
::   mbuild.bat configure [config]     Configure (config = RelWithDebInfo^|Debug^|Release, default RelWithDebInfo)
::   mbuild.bat build     [target]      Build a target (default: build all)
::   mbuild.bat test      [test_target] Build + run a test target (ctest if none given)
::   mbuild.bat ctest                   Run ctest on the configured build dir
::
:: Examples:
::   mbuild.bat configure
::   mbuild.bat build macromc_streaming_test
::   mbuild.bat test  macromc_streaming_test
:: =============================================================================

setlocal enabledelayedexpansion

:: --- Paths (match CLion's bundled toolchain) ---
set "CMAKE_EXE=C:\Program Files\JetBrains\CLion 2026.1\bin\cmake\win\x64\bin\cmake.exe"
set "VCPKG_TOOLCHAIN=C:\Users\hineven\.vcpkg-clion\vcpkg\scripts\buildsystems\vcpkg.cmake"
set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "VCPKG_BIN=C:\Users\hineven\.vcpkg-clion\vcpkg\installed\x64-windows\bin"
set "SRC_DIR=%~dp0"
:: strip trailing backslash
if "%SRC_DIR:~-1%"=="\" set "SRC_DIR=%SRC_DIR:~0,-1%"

set "CONFIG=RelWithDebInfo"
set "BUILD_DIR=%SRC_DIR%\cmake-build-relwithdebinfo"

set "ACTION=%~1"
if "%ACTION%"=="" (
    echo Usage: mbuild.bat [configure^|build^|test^|ctest] [args]
    echo Run "mbuild.bat" with no args for full help.
    exit /b 1
)

:: Allow config override as second positional for configure: mbuild configure Debug
if /i "%ACTION%"=="configure" (
    if not "%~2"=="" (
        set "CONFIG=%~2"
        set "BUILD_DIR=%SRC_DIR%\cmake-build-!CONFIG!"
    )
)

:: --- Initialize MSVC environment (CLion does this via its VS toolchain) ---
:: Without this, cl.exe can't find the C++ standard library (<cstdint> etc.)
:: because INCLUDE/LIB are unset. Must be sourced before any cmake/cl call.
if not exist "%VSVARS%" (
    echo [ERROR] vcvars64.bat not found at: %VSVARS%
    echo Edit VSVARS in this script if VS 2022 is installed elsewhere.
    exit /b 1
)
call "%VSVARS%" >nul 2>&1

:: Verify cmake exists
if not exist "%CMAKE_EXE%" (
    echo [ERROR] CLion CMake not found at: %CMAKE_EXE%
    echo Edit CMAKE_EXE in this script if CLion is installed elsewhere.
    exit /b 1
)
if not exist "%VCPKG_TOOLCHAIN%" (
    echo [ERROR] vcpkg toolchain not found at: %VCPKG_TOOLCHAIN%
    echo Edit VCPKG_TOOLCHAIN in this script if vcpkg is elsewhere.
    exit /b 1
)

:: --- Dispatch ---
if /i "%ACTION%"=="configure" goto :do_configure
if /i "%ACTION%"=="build" (
    set "TARGET=%~2"
    goto :do_build
)
if /i "%ACTION%"=="test" (
    set "TEST_TARGET=%~2"
    goto :do_test
)
if /i "%ACTION%"=="ctest" goto :do_ctest

echo [ERROR] Unknown action: %ACTION%
echo Valid actions: configure, build, test, ctest
exit /b 1

:do_configure
echo === Configuring %CONFIG% into %BUILD_DIR% ===
"%CMAKE_EXE%" -B "%BUILD_DIR%" -S "%SRC_DIR%" ^
    -DCMAKE_BUILD_TYPE=%CONFIG% ^
    -DCMAKE_TOOLCHAIN_FILE="%VCPKG_TOOLCHAIN%" ^
    -DMI_BYPASS_RHI_THREAD:BOOL=OFF
exit /b %errorlevel%

:do_build
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo === Build dir not configured, configuring first ===
    call :do_configure || exit /b 1
)
if "%TARGET%"=="" (
    echo === Building all in %BUILD_DIR% ===
    "%CMAKE_EXE%" --build "%BUILD_DIR%"
) else (
    echo === Building target: %TARGET% ===
    "%CMAKE_EXE%" --build "%BUILD_DIR%" --target %TARGET%
)
exit /b %errorlevel%

:do_test
if "%TEST_TARGET%"=="" goto :do_ctest
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo === Build dir not configured, configuring first ===
    call :do_configure || exit /b 1
)
echo === Building test target: %TEST_TARGET% ===
"%CMAKE_EXE%" --build "%BUILD_DIR%" --target %TEST_TARGET% || exit /b 1
echo === Running %TEST_TARGET% ===
:: vcpkg DLLs need to be on PATH for the test exe to find them.
set "PATH=%VCPKG_BIN%;%PATH%"
"%BUILD_DIR%\tests\%TEST_TARGET%.exe"
exit /b %errorlevel%

:do_ctest
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo [ERROR] Build dir not configured. Run: mbuild.bat configure
    exit /b 1
)
echo === Running ctest in %BUILD_DIR% ===
set "PATH=%VCPKG_BIN%;%PATH%"
"%CMAKE_EXE%" --build "%BUILD_DIR%" --target RUN_TESTS 2>nul
if errorlevel 1 (
    cd /d "%BUILD_DIR%" && "%CMAKE_EXE%" -E ctest --output-on-failure
)
exit /b %errorlevel%
