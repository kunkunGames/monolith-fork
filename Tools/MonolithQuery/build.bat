@echo off
setlocal
REM Build monolith_query.exe - standalone SQLite query tool.
REM Run from a VS Developer Command Prompt, or let this script find vcvars64.bat.

cd /d "%~dp0"

REM Compute the checkout-independent source generation and inject it as
REM /DSOURCE_HASH so --version reports a hash the staleness guard can match.
REM The shared helper canonicalizes CRLF/lone-CR to LF for text source inputs;
REM published artifact SHA-256 values remain hashes of their exact raw bytes.
where python >nul 2>&1
if errorlevel 1 (
  echo FAILED: python is required to compute and publish the Query generation
  exit /b 1
)
set "SOURCE_HASH_TOOL=..\..\Scripts\source_generation_hash.py"
if not exist "%SOURCE_HASH_TOOL%" (
  echo FAILED: source generation hash helper is missing
  exit /b 1
)
set "SOURCE_HASH="
for /f "usebackq tokens=*" %%H in (`python "%SOURCE_HASH_TOOL%" --plugin-root "..\.." --tool query`) do set "SOURCE_HASH=%%H"
if not defined SOURCE_HASH (
  echo FAILED: could not compute the Query source generation hash
  exit /b 1
)
echo Source hash: %SOURCE_HASH%

if not exist "Generated\monolith_catalog_snapshot.json" (
  echo FAILED: generated catalog snapshot is missing
  exit /b 1
)
set "CATALOG_CHECK_ROOT=..\.."
if defined MONOLITH_CATALOG_CHECK_ROOT set "CATALOG_CHECK_ROOT=%MONOLITH_CATALOG_CHECK_ROOT%"
python generate_monolith_catalog_snapshot.py --check ^
  --root "%CATALOG_CHECK_ROOT%" ^
  --out "Generated\monolith_catalog_snapshot.json"
if errorlevel 1 (
  echo FAILED: generated catalog snapshot is stale relative to action registrations
  exit /b 1
)

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
cl /nologo /c /EHsc /std:c++17 /O2 /MT /I ThirdParty /I ..\MonolithProxy\ThirdParty /DSQLITE_ENABLE_FTS5 /DSOURCE_HASH=\"%SOURCE_HASH%\" monolith_query.cpp /Fo:"%BUILD_DIR%\monolith_query.obj"
if errorlevel 1 (
  echo FAILED: monolith_query.cpp compilation failed
  exit /b 1
)
cl /nologo /c /O2 /MT /DSQLITE_ENABLE_FTS5 ThirdParty\sqlite3.c /Fo:"%BUILD_DIR%\sqlite3.obj"
if errorlevel 1 (
  echo FAILED: sqlite3.c compilation failed
  exit /b 1
)
cl /nologo /MT "%BUILD_DIR%\monolith_query.obj" "%BUILD_DIR%\sqlite3.obj" /Fe:"%OUT_EXE%" /link /Brepro
if errorlevel 1 (
  echo FAILED: monolith_query.exe build failed
  exit /b 1
)
if not exist "%OUT_EXE%" (
  echo FAILED: compiler did not create "%OUT_EXE%"
  exit /b 1
)

REM Publish the executable and its exact generated catalog under immutable names,
REM validate both hashes plus --version identity, then atomically replace only the
REM current manifest. The fixed monolith_query.exe is compatibility-only and a
REM sharing violation there is intentionally non-fatal.
python publish_query_bundle.py publish ^
  --built-exe "%OUT_EXE%" ^
  --catalog "Generated\monolith_catalog_snapshot.json" ^
  --binaries-root "..\..\Binaries" ^
  --expected-source-hash "%SOURCE_HASH%"
if errorlevel 1 (
  echo FAILED: immutable Query/catalog bundle publication failed
  exit /b 1
)

echo.
echo Built authoritative bundle: Plugins\Monolith\Binaries\monolith_query.current.json
echo Compatibility only: Plugins\Monolith\Binaries\monolith_query.exe
exit /b 0
