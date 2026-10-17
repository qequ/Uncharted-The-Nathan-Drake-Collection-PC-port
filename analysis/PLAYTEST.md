# Play testing

How to start the games, play, and keep what is needed when something goes wrong. The game files:
`F:\Uncharted - The Nathan Drake Collection (CUSA02343) [v01.00]` (the folder with `eboot.bin`
and `sce_sys`).

## Starting a game

**One way for playing and for tests: the play folder `F:\Uncharted-PC`.** It is this repository
built as the release package: `Uncharted Launcher.exe` with its own Python (nothing else needs to
be installed), the runtime, and under `analysis` the player's saves, settings, shader cache, logs
and reports. Start it with the desktop shortcut **Uncharted** (`Start-Uncharted.cmd` in the
repository opens it too). Pick a game (1, 2, 3), a preset (Medium is 1080p at 60 FPS) and press
**PLAY**. The first start of each game prepares its files and compiles shaders: expect a slow first
load and some stutter. The launcher keeps its own options there as well
(`analysis\uncharted-user\launcher.json`; the first start takes them over from
`%APPDATA%\Uncharted Launcher`, where earlier versions kept them). The Advanced tab has
**Open log folder** and **Crash reports**. Updates come from this fork's releases (none yet), never
from upstream's, which would drop this fork's changes.

**Bringing it up to the repository's code**: build the runtime (`bash gow3_pc/build.sh`), close the
game and the launcher, then run `bash tools/install_local.sh` in Git Bash. It packages this checkout
and installs it the way a launcher update does: the program is replaced; saves, settings, shader
cache, logs and reports stay. `F:\Uncharted-PC\BUILD.txt` names the commit installed.

**Tests** run the same build from a copy beside it (`bash tools/install_local.sh
F:/Uncharted-PC-test`), with the player's settings and saves copied in, and start it the same way:
`"Uncharted Launcher.exe" --play u2` (`u1`, `u2`, `u3`) starts a game with the saved settings and no
launcher window, as PLAY does, and returns when the game ends. A test started from the Claude app
goes through a shortcut that Windows Explorer opens: the app's own processes see a private copy of
AppData (it is an MSIX package), and the game must not.

**From the repository, for development only** (its own `analysis` folder, separate saves): the
launcher with `py launcher\uncharted_launcher.pyw`, or a game without it:

```
cd /d D:\ps4_ports\Uncharted-The-Nathan-Drake-Collection-PC-port
py tools\probe_uncharted.py --boot --seconds 0 --game "F:\Uncharted - The Nathan Drake Collection (CUSA02343) [v01.00]" --executable big3-ps4_Shipping.elf
```

`--executable eboot.bin` is Uncharted 1 (the default), `big2-ps4_Shipping.elf` Uncharted 2,
`big3-ps4_Shipping.elf` Uncharted 3; `--collection-menu` starts the Collection's game menu instead.
`--seconds 0` means no time limit. This skips the launcher's options (render resolution,
multithreading): results can differ from the play folder's.

Closing the game window ends the game; the launcher's STOP does the same.

## Controls

