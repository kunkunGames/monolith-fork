@echo off
setlocal EnableExtensions EnableDelayedExpansion

where cl.exe >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo FAILED: cl.exe is not on PATH and vswhere.exe was not found
        exit /b 1
    )

    set "VSINSTALL="
    for /f "usebackq tokens=*" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        echo FAILED: Visual Studio with the C++ toolchain was not found
        exit /b 1
    )

    call "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat"
    if errorlevel 1 (
        echo FAILED: vcvars64.bat failed for "!VSINSTALL!"
        exit /b 1
    )
)
echo VCVARS loaded, compiling...
cd /d "%~dp0"
echo CWD: %CD%
dir monolith_proxy.cpp
cl /EHsc /std:c++17 /O2 /MT /I ThirdParty monolith_proxy.cpp winhttp.lib /Fe:monolith_proxy.exe
if errorlevel 1 (
    echo FAILED: Compilation failed
    exit /b 1
)
if not exist "..\..\Binaries" mkdir "..\..\Binaries"
copy /Y monolith_proxy.exe "..\..\Binaries\monolith_proxy.exe"
echo SUCCESS: Built monolith_proxy.exe
