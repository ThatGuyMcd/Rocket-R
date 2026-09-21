@echo off
setlocal
cd /d "%~dp0"
title Rocket-R One Click Builder
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\OneClickBuild.ps1" %*
set "RC=%ERRORLEVEL%"
if "%RC%"=="20" (
    echo.
    echo Rocket-R setup needs the action described above before the build can continue.
    echo After completing that action, run ONE-CLICK-BUILD.cmd again.
    pause
    exit /b %RC%
)
if not "%RC%"=="0" (
    echo.
    echo Rocket-R build stopped safely with error code %RC%.
    echo See build\logs for the full log and the exact failing command.
    pause
)
exit /b %RC%
