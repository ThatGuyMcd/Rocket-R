@echo off
setlocal EnableExtensions
cd /d "%~dp0"
echo ==============================================================================
echo   Rocket-R Render Queue / Pool Audit - READ ONLY
echo ==============================================================================
echo.
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0collect_queue_pool_audit.py" "D:\Rocket-R"
) else (
  python "%~dp0collect_queue_pool_audit.py" "D:\Rocket-R"
)
set ERR=%errorlevel%
echo.
if not "%ERR%"=="0" echo Audit failed with exit code %ERR%.
if "%ERR%"=="0" echo Audit complete. Upload the ZIP path printed above to ChatGPT.
echo.
pause
exit /b %ERR%
