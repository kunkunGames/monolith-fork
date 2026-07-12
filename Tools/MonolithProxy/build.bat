@echo off
setlocal
REM Build monolith_proxy.exe - stdio to HTTP MCP proxy.
REM Run from a VS Developer Command Prompt, or let this script find vcvars64.bat.

cd /d "%~dp0"
for %%I in ("..\..") do set "MONOLITH_PLUGIN_ROOT=%%~fI"

REM Stamp the binary with the checkout-independent generation of every native
REM proxy source/header input so release packaging can prove it is not stale.
REM The shared helper canonicalizes CRLF/lone-CR to LF for text source inputs;
REM published artifact SHA-256 values remain hashes of their exact raw bytes.
where python >nul 2>&1
if errorlevel 1 (
  echo FAILED: python is required to compute the Proxy source generation
  exit /b 1
)
set "SOURCE_HASH_TOOL=..\..\Scripts\source_generation_hash.py"
if not exist "%SOURCE_HASH_TOOL%" (
  echo FAILED: source generation hash helper is missing
  exit /b 1
)
set "SOURCE_HASH="
for /f "usebackq tokens=*" %%H in (`python "%SOURCE_HASH_TOOL%" --plugin-root "..\.." --tool proxy`) do set "SOURCE_HASH=%%H"
if not defined SOURCE_HASH (
  echo FAILED: could not compute the Proxy source generation hash
  exit /b 1
)
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
set "OUT_EXE=%BUILD_DIR%\monolith_proxy.exe"
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if exist "%OUT_EXE%" del /F /Q "%OUT_EXE%"

echo Building with cl.exe...
cl /nologo /c /EHsc /std:c++17 /O2 /Brepro /experimental:deterministic "/pathmap:%MONOLITH_PLUGIN_ROOT%=." /MT /I ThirdParty /DSOURCE_HASH=\"%SOURCE_HASH%\" monolith_proxy.cpp /Fo:"%BUILD_DIR%\monolith_proxy.obj"
if errorlevel 1 (
  echo FAILED: monolith_proxy.cpp compilation failed
  exit /b 1
)
cl /nologo /c /EHsc /std:c++17 /O2 /Brepro /experimental:deterministic "/pathmap:%MONOLITH_PLUGIN_ROOT%=." /MT /I ThirdParty monolith_proxy_offline.cpp /Fo:"%BUILD_DIR%\monolith_proxy_offline.obj"
if errorlevel 1 (
  echo FAILED: monolith_proxy_offline.cpp compilation failed
  exit /b 1
)
cl /nologo /EHsc /std:c++17 /O2 /MT "%BUILD_DIR%\monolith_proxy.obj" "%BUILD_DIR%\monolith_proxy_offline.obj" winhttp.lib /Fe:"%OUT_EXE%" /link /Brepro
if errorlevel 1 (
  echo FAILED: monolith_proxy.exe build failed
  exit /b 1
)
if not exist "%OUT_EXE%" (
  echo FAILED: compiler did not create "%OUT_EXE%"
  exit /b 1
)

if not exist "..\..\Binaries" mkdir "..\..\Binaries"

REM A long-lived MCP client keeps the image file locked on Windows. Stage the
REM authoritative proxy under an immutable source-addressed name so rebuilding
REM never requires killing existing editor, game, Codex, or Claude sessions.
set "VERSIONED_NAME=monolith_proxy-%SOURCE_HASH%.exe"
set "VERSIONED_EXE=..\..\Binaries\%VERSIONED_NAME%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $source=(Resolve-Path '%OUT_EXE%').Path; $target=[IO.Path]::GetFullPath('%VERSIONED_EXE%'); $sha=[Security.Cryptography.SHA256]::Create(); try { $expected=(($sha.ComputeHash([IO.File]::ReadAllBytes($source))|ForEach-Object{$_.ToString('x2')})-join ''); if([IO.File]::Exists($target)){ $actual=(($sha.ComputeHash([IO.File]::ReadAllBytes($target))|ForEach-Object{$_.ToString('x2')})-join ''); if($actual -ne $expected){throw 'immutable proxy path already exists with different bytes: '+$target}; exit 0 }; $tmp=$target+'.tmp.'+[Guid]::NewGuid().ToString('N'); try { [IO.File]::Copy($source,$tmp,$false); $staged=(($sha.ComputeHash([IO.File]::ReadAllBytes($tmp))|ForEach-Object{$_.ToString('x2')})-join ''); if($staged -ne $expected){throw 'staged proxy hash mismatch'}; try{[IO.File]::Move($tmp,$target)}catch{if(-not [IO.File]::Exists($target)){throw}; $raced=(($sha.ComputeHash([IO.File]::ReadAllBytes($target))|ForEach-Object{$_.ToString('x2')})-join ''); if($raced -ne $expected){throw 'immutable proxy publish race produced different bytes: '+$target}} } finally { if([IO.File]::Exists($tmp)){[IO.File]::Delete($tmp)} } } finally { $sha.Dispose() }"
if errorlevel 1 (
  echo FAILED: could not atomically stage immutable proxy at %VERSIONED_EXE%
  exit /b 1
)

REM Publish the selected immutable image through a small atomic manifest. MCP
REM onboarding resolves this manifest and stores the exact versioned path.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $exe=(Resolve-Path '%VERSIONED_EXE%').Path; $v=(& $exe --version | Out-String | ConvertFrom-Json); if($v.source_hash -ne '%SOURCE_HASH%'){throw 'proxy self-reported source hash mismatch'}; if($v.tool -ne 'monolith-proxy' -or $v.runtime -ne 'native-cpp'){throw 'proxy self-reported identity mismatch'}; $manifest=Join-Path (Split-Path $exe -Parent) 'monolith_proxy.current.json'; $suffix=[Guid]::NewGuid().ToString('N'); $tmp=$manifest+'.tmp.'+$suffix; $previous=$manifest+'.replace-backup.'+$suffix; $shaAlg=[Security.Cryptography.SHA256]::Create(); try{$sha=(($shaAlg.ComputeHash([IO.File]::ReadAllBytes($exe))|ForEach-Object{$_.ToString('x2')})-join '')}finally{$shaAlg.Dispose()}; $payload=[ordered]@{schema_version=1;tool=$v.tool;runtime=$v.runtime;file='%VERSIONED_NAME%';version=$v.version;source_hash=$v.source_hash;sha256=$sha}; try{[IO.File]::WriteAllText($tmp,($payload|ConvertTo-Json -Compress),[Text.UTF8Encoding]::new($false)); if(Test-Path -LiteralPath $manifest){[IO.File]::Replace($tmp,$manifest,$previous,$true)}else{[IO.File]::Move($tmp,$manifest)}}finally{if(Test-Path -LiteralPath $tmp){Remove-Item -LiteralPath $tmp -Force}; if(Test-Path -LiteralPath $previous){Remove-Item -LiteralPath $previous -Force}}"
if errorlevel 1 (
  echo FAILED: could not publish monolith_proxy.current.json
  exit /b 1
)

REM Keep the historical fixed path as best-effort compatibility only. A lock is
REM expected when old clients are alive and is not a build failure.
copy /Y "%OUT_EXE%" "..\..\Binaries\monolith_proxy.exe" >nul 2>&1
if errorlevel 1 echo NOTE: legacy monolith_proxy.exe is in use; immutable %VERSIONED_NAME% is current.

echo Built current proxy: Plugins\Monolith\Binaries\%VERSIONED_NAME%
exit /b 0
