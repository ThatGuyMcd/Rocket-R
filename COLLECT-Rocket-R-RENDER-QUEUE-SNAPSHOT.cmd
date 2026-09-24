@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

echo ==============================================================================
echo   Rocket-R Render Queue Snapshot Collector
echo ==============================================================================
echo Repository: %ROOT%
echo.

set "PYEXE="
if exist "C:\Python314\python.exe" set "PYEXE=C:\Python314\python.exe"
if not defined PYEXE where py >nul 2>nul && set "PYEXE=py -3"
if not defined PYEXE where python >nul 2>nul && set "PYEXE=python"
if not defined PYEXE (
  echo [ERROR] Python was not found.
  pause
  exit /b 1
)

if "%PYEXE%"=="py -3" (
  py -3 "%ROOT%\Payload\collect_render_queue_snapshot.py" "%ROOT%"
) else (
  "%PYEXE%" "%ROOT%\Payload\collect_render_queue_snapshot.py" "%ROOT%"
)
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo Snapshot collection failed with exit code %RC%.
pause
exit /b %RC%
