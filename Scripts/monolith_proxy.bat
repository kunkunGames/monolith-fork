@echo off
:: Monolith MCP Proxy launcher
:: Finds a working runtime automatically and runs the proxy.
:: Usage in .mcp.json:
::   {"mcpServers": {"monolith": {"command": "<project-root>/Plugins/Monolith/Scripts/monolith_proxy.bat"}}}

setlocal

if /I "%~1"=="--help" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="help" goto :help

if not exist "%~dp0monolith_proxy.py" if not exist "%~dp0monolith_proxy.js" (
    echo [monolith-proxy] ERROR: proxy scripts not found at %~dp0 1>&2
    exit /b 1
)

:: Try system Python first. Probe execution because stale py launcher registry
:: entries can make "where" succeed while process creation fails.
where python >nul 2>&1
if errorlevel 1 goto :try_python3
python -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if errorlevel 1 goto :try_python3
if exist "%~dp0monolith_proxy.py" (
    python "%~dp0monolith_proxy.py" %*
    exit /b %errorlevel%
)

:try_python3
:: Try python3 (common on some setups)
where python3 >nul 2>&1
if errorlevel 1 goto :try_node
python3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if errorlevel 1 goto :try_node
if exist "%~dp0monolith_proxy.py" (
    python3 "%~dp0monolith_proxy.py" %*
    exit /b %errorlevel%
)

:try_node
:: Fallback to Node.js when Python is unavailable or broken.
where node >nul 2>&1
if errorlevel 1 goto :try_py
node -e "process.exit(parseInt(process.versions.node) >= 18 ? 0 : 1)" >nul 2>&1
if errorlevel 1 goto :try_py
if exist "%~dp0monolith_proxy.js" (
    node "%~dp0monolith_proxy.js" %*
    exit /b %errorlevel%
)

:try_py
:: Try py launcher last. Some broken registry entries report success during
:: probing, so prefer direct Python and Node when they are available.
where py >nul 2>&1
if errorlevel 1 goto :fail
py -3 -c "import sys; raise SystemExit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>&1
if errorlevel 1 goto :fail
if exist "%~dp0monolith_proxy.py" (
    py -3 "%~dp0monolith_proxy.py" %*
    exit /b %errorlevel%
)

:fail
echo [monolith-proxy] ERROR: Python 3.8+ or Node.js not found. Install Python 3.8+ or Node.js 18+. 1>&2
exit /b 1

:help
echo Usage:
echo   monolith_proxy.bat --help
echo   monolith_proxy.bat --version
echo   monolith_proxy.bat
echo.
echo Role:
echo   Thin Windows launcher for Scripts\monolith_proxy.py or Scripts\monolith_proxy.js.
echo   The selected script is a stdio-to-HTTP MCP bridge for MONOLITH_URL.
echo.
echo Runtime selection:
echo   Prefers Python 3.8+, then python3, then Node.js 18+, then py -3.
echo   Help exits before probing runtimes; version is forwarded to the selected runtime.
echo.
echo Offline fallback:
echo   Use Binaries\monolith_query.exe for read-only source/project/bridge queries when the editor or MCP server is unavailable.
exit /b 0
