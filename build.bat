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
cl.exe /nologo /std:c11 /W4 /O2 /D_CRT_SECURE_NO_WARNINGS /Iinclude /Isrc src\main.c src\reader.c src\jsonl.c src\parsers.c src\filter.c src\regex.c /Fe:build\logview.exe
set "BUILD_RESULT=%ERRORLEVEL%"
popd
exit /b %BUILD_RESULT%
