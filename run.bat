@echo off
cd /d "%~dp0"
if not exist "build_dbg\Debug\r3000_emu.exe" (
    echo ERROR: build_dbg\Debug\r3000_emu.exe not found. Run cmake --build build_dbg --config Debug first.
    pause
    exit /b 1
)
build_dbg\Debug\r3000_emu.exe --bios=bios\ps1_bios.bin --hle --emu-log-level=info %*
echo Exit code: %ERRORLEVEL%
pause
