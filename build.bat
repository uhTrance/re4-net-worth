@echo off
REM Builds re4_net_worth.dll.
REM
REM No CMake: the REFramework plugin API is header-only, and Dear ImGui is
REM compiled straight in from third_party\imgui (the same commit and options
REM REFramework uses - see src\imgui_config.hpp).
REM
REM Usage:
REM   build.bat          - build into build\
REM   build.bat deploy   - build, then copy into the game's plugins folder
REM                        (close the game first, it locks the DLL). Set
REM                        RE4_DIR to the game folder if it is not in the
REM                        default Steam library.

setlocal enabledelayedexpansion

set "PROJECT_DIR=%~dp0"
set "OUT_DIR=%PROJECT_DIR%build"
if not defined RE4_DIR set "RE4_DIR=%ProgramFiles(x86)%\Steam\steamapps\common\RESIDENT EVIL 4  BIOHAZARD RE4"
set "GAME_PLUGINS=%RE4_DIR%\reframework\plugins"
set "DO_DEPLOY=0"
if /i "%~1"=="deploy" set "DO_DEPLOY=1"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [build] ERROR: vswhere.exe not found - is Visual Studio Build Tools installed?
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VS_PATH=%%i"
)
if not defined VS_PATH (
    echo [build] ERROR: no VS install with the C++ toolchain found.
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: failed to initialise the x64 build environment.
    exit /b 1
)

if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"
pushd "%OUT_DIR%"

REM /O2 /MT - optimised, static CRT so the DLL needs no redistributable
REM /std:c++20 - REFramework's API.hpp uses std::span
set "COMMON=/nologo /std:c++20 /EHsc /O2 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DIMGUI_USER_CONFIG=\"imgui_config.hpp\""
set "INCLUDES=/I "%PROJECT_DIR%src" /I "%PROJECT_DIR%third_party\imgui" /external:I "%PROJECT_DIR%include" /external:I "%PROJECT_DIR%third_party\imgui" /external:W0"

echo [build] imgui...
cl %COMMON% /W0 %INCLUDES% /c "%PROJECT_DIR%third_party\imgui\imgui.cpp" "%PROJECT_DIR%third_party\imgui\imgui_draw.cpp" "%PROJECT_DIR%third_party\imgui\imgui_tables.cpp" "%PROJECT_DIR%third_party\imgui\imgui_widgets.cpp"
if errorlevel 1 goto :failed

echo [build] re4_net_worth...
cl %COMMON% /W4 /LD %INCLUDES% "%PROJECT_DIR%src\plugin.cpp" "%PROJECT_DIR%src\window.cpp" imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj user32.lib /Fe:"re4_net_worth.dll"
if errorlevel 1 goto :failed

popd
echo.
echo [build] SUCCESS
echo   %OUT_DIR%\re4_net_worth.dll

if "%DO_DEPLOY%"=="1" (
    copy /Y "%OUT_DIR%\re4_net_worth.dll" "%GAME_PLUGINS%\" >nul
    if errorlevel 1 (
        echo [build] WARNING: could not copy - is the game running?
        exit /b 1
    )
    echo   deployed re4_net_worth.dll
)
endlocal
exit /b 0

:failed
popd
echo.
echo [build] FAILED
exit /b 1
