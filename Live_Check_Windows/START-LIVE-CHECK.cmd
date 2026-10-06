@echo off
setlocal
cd /d "%~dp0"
title Rocket Colour Studio - Read-only Live Check
echo ===============================================================
echo  Rocket Colour Studio 0.4.0 / 0.3.0 - Read-only Live Check
echo ===============================================================
echo.
echo Keep Rocket-R running IN GAMEPLAY with Rocket visible.
echo Leave the currently installed v0.4.0 or v0.3.0 mod enabled.
echo This checker does not change the game, mod, settings or saves.
echo.
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PS=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS%" (
    echo Windows PowerShell was not found.
    pause
    exit /b 1
)
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Collect-Rocket-Colours.ps1"
set "RESULT=%ERRORLEVEL%"
echo.
if not "%RESULT%"=="0" echo The checker reported a problem. The report includes the error when it could be saved.
echo The window will remain open so you can read the report location.
pause
exit /b %RESULT%
