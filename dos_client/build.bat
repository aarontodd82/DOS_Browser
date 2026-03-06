@echo off
echo Building RetroSurf DOS client...
echo.

set "PROJECT_DIR=%~dp0"
set "TOOLS_DIR=%PROJECT_DIR%..\tools"
set "GNUMAKE=%TOOLS_DIR%\watt32\util\win32\gnumake.exe"

if not exist "%GNUMAKE%" (
    echo ERROR: gnumake.exe not found at %GNUMAKE%
    echo Run the Watt-32 setup first.
    exit /b 1
)

cd /d "%PROJECT_DIR%"
"%GNUMAKE%" -f Makefile %*

if %ERRORLEVEL% EQU 0 (
    echo.
    echo Build successful!
    dir /b build\RETRO.EXE 2>nul
) else (
    echo.
    echo Build FAILED. Check errors above.
)
