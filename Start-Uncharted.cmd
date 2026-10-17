@echo off
rem Uncharted: The Nathan Drake Collection - opens the launcher (no console window).
rem The installed play folder first (tools/install_local.sh names it in analysis\play-folder.txt):
rem its "Uncharted Launcher.exe" carries its own Python, and saves and settings live there.
rem Else this checkout's launcher, with pythonw on PATH or the pythonw of the Python that the
rem Python launcher (py) picks; python.org's installer puts py in C:\Windows even when Python itself
rem is not on PATH. pythonw starts from here, not through pyw: a window opened by pyw's child
rem process can come up behind the window in front, and the launcher then seems not to start.
cd /d "%~dp0"
set "PLAY="
if exist "analysis\play-folder.txt" set /p PLAY=<"analysis\play-folder.txt"
if defined PLAY if exist "%PLAY%\Uncharted Launcher.exe" (start "" "%PLAY%\Uncharted Launcher.exe" & exit /b 0)
where pythonw >nul 2>nul && (start "" pythonw "launcher\uncharted_launcher.pyw" & exit /b 0)
set "PYTHONW="
for /f "delims=" %%p in ('py -3 -c "import sys, pathlib; print(pathlib.Path(sys.executable).with_name('pythonw.exe'))" 2^>nul') do set "PYTHONW=%%p"
if defined PYTHONW if exist "%PYTHONW%" (start "" "%PYTHONW%" "launcher\uncharted_launcher.pyw" & exit /b 0)
echo Python was not found. Install Python 3.10 or newer from https://www.python.org/downloads/
echo (keep "py launcher" ticked), then run: py -m pip install pillow
pause
