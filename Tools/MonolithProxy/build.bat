@echo off
setlocal
REM Build monolith_proxy.exe - stdio to HTTP MCP proxy.
REM Run from a VS Developer Command Prompt, or let this script find vcvars64.bat.

cd /d "%~dp0"

where cl >nul 2>&1
if not errorlevel 1 goto :build

echo cl.exe not found in PATH, attempting to find via vswhere...
set "VCVARS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS%" (
  echo FAILED: vswhere.exe could not locate vcvars64.bat
  exit /b 1
)
call "%VCVARS%"
if errorlevel 1 (
  echo FAILED: vcvars64.bat failed
  exit /b 1
)

:build
where cl >nul 2>&1
if errorlevel 1 (
  echo cl.exe not found in PATH and vcvars64.bat failed to set it up.
  exit /b 1
)

set "BUILD_DIR=%CD%\build"
set "OUT_EXE=%BUILD_DIR%\monolith_proxy.exe"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if exist "%OUT_EXE%" del /F /Q "%OUT_EXE%"

echo Building with cl.exe...
cl /EHsc /std:c++17 /O2 /MT /I ThirdParty monolith_proxy.cpp winhttp.lib /Fe:"%OUT_EXE%"
if errorlevel 1 (
  echo FAILED: monolith_proxy.exe build failed
  exit /b 1
)
if not exist "%OUT_EXE%" (
  echo FAILED: compiler did not create "%OUT_EXE%"
  exit /b 1
)

if not exist "..\..\Binaries" mkdir "..\..\Binaries"
copy /Y "%OUT_EXE%" "..\..\Binaries\monolith_proxy.exe"
if errorlevel 1 (
  echo FAILED: could not copy monolith_proxy.exe to Plugins\Monolith\Binaries
  exit /b 1
)

echo Built: Plugins\Monolith\Binaries\monolith_proxy.exe
exit /b 0
