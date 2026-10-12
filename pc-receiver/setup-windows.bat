@echo off
setlocal
cd /d "%~dp0"
py -3 --version >nul 2>&1
if errorlevel 1 (
    echo Install Python 3.10 or newer from python.org, including the Python launcher.
    pause
    exit /b 1
)
if not exist ".venv\Scripts\python.exe" py -3 -m venv .venv
if errorlevel 1 exit /b 1
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 (
    pause
    exit /b 1
)
echo Setup complete. Run start-windows.bat.
