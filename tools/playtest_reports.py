"""Play-test reports: the start script's half of gow3_pc/src/runtime_report.c.

The runtime writes a folder in analysis/reports for a crash, a freeze (no new frame for 20 s) or
a moment the player marks with F8, and names it in the log ("Crash report: <folder>"). While the
game runs, ScreenWatcher keeps a screenshot of its window from the last 10 seconds and takes one
into each mark or freeze folder; after the game ends, finish() adds to every folder of that run
the end of the log, the settings and the screenshot, names the runtime's functions in a crash's
call stack (llvm-symbolizer), and writes a report for an exit the runtime could not report (a
stop, a fast fail). Everything here is best effort: a report must never keep the game from
starting or the start script from ending.
"""
import ctypes
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parent.parent
REPORTS = ROOT / "analysis" / "reports"
GAMES = {"eboot.bin": ("Uncharted: Drake's Fortune", "U1"),
         "big2-ps4_Shipping.elf": ("Uncharted 2: Among Thieves", "U2"),
         "big3-ps4_Shipping.elf": ("Uncharted 3: Drake's Deception", "U3")}
ANNOUNCED = re.compile(r"^(Crash|Freeze|Mark) report: (.+?)\s*$")
EXIT_CODES = {1: "the runtime stopped with an error (ERROR line in the log)",
              3: "a crash (fault) or abort()",
              20: "the game called a PS4 function the runtime does not have, or the CPU lacks an instruction set",
              21: "a thread could not be created",
              22: "the game's stack protector found its stack overwritten",
              23: "a failed check: in the GPU library, or the game's own (debug exception)",
              0xC0000005: "access violation outside the runtime's handler",
              0xC00000FD: "stack overflow",
              0xC0000374: "heap corruption",
              0xC0000409: "fast fail: a security check or a stack buffer overrun ended the process at once"}


def names(executable, guest_args):
    """(game name, short name) of what is starting; eboot.bin without arguments is the menu."""
    if executable == "eboot.bin" and list(guest_args or []) == ["/app0/eboot.bin"]:
        return "The Nathan Drake Collection (game menu)", "Menu"
    return GAMES.get(executable, (executable, Path(executable).stem))


def environment(executable, guest_args, version):
    name, short = names(executable, guest_args)
    REPORTS.mkdir(parents=True, exist_ok=True)
    return {"GOW3_REPORT_DIR": str(REPORTS), "GOW3_REPORT_GAME": f"{name}, {version}" if version else name,
            "GOW3_REPORT_SHORT": short, "GOW3_REPORT_EXE": executable}


# --- screenshots of the game window ---------------------------------------------------------

def _window_of(pid):
    """The largest visible top-level window of the process (its game window), or None."""
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def visit(hwnd, _):
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            rect = (ctypes.c_long * 4)()
            user32.GetClientRect(ctypes.c_void_p(hwnd), rect)
            found.append((rect[2] * rect[3], hwnd, rect[2], rect[3]))
        return True

    user32.EnumWindows(visit, None)
    return max(found)[1:] if found else None


def capture(pid, out, width=960):
    """A JPEG of the game window's client area (PrintWindow, so other windows may cover it).
    False when there is no window, it came back black (exclusive fullscreen), or Pillow is missing."""
    try:
        from PIL import Image
    except ImportError:
        return False
    window = _window_of(pid)
    if not window or window[1] < 16 or window[2] < 16:
        return False
    hwnd, w, h = window
    user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
    for function in (user32.GetDC, gdi32.CreateCompatibleDC, gdi32.CreateCompatibleBitmap, gdi32.SelectObject):
        function.restype = ctypes.c_void_p
    hdc = user32.GetDC(ctypes.c_void_p(hwnd))
    memory = gdi32.CreateCompatibleDC(ctypes.c_void_p(hdc))
    bitmap = gdi32.CreateCompatibleBitmap(ctypes.c_void_p(hdc), w, h)
    try:
        gdi32.SelectObject(ctypes.c_void_p(memory), ctypes.c_void_p(bitmap))
        if not user32.PrintWindow(ctypes.c_void_p(hwnd), ctypes.c_void_p(memory), 3):  # client only, full content
            return False
        header = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        info = ctypes.create_string_buffer(header, 44)
        pixels = ctypes.create_string_buffer(w * h * 4)
        if not gdi32.GetDIBits(ctypes.c_void_p(memory), ctypes.c_void_p(bitmap), 0, h, pixels, info, 0):
            return False
    finally:
        gdi32.DeleteObject(ctypes.c_void_p(bitmap))
        gdi32.DeleteDC(ctypes.c_void_p(memory))
        user32.ReleaseDC(ctypes.c_void_p(hwnd), ctypes.c_void_p(hdc))
    image = Image.frombuffer("RGB", (w, h), pixels, "raw", "BGRX", 0, 1)
    if image.getextrema() == ((0, 0), (0, 0), (0, 0)):
        return False
    if image.width > width:
        image = image.resize((width, round(image.height * width / image.width)))
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_suffix(".part")
    image.save(partial, "JPEG", quality=85)
    os.replace(partial, out)
    return True


