@echo off
REM Thin wrapper around the CMake build (see native/CMakeLists.txt). CMake
REM is required as of the Qt6 GUI shell migration -- Qt's own tooling
REM (AUTOMOC, qt_add_executable) assumes it, so this replaced the old
REM hand-rolled cl.exe-invoking script. Builds segytest.exe, segybench.exe,
REM segyviewer.exe and gen_fixture.exe.
REM
REM Requires: cmake on PATH, and a Qt6 install findable via CMAKE_PREFIX_PATH
REM or a vcpkg toolchain (set VCPKG_ROOT, default below, if using vcpkg's
REM "qt6-base" port) -- see native/README.md.
setlocal enabledelayedexpansion

if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\Users\Jeroen\Dev\vcpkg"
set "ROOT=%~dp0"
set "OUT=%ROOT%build-windows"

where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake not found on PATH. Install it (e.g. "winget install -e --id Kitware.CMake"^)
    echo and open a new terminal so PATH picks it up.
    exit /b 1
)

set "TOOLCHAIN_ARG="
if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    set "TOOLCHAIN_ARG=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows"
)

cmake -S "%ROOT%" -B "%OUT%" -A x64 %TOOLCHAIN_ARG%
if errorlevel 1 exit /b 1

cmake --build "%OUT%" --config Release
if errorlevel 1 exit /b 1

echo.
echo All builds succeeded. Binaries are in "%OUT%\Release".
endlocal
