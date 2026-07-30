@echo off
setlocal EnableExtensions EnableDelayedExpansion

where cl.exe >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    if not exist "!VSWHERE!" (
        echo FAILED: vswhere.exe was not found and cl.exe is not already configured
        exit /b 1
    )

    set "VSINSTALL="
    for /f "usebackq delims=" %%I in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        echo FAILED: no Visual Studio installation with the x64 C++ toolchain was found
        exit /b 1
    )

    set "VCVARS64=!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS64!" (
        echo FAILED: vcvars64.bat was not found under "!VSINSTALL!"
        exit /b 1
    )

    call "!VCVARS64!"
    if errorlevel 1 (
        echo FAILED: vcvars64.bat failed
        exit /b 1
    )
)

echo C++ toolchain ready, compiling...
pushd "%~dp0"
if errorlevel 1 (
    echo FAILED: could not enter "%~dp0"
    exit /b 1
)
echo CWD: %CD%
dir monolith_proxy.cpp
cl /EHsc /std:c++17 /O2 /MT /I ThirdParty monolith_proxy.cpp winhttp.lib /Fe:monolith_proxy.exe
if errorlevel 1 (
    echo FAILED: Compilation failed
    popd
    exit /b 1
)
if not exist "..\..\Binaries" mkdir "..\..\Binaries"
copy /Y monolith_proxy.exe "..\..\Binaries\monolith_proxy.exe"
if errorlevel 1 (
    echo FAILED: could not copy monolith_proxy.exe into Binaries
    popd
    exit /b 1
)
popd
echo SUCCESS: Built monolith_proxy.exe
exit /b 0