class ScreenWatcher(threading.Thread):
    """While the game runs: a screenshot every `period` seconds (the last two are kept, for a
    crash), and one into each mark or freeze report as soon as the log names it."""

    def __init__(self, pid, log, short, period=10.0):
        super().__init__(daemon=True)
        self.pid, self.log, self.period = pid, log, period
        self.shots = REPORTS / ".screens" / short
        self.done = threading.Event()
        self.position, self.partial = 0, b""

    def last(self):
        return [path for path in (self.shots / "last.jpg", self.shots / "before.jpg") if path.exists()]

    def new_announcements(self):
        try:
            with open(self.log, "rb") as f:
                f.seek(self.position)
                data = f.read()
        except OSError:
            return []
        self.position += len(data)
        lines = (self.partial + data).split(b"\n")
        self.partial = lines.pop()
        found = []
        for line in lines:
            match = ANNOUNCED.match(line.decode("utf-8", "replace").strip())
            if match:
                found.append((match.group(1), Path(match.group(2))))
        return found

    def run(self):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            return
        for old in self.last():
            old.unlink(missing_ok=True)
        next_shot = time.monotonic() + 15  # the window opens after the game files are read
        while not self.done.wait(1.0):
            try:
                for kind, folder in self.new_announcements():
                    if kind in ("Mark", "Freeze"):
                        capture(self.pid, folder / "screen.jpg")
                if time.monotonic() >= next_shot:
                    next_shot = time.monotonic() + self.period
                    taken = self.shots / "taken.jpg"
                    if capture(self.pid, taken):
                        if (self.shots / "last.jpg").exists():
                            os.replace(self.shots / "last.jpg", self.shots / "before.jpg")
                        os.replace(taken, self.shots / "last.jpg")
            except Exception as error:  # never let a screenshot problem end the run
                print(f"Reports: screenshot failed: {error}")
                next_shot = time.monotonic() + 60

    def stop(self):
        self.done.set()
        self.join(timeout=5)


# --- after the game ends ----------------------------------------------------------------

def _settings_text(env, config):
    lines = ["Settings of this run", ""]
    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True,
                              text=True, timeout=10).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"],
                               capture_output=True, text=True, timeout=10).stdout.strip()
        lines.append(f"Source: commit {head or '?'}{' with local changes' if dirty else ''}")
    except (OSError, subprocess.SubprocessError):
        pass
    runtime = ROOT / "gow3_pc/out/gow3-probe.exe"
    if runtime.exists():
        lines.append(f"Runtime built: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(runtime.stat().st_mtime))}")
    lines += ["", "Environment (GOW3_*, UNCHARTED_*):"]
    lines += [f"  {key}={value}" for key, value in sorted(env.items()) if key.startswith(("GOW3_", "UNCHARTED_"))]
    if config and Path(config).exists():
        lines += ["", f"Settings file {config}:"]
        lines += ["  " + line for line in Path(config).read_text(encoding="utf-8", errors="replace").splitlines()]
    return "\n".join(lines) + "\n"


def _image_base(exe):
    data = exe.read_bytes()[:4096]
    pe, = struct.unpack_from("<I", data, 0x3C)
    magic, = struct.unpack_from("<H", data, pe + 24)
    return struct.unpack_from("<Q", data, pe + 24 + 24)[0] if magic == 0x20B else 0x140000000


