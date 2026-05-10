@echo off
REM Build monolith_query.exe — standalone SQLite query tool
REM Run from VS Developer Command Prompt, or let it find cl.exe via vswhere

where cl >nul 2>&1
if %ERRORLEVEL% equ 0 goto :build

echo cl.exe not found in PATH, attempting to find via vswhere...
set "VCVARS="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if exist "%VCVARS%" (
    call "%VCVARS%"
) else (
    echo FAILED: vswhere.exe could not locate vcvars64.bat
    exit /b 1
)

:build

where cl >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo Building with cl.exe...
    cl /EHsc /std:c++17 /O2 /MT /I ThirdParty /I ..\MonolithProxy\ThirdParty /DSQLITE_ENABLE_FTS5 monolith_query.cpp ThirdParty\sqlite3.c /Fe:monolith_query.exe
    if %ERRORLEVEL% equ 0 goto :copy
    echo Build failed.
    exit /b 1
)

echo cl.exe not found in PATH and vcvars64.bat failed to set it up.
echo Run from a Visual Studio Developer Command Prompt.
exit /b 1

:copy
if not exist ..\..\Binaries mkdir ..\..\Binaries
copy /Y monolith_query.exe ..\..\Binaries\monolith_query.exe
echo.
echo Built: Plugins\Monolith\Binaries\monolith_query.exe
