@echo off
rem Uncharted: The Nathan Drake Collection - opens the launcher (no console window).
rem Python: pythonw on PATH, else the pythonw of the Python that the Python launcher (py) picks;
rem python.org's installer puts py in C:\Windows even when Python itself is not on PATH.
rem pythonw starts from here, not through pyw: a window opened by pyw's child process can come up
rem behind the window in front, and the launcher then seems not to start.
cd /d "%~dp0"
where pythonw >nul 2>nul && (start "" pythonw "launcher\uncharted_launcher.pyw" & exit /b 0)
set "PYTHONW="
for /f "delims=" %%p in ('py -3 -c "import sys, pathlib; print(pathlib.Path(sys.executable).with_name('pythonw.exe'))" 2^>nul') do set "PYTHONW=%%p"
if defined PYTHONW if exist "%PYTHONW%" (start "" "%PYTHONW%" "launcher\uncharted_launcher.pyw" & exit /b 0)
echo Python was not found. Install Python 3.10 or newer from https://www.python.org/downloads/
echo (keep "py launcher" ticked), then run: py -m pip install pillow
pause