def _symbolize(report):
    """Names the runtime's own frames (<exe>+0x<offset> of an exe in gow3_pc/out) with
    llvm-symbolizer and the exe's debug information; appended to the report."""
    text = report.read_text(encoding="utf-8", errors="replace")
    frames = sorted(set(re.findall(r"\b([\w.-]+\.exe)\+0x([0-9a-f]+)\b", text)))
    out = ROOT / "gow3_pc/out"
    symbolizer = Path(os.environ.get("MSYS2_ROOT", "C:/msys64")) / "clang64/bin/llvm-symbolizer.exe"
    frames = [(exe, offset) for exe, offset in frames if (out / exe).exists()]
    if not frames or not symbolizer.exists():
        return
    lines = []
    for exe in sorted({exe for exe, _ in frames}):
        base = _image_base(out / exe)
        offsets = [offset for name, offset in frames if name == exe]
        try:
            result = subprocess.run([str(symbolizer), f"--obj={out / exe}", "--relative-address"] +
                                    [f"0x{int(offset, 16):x}" for offset in offsets],
                                    capture_output=True, text=True, timeout=60)
        except (OSError, subprocess.SubprocessError):
            return
        blocks = [block.splitlines() for block in result.stdout.strip().split("\n\n")]
        if len(blocks) != len(offsets):  # an older llvm-symbolizer: addresses with the image base
            try:
                result = subprocess.run([str(symbolizer), f"--obj={out / exe}"] +
                                        [f"0x{base + int(offset, 16):x}" for offset in offsets],
                                        capture_output=True, text=True, timeout=60)
            except (OSError, subprocess.SubprocessError):
                return
            blocks = [block.splitlines() for block in result.stdout.strip().split("\n\n")]
        for offset, block in zip(offsets, blocks):
            pairs = [(block[i], block[i + 1]) for i in range(0, len(block) - 1, 2)]
            named = []
            for function, place in pairs:  # place: <file>:<line>:<column>, the file may hold a drive colon
                file, line = (place.rsplit(":", 2)[:2] if place.count(":") >= 2 else (place, "0"))
                if function != "??":
                    named.append(function if file == "??" else f"{function} ({Path(file).name}:{line})")
            if named:
                lines.append(f"  {exe}+0x{offset}  {' <- inlined in '.join(named)}")
    if lines:
        with report.open("a", encoding="utf-8") as f:
            f.write("\nRuntime functions in the call stack (llvm-symbolizer)\n" + "\n".join(lines) + "\n")


def _log_lines(log):
    try:
        return log.read_bytes().decode("utf-8", "replace").splitlines()
    except OSError:
        return []


def _write_own_report(code, lines, short, name):
    """A report for an exit the runtime did not report itself."""
    stamp = time.strftime("%Y-%m-%d %H-%M-%S")
    folder = REPORTS / f"{stamp} {short} {'stop' if code in (1, 20, 21) else 'crash'}"
    folder.mkdir(parents=True, exist_ok=True)
    meaning = EXIT_CODES.get(code & 0xFFFFFFFF, "unknown")
    found = [line for line in lines[-400:] if line.startswith(("STOP:", "ERROR:", "Fault:")) or "Assertion" in line
             or "Unreachable code" in line]
    text = [f"{name} ended unexpectedly", "",
            f"Exit code: {code} (0x{code & 0xFFFFFFFF:08X}): {meaning}",
            f"When:      {time.strftime('%Y-%m-%d %H:%M:%S')}", "",
            "The runtime wrote no report of its own, so this one has only the log: what it said last is in",
            "log-tail.txt. Lines that name the cause:"]
    text += ["  " + line for line in found[-12:]] or ["  none"]
    (folder / "report.txt").write_text("\n".join(text) + "\n", encoding="utf-8")
    return folder


def finish(log, code, env, config, watcher, executable, guest_args):
    """Completes the reports of the run that wrote `log` and returns their folders. code is the
    runtime's exit code, None when the run was stopped (time limit)."""
    lines = _log_lines(log)
    name, short = names(executable, guest_args)
    reports = []
    for index, line in enumerate(lines):
        match = ANNOUNCED.match(line.strip())
        if match and Path(match.group(2)).is_dir():
            reports.append((match.group(1), Path(match.group(2)), index))
    if code not in (None, 0, 30) and not any(kind == "Crash" for kind, _, _ in reports):
        reports.append(("Crash", _write_own_report(code, lines, short, name), len(lines)))
    settings = _settings_text(env, config)
    for kind, folder, index in reports:
        try:
            tail = lines[-400:] if kind == "Crash" else lines[max(0, index - 200):index + 1]
            (folder / "log-tail.txt").write_text("\n".join(tail) + "\n", encoding="utf-8")
            (folder / "settings.txt").write_text(settings, encoding="utf-8")
            if kind == "Crash" and watcher:
                for shot, target in zip(watcher.last(), ("screen.jpg", "screen-10s-earlier.jpg")):
                    shutil.copy2(shot, folder / target)
            if (folder / "report.txt").exists():
                _symbolize(folder / "report.txt")
        except OSError as error:
            print(f"Reports: {folder} incomplete: {error}")
        print(f"{kind} report folder: {folder}")
    return [folder for _, folder, _ in reports]
