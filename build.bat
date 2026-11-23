@echo off
rem LambdaGC builder for Windows: converts your Steam copy of Half-Life 2 (steam_legacy branch)
rem and builds LambdaGC.iso. Needs Python 3 and devkitPro - see README.md.
setlocal
cd /d "%~dp0"
where py >nul 2>nul && (set PY=py -3) || (set PY=python)
%PY% --version >nul 2>nul || (
  echo Python 3 was not found. Install it from https://www.python.org/downloads/ ^(tick "Add python.exe to PATH"^).
  pause
  exit /b 1
)
%PY% build.py %*
if errorlevel 1 (
  echo.
  echo The build failed - see the messages above.
  pause
  exit /b 1
)
pause
