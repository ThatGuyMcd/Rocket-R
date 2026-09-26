@echo off
setlocal EnableExtensions
set "SDK=%LOCALAPPDATA%\Android\Sdk"
set "ADB=%SDK%\platform-tools\adb.exe"
if not exist "%ADB%" (
  echo Android adb was not found at:
  echo   %ADB%
  echo Run the Rocket-R Android builder once so platform-tools are installed.
  pause
  exit /b 1
)

echo.
echo Rocket-R Android crash capture
echo ================================
echo Connect the Android device with USB debugging enabled and accept the device prompt.
echo.
"%ADB%" devices
echo.
echo Clearing existing Android logs...
"%ADB%" logcat -c
"%ADB%" logcat -b crash -c 2>nul

echo.
echo Launch Rocket-R on the device, choose/play the ROM, and reproduce any crash.
echo When Rocket-R closes or fails, return here and press any key.
pause >nul

set "LOGDIR=%~dp0build\logs"
if not exist "%LOGDIR%" mkdir "%LOGDIR%"
set "OUT=%LOGDIR%\rocket-r-android-crash.txt"
set "CRASH=%LOGDIR%\rocket-r-android-crash-buffer.txt"
set "PKG=%LOGDIR%\rocket-r-android-package.txt"

"%ADB%" logcat -d -v threadtime > "%OUT%"
"%ADB%" logcat -b crash -d -v threadtime > "%CRASH%" 2>nul
"%ADB%" shell dumpsys package com.rocketret.rocketr > "%PKG%" 2>nul

echo.
echo Saved:
echo   %OUT%
echo   %CRASH%
echo   %PKG%
echo.
echo If Rocket-R still crashes, upload all three files. The crash buffer normally
echo includes the native signal, libmain.so address/backtrace and Vulkan/SDL errors.
pause
