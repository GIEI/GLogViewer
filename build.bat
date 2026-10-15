@echo off
setlocal EnableExtensions EnableDelayedExpansion
pushd "%~dp0"

where cl.exe >nul 2>nul
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VSINSTALL=%%I"
        if defined VSINSTALL if exist "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" (
            call "!VSINSTALL!\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
        )
    )
)

where cl.exe >nul 2>nul
if errorlevel 1 (
    echo A Visual Studio C++ build tools installation is required.
    popd
    exit /b 1
)

if not exist build mkdir build

if /I "%~1"=="gui" goto build_gui

cl.exe /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc src\main.c src\reader.c src\follow.c src\terminal.c src\tui.c src\search_query.c src\json_pretty.c src\jsonl.c src\parsers.c src\filter.c src\output.c src\stats.c src\regex.c /Fe:build\logview.exe
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%

:build_gui
set "GUI_OUTPUT=build\logview-gui.exe"
if not "%~2"=="" set "GUI_OUTPUT=%~2"
if not defined SDL3_DIR if exist "third_party\SDL3\include\SDL3\SDL.h" (
    set "SDL3_DIR=%CD%\third_party\SDL3"
)
if not defined SDL3_DIR (
    for %%T in ("%LOCALAPPDATA%\Temp" "%USERPROFILE%\AppData\Local\Temp") do (
        for /d %%D in ("%%~fT\SDL3-devel-*") do (
            for /d %%S in ("%%~fD\SDL3-*") do (
                if not defined SDL3_DIR if exist "%%~fS\include\SDL3\SDL.h" if exist "%%~fS\lib\x64\SDL3.lib" set "SDL3_DIR=%%~fS"
            )
        )
    )
)
if not defined SDL3_DIR (
    echo SDL3 development files were not found. Set SDL3_DIR to the extracted SDL3 Visual C development package directory.
    echo See README.md for the Windows setup command.
    popd
    exit /b 1
)
if not exist "%SDL3_DIR%\include\SDL3\SDL.h" (
    echo SDL3_DIR does not contain include\SDL3\SDL.h.
    popd
    exit /b 1
)
if not exist "%SDL3_DIR%\lib\x64\SDL3.lib" (
    echo SDL3_DIR does not contain lib\x64\SDL3.lib.
    popd
    exit /b 1
)
cl.exe /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc /Ithird_party\nuklear /I"%SDL3_DIR%\include" src\gui.c src\gui_nuklear.c src\reader.c src\follow.c src\search_query.c src\json_pretty.c src\jsonl.c src\parsers.c src\filter.c src\output.c src\stats.c src\regex.c /Fe:"%GUI_OUTPUT%" /link /LIBPATH:"%SDL3_DIR%\lib\x64" SDL3.lib
set "BUILD_RESULT=%ERRORLEVEL%"
if not "%BUILD_RESULT%"=="0" (
    popd
    exit /b %BUILD_RESULT%
)
copy /Y "%SDL3_DIR%\lib\x64\SDL3.dll" build\SDL3.dll >nul
if errorlevel 1 set "BUILD_RESULT=1"
popd
exit /b %BUILD_RESULT%
