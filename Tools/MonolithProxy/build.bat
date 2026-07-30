@echo off
REM Backward-compatible entry point. Keep toolchain discovery and error
REM handling in one authoritative script.
call "%~dp0build_proxy.bat"
exit /b %ERRORLEVEL%
