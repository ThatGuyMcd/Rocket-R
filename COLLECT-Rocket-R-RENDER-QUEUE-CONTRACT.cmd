@echo off
setlocal EnableExtensions
cd /d "%~dp0"
cls
echo ==============================================================================
echo   Rocket-R - READ-ONLY RENDER QUEUE CONTRACT COLLECTOR
echo ==============================================================================
echo This does NOT patch or edit Rocket-R source or generated code.
echo It extracts the exact queue users we need for the >256-entry repair.
echo.
set "PY="
for %%P in (py.exe python.exe python3.exe) do (
  if not defined PY where %%P >nul 2>nul && set "PY=%%P"
)
if not defined PY (
  echo Python was not found in PATH.
  echo.
  pause
  exit /b 1
)
if not exist "%~dp0collect_render_queue_contract.py" (
  echo Missing collect_render_queue_contract.py beside this CMD.
  echo.
  pause
  exit /b 1
)
%PY% "%~dp0collect_render_queue_contract.py" "%~dp0"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo Collector failed with exit code %RC%.
) else (
  echo Collector complete. Upload the ZIP path printed above to ChatGPT.
)
echo.
pause
exit /b %RC%
