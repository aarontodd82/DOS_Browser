@echo off
echo Setting up RetroSurf Pi Server environment...

python -m venv venv
if errorlevel 1 (
    echo ERROR: Failed to create virtual environment. Is Python installed?
    pause
    exit /b 1
)

echo Installing Python dependencies...
venv\Scripts\pip install -r requirements.txt
if errorlevel 1 (
    echo ERROR: Failed to install dependencies.
    pause
    exit /b 1
)

echo Installing Playwright Chromium browser...
venv\Scripts\playwright install chromium
if errorlevel 1 (
    echo ERROR: Failed to install Chromium.
    pause
    exit /b 1
)

echo.
echo Setup complete! To run the test:
echo   venv\Scripts\python test_pipeline.py
echo.
pause
