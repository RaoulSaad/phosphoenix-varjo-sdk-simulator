@echo off
setlocal
set "ENV_PY=%USERPROFILE%\.conda\envs\phos-rtcv\python.exe"
if not exist "%ENV_PY%" (
  echo Conda env phos-rtcv not found at %ENV_PY%
  echo Edit this file or set the python path in the launcher's Advanced section.
  pause
  exit /b 1
)
cd /d "%~dp0"
start "Phosphene simulator launcher" /min "%ENV_PY%" launcher\app.py
