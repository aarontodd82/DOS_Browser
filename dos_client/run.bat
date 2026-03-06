@echo off
echo ============================================
echo  RetroSurf - Launching DOSBox-X
echo ============================================
echo.

set "SCRIPT_DIR=%~dp0"
set "DOSBOX_EXE=%SCRIPT_DIR%..\tools\dosbox-x\mingw-build\mingw\dosbox-x.exe"

if not exist "%DOSBOX_EXE%" (
    echo ERROR: DOSBox-X not found at:
    echo   %DOSBOX_EXE%
    echo.
    echo Make sure the tools were set up correctly.
    pause
    exit /b 1
)

echo REMINDER: Start the server first in another window!
echo   cd %SCRIPT_DIR%..\pi_server
echo   python server.py
echo.
echo Starting DOSBox-X...

start "" "%DOSBOX_EXE%" -conf "%SCRIPT_DIR%dosbox-x.conf"
