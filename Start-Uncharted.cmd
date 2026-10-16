@echo off
rem Uncharted: The Nathan Drake Collection - opens the launcher (no console window).
rem Python: pythonw on PATH, else the Python launcher (pyw), which python.org's installer puts
rem in C:\Windows even when Python itself is not added to PATH.
cd /d "%~dp0"
where pythonw >nul 2>nul && (start "" pythonw "launcher\uncharted_launcher.pyw" & exit /b 0)
where pyw >nul 2>nul && (start "" pyw "launcher\uncharted_launcher.pyw" & exit /b 0)
echo Python was not found. Install Python 3.10 or newer from https://www.python.org/downloads/
echo (keep "py launcher" ticked), then run: py -m pip install pillow
pause
