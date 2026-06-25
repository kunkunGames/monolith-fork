@echo off
setlocal
REM Build monolith_query.exe - standalone SQLite query tool.
REM Run from a VS Developer Command Prompt, or let this script find vcvars64.bat.

cd /d "%~dp0"

REM Compute a SHA256 of the source inputs and inject its first 16 hex chars as
REM /DSOURCE_HASH so --version reports a hash the staleness guard can match.
REM Include header-only helper modules because they contribute to the binary.
set SOURCE_HASH=dev
for /f "usebackq tokens=*" %%H in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$files=@('monolith_query.cpp','monolith_query_tool_log.h','monolith_query_crg.h','monolith_query_review_ranges.h','monolith_query_bridge.h','monolith_query_help.h'); $ms=New-Object IO.MemoryStream; foreach($f in $files){ $b=[IO.File]::ReadAllBytes((Resolve-Path $f)); $ms.Write($b,0,$b.Length) }; $hash=[Security.Cryptography.SHA256]::Create().ComputeHash($ms.ToArray()); (($hash | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0,16)"`) do set SOURCE_HASH=%%H
echo Source hash: %SOURCE_HASH%

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
set "OUT_EXE=%BUILD_DIR%\monolith_query.exe"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if exist "%OUT_EXE%" del /F /Q "%OUT_EXE%"

echo Building with cl.exe...
cl /EHsc /std:c++17 /O2 /MT /I ThirdParty /I ..\MonolithProxy\ThirdParty /DSQLITE_ENABLE_FTS5 /DSOURCE_HASH=\"%SOURCE_HASH%\" monolith_query.cpp ThirdParty\sqlite3.c /Fe:"%OUT_EXE%"
if errorlevel 1 (
  echo FAILED: monolith_query.exe build failed
  exit /b 1
)
if not exist "%OUT_EXE%" (
  echo FAILED: compiler did not create "%OUT_EXE%"
  exit /b 1
)

if not exist "..\..\Binaries" mkdir "..\..\Binaries"
copy /Y "%OUT_EXE%" "..\..\Binaries\monolith_query.exe"
if errorlevel 1 (
  echo FAILED: could not copy monolith_query.exe to Plugins\Monolith\Binaries
  exit /b 1
)

echo.
echo Built: Plugins\Monolith\Binaries\monolith_query.exe
exit /b 0
