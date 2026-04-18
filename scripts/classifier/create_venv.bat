@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "VENV_DIR=%SCRIPT_DIR%.venv"

echo [classifier] creating virtual environment in "%VENV_DIR%"
python -m venv "%VENV_DIR%"
if errorlevel 1 (
  echo [classifier] failed to create venv
  exit /b 1
)

call "%VENV_DIR%\Scripts\activate.bat"
if errorlevel 1 (
  echo [classifier] failed to activate venv
  exit /b 1
)

python -m pip install --upgrade pip
if errorlevel 1 (
  echo [classifier] failed to upgrade pip
  exit /b 1
)

python -m pip install -r "%SCRIPT_DIR%requirements.txt"
if errorlevel 1 (
  echo [classifier] failed to install requirements
  exit /b 1
)

echo.
echo [classifier] venv ready.
echo [classifier] activate with:
echo   call "%VENV_DIR%\Scripts\activate.bat"
echo [classifier] then run:
echo   python -m scripts.classifier --help

endlocal
