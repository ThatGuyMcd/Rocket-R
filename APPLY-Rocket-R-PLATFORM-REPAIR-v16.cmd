@echo off
setlocal
cd /d "%~dp0"
echo.
echo ============================================================================
echo   Rocket-R Platform Repair v16 - Android + Linux / Steam Deck
echo ============================================================================
echo.
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Apply-Rocket-R-Platform-Repair-v16.ps1" -ProjectRoot "%CD%"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo Repair v16 FAILED with exit code %RC%.
) else (
  echo Repair v16 completed successfully.
)
echo.
pause
exit /b %RC%
