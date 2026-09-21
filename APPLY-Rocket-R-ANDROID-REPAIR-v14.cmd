@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Apply-Rocket-R-Android-Repair-v14.ps1"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
    echo Repair failed with exit code %RC%.
) else (
    echo Repair completed successfully.
)
echo.
pause
exit /b %RC%
