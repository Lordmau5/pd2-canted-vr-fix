@echo off
rem Builds mod\canted_fix.dll (SuperBLT native plugin) against the MSVC x64 libraries.
rem Uses clang-cl when it's on PATH, otherwise cl. Run from any prompt: if no x64 developer
rem environment is active, the latest Visual Studio / Build Tools install is found with vswhere.
setlocal
cd /d "%~dp0"

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :env_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo error: vswhere.exe not found. Install Visual Studio or the Build Tools with the C++ workload,
	echo        or run this script from an "x64 Native Tools Command Prompt".
	exit /b 1
)
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
	echo error: no Visual Studio install with the x64 C++ tools found.
	exit /b 1
)
rem vcvars runs vswhere itself and expects it on PATH
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:env_ready

set "CC=cl"
where clang-cl >nul 2>&1 && set "CC=clang-cl"

if not exist build mkdir build
%CC% /nologo /O2 /MT /EHsc /std:c++17 /W3 /LD src\proxy.cpp /Fobuild\ /Febuild\canted_fix.dll /link user32.lib || exit /b 1
copy /y build\canted_fix.dll mod\canted_fix.dll >nul || exit /b 1
echo built mod\canted_fix.dll with %CC%
