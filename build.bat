@echo off
rem Builds build\mango.exe and runs the tests. Needs Visual Studio 2022
rem (or its Build Tools) with the C++ workload. Run from any prompt.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio with C++ tools not found.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build\obj mkdir build\obj
if not exist build\tests mkdir build\tests
set CFLAGS=/nologo /O2 /DNDEBUG /utf-8 /W3 /MP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DIMGUI_DISABLE_DEMO_WINDOWS /DIMGUI_USE_WCHAR32 /Isrc /Ithird_party /Ithird_party\imgui /Ithird_party\imgui\backends
set CXXFLAGS=%CFLAGS% /std:c++17 /EHsc
set LIBS=winhttp.lib ws2_32.lib bcrypt.lib crypt32.lib shell32.lib ole32.lib user32.lib advapi32.lib

echo Building tests...
cl %CXXFLAGS% /Fobuild\tests\ /Fe:build\tests.exe tests\tests.cpp src\util.cpp src\auth.cpp src\net.cpp src\spotify.cpp src\soloist.cpp /link %LIBS% > build\tests.log 2>&1 || (type build\tests.log & exit /b 1)
build\tests.exe || exit /b 1

echo Building mango.exe...
rc /nologo /fo build\app.res src\app.rc || exit /b 1
cl %CXXFLAGS% /Fobuild\obj\ /Fe:build\mango.exe src\*.cpp third_party\imgui\imgui*.cpp third_party\imgui\backends\*.cpp build\app.res ^
    /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib windowscodecs.lib gdi32.lib %LIBS% > build\build.log 2>&1 || (type build\build.log & exit /b 1)
findstr /i /c:"warning" build\build.log
echo Built build\mango.exe
