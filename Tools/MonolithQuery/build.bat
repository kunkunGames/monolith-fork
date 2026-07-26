@echo off
REM Build monolith_query.exe — standalone SQLite query tool
REM Run from VS Developer Command Prompt, or let it find cl.exe via vswhere

REM Compute a SHA256 over every C++ input owned by this executable and inject
REM its first 16 hex chars as /DSOURCE_HASH. Keep this ordered source manifest
REM in sync with Scripts/check_offline_exe_fresh.py.
set SOURCE_HASH=dev
set SRCHASH_RAW=
set HASH_INPUT=%TEMP%\monolith_query_hash_%RANDOM%_%RANDOM%.bin
copy /b monolith_query.cpp+..\..\Source\MonolithIndex\Private\ProjectSearchQueryProjectionCore.h "%HASH_INPUT%" >nul
if %ERRORLEVEL% neq 0 (
    echo Failed to assemble the native source manifest for hashing.
    if exist "%HASH_INPUT%" del /q "%HASH_INPUT%"
    exit /b 1
)
for /f "skip=1 tokens=*" %%H in ('certutil -hashfile "%HASH_INPUT%" SHA256') do if not defined SRCHASH_RAW set SRCHASH_RAW=%%H
if exist "%HASH_INPUT%" del /q "%HASH_INPUT%"
if not defined SRCHASH_RAW (
    echo Failed to compute the native source-manifest hash.
    exit /b 1
)
REM Strip spaces certutil may insert between hex byte groups, take first 16 chars.
set SRCHASH_NOSPACE=%SRCHASH_RAW: =%
set SOURCE_HASH=%SRCHASH_NOSPACE:~0,16%
echo Source hash: %SOURCE_HASH%

where cl >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo Building with cl.exe...
    cl /EHsc /std:c++17 /O2 /MT /I ThirdParty /I ..\MonolithProxy\ThirdParty /DSQLITE_ENABLE_FTS5 /DSOURCE_HASH=\"%SOURCE_HASH%\" monolith_query.cpp ThirdParty\sqlite3.c /Fe:monolith_query.exe
    if %ERRORLEVEL% equ 0 goto :copy
    echo Build failed.
    exit /b 1
)

echo cl.exe not found in PATH.
echo Run from a Visual Studio Developer Command Prompt, or add cl.exe to PATH.
exit /b 1

:copy
if not exist ..\..\Binaries mkdir ..\..\Binaries
copy /Y monolith_query.exe ..\..\Binaries\monolith_query.exe
echo.
echo Built: Plugins\Monolith\Binaries\monolith_query.exe
