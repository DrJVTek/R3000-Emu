@echo off
setlocal

rem Restores the intended UE5 plugin link layout:
rem 1. Repo plugin Source is a real directory from git
rem 2. Repo plugin Private\src is a symlink to repo\src
rem 3. PSXVR plugin Source is a symlink to repo plugin Source

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_ROOT=%%~fI"

if "%~1"=="" (
    echo Usage: %~nx0 ^<PSXVR_ROOT^>
    echo Example: %~nx0 E:\Projects\github\Live\PSXVR
    exit /b 1
)

set "PSXVR_ROOT=%~1"
set "PLUGIN_ROOT=%REPO_ROOT%\integrations\ue5\R3000Emu"
set "PLUGIN_SOURCE=%PLUGIN_ROOT%\Source"
set "PLUGIN_PRIVATE_SRC=%PLUGIN_SOURCE%\R3000EmuRuntime\Private\src"
set "CORE_SRC=%REPO_ROOT%\src"
set "PSXVR_PLUGIN_SOURCE=%PSXVR_ROOT%\Plugins\R3000Emu\Source"

echo.
echo [1/5] Restoring repo plugin Source from git...
cmd /c rmdir "%PLUGIN_SOURCE%" 2>nul
git -C "%REPO_ROOT%" restore integrations/ue5/R3000Emu/Source
if errorlevel 1 (
    echo ERROR: git restore failed for repo plugin Source.
    exit /b 1
)

echo.
echo [2/5] Recreating repo plugin Private\src symlink...
if exist "%PLUGIN_PRIVATE_SRC%" (
    cmd /c rmdir "%PLUGIN_PRIVATE_SRC%" 2>nul
    if exist "%PLUGIN_PRIVATE_SRC%" rmdir /s /q "%PLUGIN_PRIVATE_SRC%"
)
cmd /c mklink /D "%PLUGIN_PRIVATE_SRC%" "%CORE_SRC%"
if errorlevel 1 (
    echo ERROR: mklink failed for repo Private\src.
    exit /b 1
)

echo.
echo [3/5] Recreating PSXVR plugin Source symlink...
if exist "%PSXVR_PLUGIN_SOURCE%" (
    cmd /c rmdir "%PSXVR_PLUGIN_SOURCE%" 2>nul
    if exist "%PSXVR_PLUGIN_SOURCE%" rmdir /s /q "%PSXVR_PLUGIN_SOURCE%"
)
cmd /c mklink /D "%PSXVR_PLUGIN_SOURCE%" "%PLUGIN_SOURCE%"
if errorlevel 1 (
    echo ERROR: mklink failed for PSXVR plugin Source.
    exit /b 1
)

echo.
echo [4/5] Repo plugin state:
cmd /c dir "%PLUGIN_ROOT%"

echo.
echo [5/5] PSXVR plugin state:
cmd /c dir "%PSXVR_ROOT%\Plugins\R3000Emu"

echo.
echo Done.
exit /b 0
