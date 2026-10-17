#!/usr/bin/env bash
# Installs this checkout into the play folder that the player and the test runs both start from
# (default F:/Uncharted-PC; the first argument or UNCHARTED_PLAY_DIR picks another):
#   1. tools/package_windows.sh builds the package, keeping the runtime's debug information
#      (crash reports) and without the zip,
#   2. the package's tools/apply_update.py installs it as a launcher update does: the program
#      folders are replaced; analysis\ (saves, settings, launcher.json, shader cache, logs, reports)
#      stays,
#   3. BUILD.txt in the play folder names the commit installed; analysis/play-folder.txt in this
#      checkout names the play folder (Start-Uncharted.cmd opens that one), unless the folder was
#      given as an argument (a test copy beside the player's).
# Start "<play folder>\Uncharted Launcher.exe" for the launcher, or add --play u1|u2|u3 for a game
# with the saved settings and no launcher window (it returns when the game ends).
# Refuses while anything runs from the play folder. Build the runtime first (gow3_pc/build.sh);
# run from Git Bash or an MSYS2 CLANG64 shell.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
play=${1:-${UNCHARTED_PLAY_DIR:-F:/Uncharted-PC}}
play_win=$(cygpath -w "$play")
running=$(powershell -NoProfile -Command \
    "@(Get-Process | Where-Object { \$_.Path -like '${play_win%\\}\\*' }).Count" | tr -d '\r')
if [[ ${running:-0} != 0 ]]; then
    echo "$running processes run from $play_win: close the game and the launcher first." >&2
    exit 1
fi
# The package carries a Windows Python with Pillow: `python` on PATH, else the one `py` starts
# (Windows' "python" app alias only opens the Store).
if [[ -z ${PYTHON_HOME:-} ]] && ! python -c 'import sys' > /dev/null 2>&1; then
    PYTHON_HOME=$(py -3 -c 'import os, sys; print(os.path.dirname(sys.executable))' | tr -d '\r')
    export PYTHON_HOME
fi
PACKAGE_SYMBOLS=1 PACKAGE_ZIP=0 bash tools/package_windows.sh
mkdir -p "$play"
dist/Uncharted-windows/python/python.exe -I dist/Uncharted-windows/tools/apply_update.py \
    --root "$play_win" --no-restart --quiet
# What the install replaced (update\old): an update leaves it for the next launcher to remove.
rm -rf -- "$play/update"
build="$(git log -1 --format='%h %cd %s' --date=format:'%Y-%m-%d %H:%M')"
if [[ -n $(git status --porcelain --untracked-files=no -- . ':!gow3_pc/gpu/third_party') ]]; then
    build+=" (with uncommitted changes)"
fi
printf '%s\n' "$build" > "$play/BUILD.txt"
if [[ $# -eq 0 ]]; then
    mkdir -p analysis
    printf '%s\r\n' "$play_win" > analysis/play-folder.txt
fi
echo "Installed into $play_win: $build"
