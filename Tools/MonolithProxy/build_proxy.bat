@echo off
setlocal
call "%~dp0build.bat"
exit /b %ERRORLEVEL%
