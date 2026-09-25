@echo off
setlocal
set "ROOT=%~1"
if not defined ROOT set "ROOT=D:\Rocket-R"
where py >nul 2>nul
if %errorlevel%==0 (
  py -3 "%~dp0apply_platform_launch_repair_v19.py" --root "%ROOT%" --verify-only
) else (
  python "%~dp0apply_platform_launch_repair_v19.py" --root "%ROOT%" --verify-only
)
pause
