@echo off
setlocal EnableExtensions
pushd "%~dp0" >nul

echo ==============================================================================
echo   Rocket-R Graphics v26 - ROLLBACK LAST INSTALL
 echo ==============================================================================
echo This restores the repository files captured immediately before the last v26 run.
echo.

set "PYMODE="
where python >nul 2>&1
if not errorlevel 1 set "PYMODE=python"
if not defined PYMODE (
    where py >nul 2>&1
    if not errorlevel 1 set "PYMODE=py"
)
if not defined PYMODE (
    echo [ERROR] Python 3 was not found in PATH.
    popd >nul
    pause
    exit /b 1
)

if /I "%PYMODE%"=="python" (
    python "%~dp0Payload\scripts\apply_graphics_v26.py" --root "." --rollback-latest
) else (
    py -3 "%~dp0Payload\scripts\apply_graphics_v26.py" --root "." --rollback-latest
)
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" (
    echo [OK] Last v26 install rolled back.
) else (
    echo [ERROR] Rollback failed with exit code %RC%.
)
popd >nul
pause
exit /b %RC%
