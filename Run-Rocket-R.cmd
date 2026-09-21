@echo off
setlocal
cd /d "%~dp0"
set "EXE="
for /r "%~dp0build\windows" %%F in (Rocket-R.exe) do if not defined EXE set "EXE=%%~fF"
if not defined EXE (
  echo Rocket-R.exe has not been built yet.
  echo Run ONE-CLICK-BUILD.cmd first.
  pause
  exit /b 3
)
set "ROM=%~dp0build\private\rocket.us.z64"
if exist "%ROM%" (
  "%EXE%" --rom "%ROM%"
) else (
  "%EXE%"
)
exit /b %ERRORLEVEL%
