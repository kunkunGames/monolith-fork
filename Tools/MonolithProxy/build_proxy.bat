@echo off
set "VCVARS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "%VCVARS%" (
    call "%VCVARS%"
) else (
    call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
)
if %ERRORLEVEL% neq 0 (
    echo FAILED: vcvars64.bat not found or failed
    exit /b 1
)
echo VCVARS loaded, compiling...
cd /d "%~dp0"
echo CWD: %CD%
dir monolith_proxy.cpp
cl /EHsc /std:c++17 /O2 /MT /I ThirdParty monolith_proxy.cpp winhttp.lib /Fe:monolith_proxy.exe
if %ERRORLEVEL% neq 0 (
    echo FAILED: Compilation failed
    exit /b 1
)
if not exist "..\..\Binaries" mkdir "..\..\Binaries"
copy /Y monolith_proxy.exe "..\..\Binaries\monolith_proxy.exe"
echo SUCCESS: Built monolith_proxy.exe
