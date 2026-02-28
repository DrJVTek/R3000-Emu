@echo off
setlocal
set REPO_ROOT=%~dp0..
set BUILD_DIR=%REPO_ROOT%\build
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

if not exist "%BUILD_DIR%" (
    echo [R3000] build/ not found, configuring...
    cmake -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -S "%REPO_ROOT%"
)

echo [R3000] Building r3000_core.lib (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config %CONFIG% --target r3000_core
if %ERRORLEVEL% equ 0 (
    echo [R3000] OK: lib\%CONFIG%\r3000_core.lib
) else (
    echo [R3000] BUILD FAILED
    exit /b 1
)
