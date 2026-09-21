@echo off
cd /d "%~dp0"
call "%~dp0ONE-CLICK-BUILD.cmd" -NoPackage -NoLaunch
exit /b %ERRORLEVEL%
