@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "ROCKET_INTERPOLATION_TRACE=1"
set "TRACE_FILE=%CD%\Rocket-R-interpolation-coverage.log"

echo ==============================================================================
echo   Rocket-R Interpolation v35 - Runtime Coverage Trace
echo ==============================================================================
echo Trace file: "%TRACE_FILE%"
echo.

set "ROCKET_EXE="
if exist "%CD%\dist\Rocket-R.exe" set "ROCKET_EXE=%CD%\dist\Rocket-R.exe"
if not defined ROCKET_EXE if exist "%CD%\build\windows\Rocket-R.exe" set "ROCKET_EXE=%CD%\build\windows\Rocket-R.exe"
if not defined ROCKET_EXE (
    for /f "delims=" %%F in ('where /r "%CD%\dist" Rocket-R.exe 2^>nul') do if not defined ROCKET_EXE set "ROCKET_EXE=%%F"
)
if not defined ROCKET_EXE (
    for /f "delims=" %%F in ('where /r "%CD%\build\windows" Rocket-R.exe 2^>nul') do if not defined ROCKET_EXE set "ROCKET_EXE=%%F"
)
if not defined ROCKET_EXE (
    echo ERROR: Rocket-R.exe was not found under dist or build\windows.
    echo Build the Windows target once with ONE-CLICK-BUILD.cmd, then run this again.
    pause
    exit /b 1
)

echo Launching: "%ROCKET_EXE%"
echo Exercise Rocket's wheel, Hotdog wheels, machine pieces, tokens and slow/fast sky pans.
echo A summary is appended every 120 authored frames while this launcher is used.
echo.
"%ROCKET_EXE%"
set "RC=%ERRORLEVEL%"
echo.
echo Rocket-R exited with code %RC%.
if exist "%TRACE_FILE%" (
    echo Coverage log written to: "%TRACE_FILE%"
) else (
    echo No coverage log was written. Make sure the v35 build was actually launched.
)
pause
exit /b %RC%
