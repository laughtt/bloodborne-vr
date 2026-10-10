@echo off
setlocal
title Bloodborne VR (alpha)
cd /d "%~dp0"
if exist "python\python.exe" (
  "python\python.exe" "launcher\bbvr_launcher.py" %*
  goto done
)
where py >nul 2>nul
if not errorlevel 1 (
  py -3 "launcher\bbvr_launcher.py" %*
  goto done
)
where python >nul 2>nul
if not errorlevel 1 (
  python "launcher\bbvr_launcher.py" %*
  goto done
)
echo Python was not found. Use the full release zip (it has a python folder) or install Python 3.10+.
:done
echo.
pause
