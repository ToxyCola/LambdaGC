@echo off
rem LambdaGC builder for Windows. Needs only Half-Life 2 on Steam, on the steam_legacy branch, and an
rem internet connection: it fetches a private copy of Python (into .tools\python), then build.py fetches
rem everything else it needs (into .tools) and builds LambdaGC.iso. Nothing is installed system-wide.
setlocal
cd /d "%~dp0"
set "PYDIR=%~dp0.tools\python"
set "PYVER=3.13.17"
if exist "%PYDIR%\python.exe" if exist "%PYDIR%\Scripts\pip.exe" goto run

echo [LambdaGC] fetching Python %PYVER% (once) ...
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; [Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;" ^
  "$d=$env:PYDIR; New-Item -ItemType Directory -Force $d | Out-Null;" ^
  "$z=Join-Path $env:TEMP 'lambdagc-python.zip';" ^
  "Invoke-WebRequest -UseBasicParsing ('https://www.python.org/ftp/python/'+$env:PYVER+'/python-'+$env:PYVER+'-embed-amd64.zip') -OutFile $z;" ^
  "Expand-Archive -Force $z $d; Remove-Item $z;" ^
  "$pth=Get-ChildItem $d -Filter 'python*._pth' | Select-Object -First 1;" ^
  "(Get-Content $pth.FullName) -replace '^#\s*import site','import site' | Set-Content -Encoding ascii $pth.FullName;" ^
  "Invoke-WebRequest -UseBasicParsing 'https://bootstrap.pypa.io/get-pip.py' -OutFile (Join-Path $d 'get-pip.py')"
if errorlevel 1 goto netfail
"%PYDIR%\python.exe" "%PYDIR%\get-pip.py" --no-warn-script-location
if errorlevel 1 goto netfail

:run
"%PYDIR%\python.exe" "%~dp0build.py" %*
if errorlevel 1 (
  echo.
  echo [LambdaGC] The build stopped - see the message above.
  pause
  exit /b 1
)
echo.
echo [LambdaGC] LambdaGC.iso is ready.
pause
exit /b 0

:netfail
echo.
echo [LambdaGC] Couldn't set up Python. Check your internet connection and run build.bat again.
pause
exit /b 1
