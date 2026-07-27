@echo off
setlocal EnableExtensions
REM Build monolith_query.exe — standalone SQLite query tool
REM Run from a VS Developer Command Prompt; cl.exe and link.exe must be on PATH.

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

set QUERY_OBJ=monolith_query.build.obj
set SQLITE_OBJ=sqlite3.build.obj
set BUILD_EXE=monolith_query.build.exe
where cl >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo Building with cl.exe...
    if exist "%QUERY_OBJ%" del /q "%QUERY_OBJ%"
    if exist "%SQLITE_OBJ%" del /q "%SQLITE_OBJ%"
    if exist "%BUILD_EXE%" del /q "%BUILD_EXE%"

    cl /nologo /EHsc /std:c++17 /O2 /MT /I ThirdParty /I ..\MonolithProxy\ThirdParty /DSQLITE_ENABLE_FTS5 /DSOURCE_HASH=\"%SOURCE_HASH%\" /c monolith_query.cpp /Fo:"%QUERY_OBJ%"
    if errorlevel 1 (
        echo Build failed while compiling monolith_query.cpp.
        goto :build_failed
    )
    cl /nologo /O2 /MT /DSQLITE_ENABLE_FTS5 /c ThirdParty\sqlite3.c /Fo:"%SQLITE_OBJ%"
    if errorlevel 1 (
        echo Build failed while compiling sqlite3.c.
        goto :build_failed
    )
    link /nologo /OUT:"%BUILD_EXE%" "%QUERY_OBJ%" "%SQLITE_OBJ%"
    if errorlevel 1 (
        echo Build failed while linking monolith_query.exe.
        goto :build_failed
    )
    if not exist "%BUILD_EXE%" (
        echo Build failed: linker reported success without producing "%BUILD_EXE%".
        goto :build_failed
    )
    move /Y "%BUILD_EXE%" monolith_query.exe >nul
    if errorlevel 1 (
        echo Build failed while replacing monolith_query.exe.
        goto :build_failed
    )
    if exist "%QUERY_OBJ%" del /q "%QUERY_OBJ%"
    if exist "%SQLITE_OBJ%" del /q "%SQLITE_OBJ%"
    goto :copy
)

echo cl.exe not found in PATH.
echo Run from a Visual Studio Developer Command Prompt, or add cl.exe to PATH.
exit /b 1

:build_failed
if exist "%QUERY_OBJ%" del /q "%QUERY_OBJ%"
if exist "%SQLITE_OBJ%" del /q "%SQLITE_OBJ%"
if exist "%BUILD_EXE%" del /q "%BUILD_EXE%"
exit /b 1

:copy
if not exist ..\..\Binaries mkdir ..\..\Binaries
copy /Y monolith_query.exe ..\..\Binaries\monolith_query.exe
if errorlevel 1 (
    echo Failed to copy monolith_query.exe into Binaries.
    exit /b 1
)
echo.
echo Built: Plugins\Monolith\Binaries\monolith_query.exe
exit /b 0