Controllers (DualSense, DualShock 4, Xbox) work as on the console. Keyboard and mouse, by default
(the launcher's Controls tab and the in-game page below change them per game):

| PS4 button | Key | Use |
|---|---|---|
| Left stick | W A S D | move |
| Right stick | mouse, arrow keys | look |
| Cross | Space | jump, climb |
| Circle | Left Ctrl | cover, roll, drop |
| Square | F | melee |
| Triangle | R | reload, pick up |
| L2 / R2 | right / left mouse button | aim / fire |
| L1 / R1 | Q / E (mouse back / forward) | grenade aim / throw |
| L3 / R3 | Left Shift / V (middle mouse) | |
| D-pad | 1 2 3 4, mouse wheel | weapons |
| Options | Esc or Enter | pause menu |
| Touchpad | Tab / Backspace | |

**Changing the keys in game**: Options, Controls, **Controller Layout** shows a keyboard and mouse
page while you play with them (with a controller, the game's own picture). Select an action, press
**Enter**, then the new key or mouse button (**Esc** cancels); **Delete** clears it. A key already
in use moves over and the other action takes the old key. Mouse sensitivity, mouse vertical
(Normal / Flipped), the button icons and **Reset Defaults** are there too. Changes apply at once
and are kept per game (`analysis\uncharted-user\probe-u1.ini`, `-u2`, `-u3`), where the launcher's
Controls tab reads and changes them too (an open launcher shows the page's changes at once).
**Esc** (or the key of Circle) goes back.

The games' button prompts ("Press F to attack") show the key or mouse button that presses them, or
an Xbox controller's letters, following what you used last. The page's **Button Icons** choice
(Automatic, Keyboard, Xbox, PlayStation) fixes one; `GOW3_GLYPHS=keyboard` (`xbox`, `playstation`)
does the same for a test.

**Insert** (or R3 + L2 on a controller) opens the port's own menu (display, frame rate, overlay).
**F8** marks a moment for a report (below).

## When something goes wrong

Every report is a folder in `analysis\reports` of the folder the game runs from
(`F:\Uncharted-PC\analysis\reports`), named `<date> <time> <game> <kind>`. All paths below are in
that folder.

| Kind | When | What happens |
|---|---|---|
| `crash` | the game closes by itself | the launcher says "The game crashed" with the summary and offers to open the folder |
| `freeze` | no new frame for 20 seconds | written once per freeze while the game keeps running; close it if it never recovers |
| `mark N` | you press **F8** | for anything that looks wrong (graphics, sound, a stuck character): "Mark N saved" shows at the top of the screen |

In each folder:

- `report.txt`: the game and version, when, the **save last written or read** (the games name the
  chapter in it: "Campaign 12% Complete - Another Round"), the **level packages last loaded**
  (they name the place: `airport-2.pak`), and the files opened just before. For a crash: what
  failed, the thread, the call stack (game code as `big3-ps4_Shipping.elf+0x...`, an address in
  that executable; runtime functions by name and line). For a freeze or mark: every thread's call
  stack and CPU use.
- `screen.jpg`: the screen at the mark or freeze, or within 10 seconds before a crash
  (`screen-10s-earlier.jpg` too): the frame the game presented, in any display mode.
- `log-tail.txt`: the end of the game's log. `settings.txt`: every setting of that run and the
  source commit. `minidump.dmp` (crash): for a debugger.

Add a `notes.txt` with what you did and saw. To hand a report over, name its folder: the files stay
on this disk. The full log of the last run of each game is `analysis\uncharted\boot.log`
(`big2-ps4_Shipping`, `big3-ps4_Shipping`), and every run's log is kept under `attempts`.

## Settings for testing

`analysis\uncharted-user\test-env.txt` (every game) and `test-env-u1.txt`, `-u2`, `-u3` (one game)
hold `NAME=value` lines that override everything for the next start; delete the line or the file
to go back. Useful ones:

| Setting | |
|---|---|
| `GOW3_ASYNC_READBACK=0` | GPU readbacks: the CPU waits for them (correct, slower) |
| `GOW3_ASYNC_READBACK=1` | readbacks in the background, pages read-protected until they arrive: **the default of Uncharted 2 and 3** since 0.1.2 |
| `GOW3_ASYNC_READBACK=2` | readbacks in the background, unprotected: the default of Uncharted 1. It crashed Uncharted 3 (pub fight, Cartagena museum) |
| `GOW3_FREEZE_SECONDS=30` | how long without a new frame counts as a freeze (0: no freeze reports) |
| `GOW3_MARK_KEY=F7` | another mark key (an SDL key name; `off` turns it off) |
| `GOW3_MINIDUMP=0` | no minidump with crash reports |

## Known problems (16 October 2026, version 0.1.2)

- Each game first says "Friend Stats are not available when offline": expected, there is no PSN.
- Uncharted 2's heaviest scenes run at about 50 FPS. Keep "Frames queued" at 1: with 2, GPU results
  reach the game a frame late and Uncharted 2's NPCs float. Multithreaded rendering is still
  experimental.
- Uncharted 3: Sully's shirt in the bar can flicker at 60 FPS (not at 30).
- Uncharted 1 renders at most 2880x1620 and upscales above that.
- Uncharted 3 has been played the least. The full list: [ROADMAP.md](ROADMAP.md) and
  [STATUS.md](STATUS.md).
