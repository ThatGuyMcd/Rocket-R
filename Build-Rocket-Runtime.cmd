@echo off
cd /d "%~dp0"
call "%~dp0ONE-CLICK-BUILD.cmd" %*
exit /b %ERRORLEVEL%
