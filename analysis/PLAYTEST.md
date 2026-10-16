# Play testing

How to start the games from this source tree, play, and keep what is needed when something goes
wrong. The game files: `F:\Uncharted - The Nathan Drake Collection (CUSA02343) [v01.00]` (the folder
with `eboot.bin` and `sce_sys`).

## Starting a game

**With the launcher.** Double-click `Start-Uncharted.cmd` in the repository folder. The first PLAY
asks for the game folder (above). Pick a game (1, 2, 3), a preset (Medium is 1080p at 60 FPS) and
press **PLAY**. The first start of each game prepares its files and compiles shaders: expect a
slow first load and some stutter. The Advanced tab has **Desktop shortcut**, **Open log folder** and
**Crash reports**. A copy run from the source tree never updates itself: it changes with git.

**From a terminal**, without the launcher (Command Prompt or PowerShell):

```
cd /d D:\ps4_ports\Uncharted-The-Nathan-Drake-Collection-PC-port
py tools\probe_uncharted.py --boot --seconds 0 --game "F:\Uncharted - The Nathan Drake Collection (CUSA02343) [v01.00]" --executable big3-ps4_Shipping.elf
```

`--executable eboot.bin` is Uncharted 1 (the default), `big2-ps4_Shipping.elf` Uncharted 2,
`big3-ps4_Shipping.elf` Uncharted 3; `--collection-menu` starts the Collection's game menu instead.
The settings are the launcher's (`analysis\uncharted-user\probe-u1.ini`, `-u2`, `-u3`). `--seconds 0`
means no time limit.

Closing the game window ends the game; the launcher's STOP does the same.

## Controls

Controllers (DualSense, DualShock 4, Xbox) work as on the console. Keyboard and mouse, by default
(the launcher's Controls tab changes them per game):

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

**Insert** (or R3 + L2 on a controller) opens the port's own menu (display, frame rate, overlay).
**F8** marks a moment for a report (below).

## When something goes wrong

Every report is a folder in `analysis\reports` named `<date> <time> <game> <kind>`:

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
  (`screen-10s-earlier.jpg` too). Not in exclusive fullscreen: use windowed or borderless.
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
| `GOW3_ASYNC_READBACK=0` | GPU readbacks: the CPU waits for them. **Uncharted 3's default**: the fast mode crashed it in the London pub fight. |
| `GOW3_ASYNC_READBACK=1` | readbacks in the background, pages read-protected until they arrive (not yet tested in Uncharted 3) |
| `GOW3_ASYNC_READBACK=2` | readbacks in the background, unprotected: the default of Uncharted 1 and 2 |
| `GOW3_FREEZE_SECONDS=30` | how long without a new frame counts as a freeze (0: no freeze reports) |
| `GOW3_MARK_KEY=F7` | another mark key (an SDL key name; `off` turns it off) |
| `GOW3_MINIDUMP=0` | no minidump with crash reports |

## Known problems (16 October 2026)

- Each game first says "Friend Stats are not available when offline": expected, there is no PSN.
- Uncharted 2's heaviest scenes run at 40-45 FPS; multithreaded rendering with 2 frames queued can
  end Uncharted 2 with the game assert `m_gfxEopTick` (leave multithreaded rendering off).
- Uncharted 1 renders at most 2880x1620 and upscales above that.
- Uncharted 3 has been played the least. The full list: [ROADMAP.md](ROADMAP.md) and
  [STATUS.md](STATUS.md).
