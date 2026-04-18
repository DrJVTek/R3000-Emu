@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%..\.."
set "VENV_DIR=%SCRIPT_DIR%.venv"

if not exist "%VENV_DIR%\Scripts\python.exe" (
  echo [classifier] missing venv at "%VENV_DIR%"
  echo [classifier] run create_venv.bat first
  exit /b 1
)

call "%VENV_DIR%\Scripts\activate.bat"
if errorlevel 1 (
  echo [classifier] failed to activate venv
  exit /b 1
)

pushd "%REPO_ROOT%"
python -m scripts.classifier %*
set "EXIT_CODE=%ERRORLEVEL%"
popd

endlocal & exit /b %EXIT_CODE%
