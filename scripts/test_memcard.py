#!/usr/bin/env python3
"""Checks that the runner writes the memory card file when the game ends, and that the write is safe.

  python scripts/test_memcard.py                      uses build/native-gow2/ps2run and games/gow2 (any built game works: --game NAME)
  python scripts/test_memcard.py --exe ps2run.exe --program SOME.ELF [--disc game.iso]

How it works: the runner has a hidden test switch, the environment variable PS2_TEST_CARD_DIRTY=<frame>[,stop][,hold]
(runtime/src/iop/sio2.cpp). At that frame it writes a marker text into the card and counts the card as changed, as if the game had
just saved. With ",stop" it also ends the run in the same frame, as if the game had quit; with ",hold" the usual write 50 frames
after the last change never comes. So the file can only be there if the run wrote it when it ended. The test then looks at the file
and at the order of the log lines. Each case runs the game headless for 60 to 130 frames (a few seconds).

  1. the frame limit (--frames) ends the run right after the card changed           -> the file is written
  2. the game stops (halt) right after the card changed                               -> the file is written
  3. a card file that exists is changed in one page only, everything else is kept      (a whole card, not a new empty one)
  4. the write cannot get its temporary file                                           -> the old card file is untouched
  5. the normal write 50 frames after a change still happens, before the run ends
  6. (Windows) the window is closed some time after the card changed                   -> the file is written
  7. (Windows) Ctrl+Break reaches the program some time after the card changed         -> the file is written
  8. (Windows) the program crashes right after the card changed                        -> the newest card goes to <card>.crash.ps2,
     the card file keeps the last card written normally
  9. (no game needed; alone with --checker-only) the card checker mcformat.py check accepts a root directory of two clusters and
     still fails on a damaged page, a looping chain, a chain through a free cluster, a too-short root chain, a shared cluster
Cases 6 and 7 open the game window (and a console for case 7, if this script has none) for a few seconds; --no-window leaves them out.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CARD_BYTES = 16384 * 528
MARKER_AT = 4000 * 528  # the page the runner puts the marker into
MARKER = b"ps2-native-recomp card flush test, frame "
CHANGED = "[mc] test: card marked as changed"


def find_program(game):
    disc = ROOT / "games" / game / "disc"
    for path in sorted(disc.iterdir()) if disc.is_dir() else []:
        if re.fullmatch(r"[A-Z]{4}_\d{3}\.\d{2}", path.name) or path.name == "core.elf":
            return path
    sys.exit(f"no game program in {disc}: build the game first (scripts/port.sh)")


def command(exe, program, disc, card, *more):
    cmd = [str(exe), str(program), "--memcard", str(card)]
    if disc:
        cmd += ["--disc", str(disc)]
    return cmd + [str(m) for m in more]


def run(exe, program, disc, card, frames, switch):
    env = dict(os.environ, PS2_TEST_CARD_DIRTY=switch)
    done = subprocess.run(command(exe, program, disc, card, "--frames", frames), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=600)
    return done.returncode, done.stdout.decode(errors="replace").splitlines()


def index_of(lines, text):
    for i, line in enumerate(lines):
        if text in line:
            return i
    return None


failures = []


def check(name, ok, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {name}{(': ' + detail) if detail and not ok else ''}")
    if not ok:
        failures.append(name)


# --- Windows only: closing the window and sending Ctrl+Break ---------------------------------------------------------

def wait_for_line(log_path, text, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            if text in Path(log_path).read_bytes().decode(errors="replace"):
                return True
        except OSError:
            pass
        time.sleep(0.02)
    return False


def close_window(pid):
    """Sends WM_CLOSE to the program's window, which is what its close button does."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(visit, 0)
    for hwnd in found:
        user32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE
    return bool(found)


def console_text():
    """Everything written so far into this process's console, read back from its screen buffer."""
    import ctypes
    from ctypes import wintypes
    kernel = ctypes.windll.kernel32

    class Coord(ctypes.Structure):
        _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

    class Rect(ctypes.Structure):
        _fields_ = [("Left", ctypes.c_short), ("Top", ctypes.c_short), ("Right", ctypes.c_short), ("Bottom", ctypes.c_short)]

    class Info(ctypes.Structure):
        _fields_ = [("size", Coord), ("cursor", Coord), ("attributes", wintypes.WORD), ("window", Rect), ("maximum", Coord)]

    kernel.CreateFileW.restype = wintypes.HANDLE
    handle = kernel.CreateFileW("CONOUT$", 0xC0000000, 3, None, 3, 0, None)  # read and write, shared, existing: the screen buffer
    info = Info()
    if not kernel.GetConsoleScreenBufferInfo(handle, ctypes.byref(info)):
        kernel.CloseHandle(handle)
        return ""
    cells = info.size.X * (info.cursor.Y + 1)
    text = ctypes.create_unicode_buffer(cells)
    got = wintypes.DWORD()
    kernel.ReadConsoleOutputCharacterW(handle, text, cells, Coord(0, 0), ctypes.byref(got))
    kernel.CloseHandle(handle)
    return text.value


def case_window_closed(exe, program, disc, card, work):
    print("6. the window is closed some time after a change")
    if card.exists():
        card.unlink()
    log_path = work / "window.log"
    env = dict(os.environ, PS2_TEST_CARD_DIRTY="120,hold")
    with open(log_path, "wb") as log:
        process = subprocess.Popen(command(exe, program, disc, card, "--window"), cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            check("the game got to the change", wait_for_line(log_path, CHANGED, 120))
            time.sleep(2.0)  # longer than the 50-frame write (held back here) would need
            check("the card file is not there yet", not card.exists())
            check("the window was found and closed", close_window(process.pid))
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                check("the program ended after the window was closed", False, "still running after 60 s")
        finally:
            if process.poll() is None:
                process.kill()
    lines = log_path.read_text(errors="replace").splitlines()
    halted, written = index_of(lines, "[ee] halted: window closed"), index_of(lines, "[mc] card file written")
    # the card is written the moment the window closes, just before the run is told to stop; the 50-frame write was held back
    check("the window close was seen and the card file was written", halted is not None and written is not None, f"halted line {halted}, written line {written}")
    data = card.read_bytes() if card.exists() else b""
    check("the file has the marker", data[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER)


def case_ctrl_break(exe, program, disc, card):
    import ctypes
    print("7. Ctrl+Break reaches the program some time after a change")
    if card.exists():
        card.unlink()
    kernel = ctypes.windll.kernel32
    attached = (ctypes.c_uint * 4)()
    allocated = False
    if not kernel.GetConsoleProcessList(attached, 4):  # not attached to any console (started without one): make one
        allocated = bool(kernel.AllocConsole())
    if not kernel.GetConsoleProcessList(attached, 4):
        print("  SKIP  no console to send Ctrl+Break through")
        return
    try:
        # The program has no output file here, so it joins this console (as it does when started from a prompt) and prints its
        # log lines into it; they are read back from the console's screen buffer. Ctrl+Break goes to its process group only.
        env = dict(os.environ, PS2_TEST_CARD_DIRTY="120,hold")
        process = subprocess.Popen(command(exe, program, disc, card, "--window"), cwd=ROOT, env=env, creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        try:
            end = time.time() + 120
            while time.time() < end and CHANGED not in console_text() and process.poll() is None:
                time.sleep(0.1)
            check("the game got to the change", process.poll() is None and CHANGED in console_text())
            time.sleep(1.5)
            check("the card file is not there yet", not card.exists())
            kernel.GenerateConsoleCtrlEvent(1, process.pid)  # CTRL_BREAK_EVENT
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                process.kill()
                check("the program ended after Ctrl+Break", False, "still running after 30 s")
        finally:
            if process.poll() is None:
                process.kill()
        data = card.read_bytes() if card.exists() else b""
        check("the file has the marker", data[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER, f"{len(data)} bytes")
    finally:
        if allocated:
            kernel.FreeConsole()


def case_crash(exe, program, disc, card, before):
    import ctypes
    print("8. the program crashes right after a change: the newest card goes to card.crash.ps2, the card file stays")
    crash_copy = card.with_name(card.stem + ".crash" + card.suffix)
    card.write_bytes(before)  # the last card written normally
    if crash_copy.exists():
        crash_copy.unlink()
    # no "has stopped working" window: the child inherits this error mode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)
    old_mode = ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
    try:
        code, log = run(exe, program, disc, card, 100000, "70,crash")
    finally:
        ctypes.windll.kernel32.SetErrorMode(old_mode)
    check("the program crashed", code != 0 and index_of(log, CHANGED) is not None, f"exit code {code}")
    data = crash_copy.read_bytes() if crash_copy.exists() else b""
    check("card.crash.ps2 is a whole card with the new marker",
          len(data) == CARD_BYTES and data[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER and b"frame 70" in data[MARKER_AT:MARKER_AT + 64],
          f"{len(data)} bytes")
    check("the runner says where the newest card went", index_of(log, crash_copy.name) is not None)
    check("the card file is unchanged", card.read_bytes() == before if card.exists() else False)
    check("no temporary file left", not card.with_name(card.name + ".tmp").exists() and not crash_copy.with_name(crash_copy.name + ".tmp").exists())


# --- the card checker (scripts/mcformat.py check), no game needed ----------------------------------------------------------

def checker_cases():
    """Cards built in memory: a root directory of two clusters must pass, every kind of real damage must fail with a clear message."""
    import contextlib
    import io
    import struct
    import time
    sys.path.insert(0, str(ROOT / "scripts"))
    import mcformat as mc

    work = Path(tempfile.mkdtemp(prefix="mccheck-test-"))
    when = time.strptime("2026-01-01 00:00:00", "%Y-%m-%d %H:%M:%S")

    def run_check(raw):
        path = work / "card.ps2"
        path.write_bytes(bytes(raw))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = mc.check_card(str(path))
        return code, out.getvalue()

    def put_cluster(raw, cluster, content):
        content = bytes(content).ljust(mc.CLUSTER_SIZE, b"\xFF")
        for i in range(mc.PAGES_PER_CLUSTER):
            chunk = content[i * mc.PAGE_SIZE:(i + 1) * mc.PAGE_SIZE]
            page = cluster * mc.PAGES_PER_CLUSTER + i
            raw[page * mc.RAW_PAGE:(page + 1) * mc.RAW_PAGE] = chunk + mc.ecc_page(chunk)

    def get_cluster(raw, cluster):
        return b"".join(bytes(raw[(cluster * 2 + i) * mc.RAW_PAGE:(cluster * 2 + i) * mc.RAW_PAGE + mc.PAGE_SIZE]) for i in range(2))

    def set_fat(raw, index, value):
        cluster = 9 + index // 256   # the made card's FAT starts at cluster 9
        data = bytearray(get_cluster(raw, cluster))
        struct.pack_into("<L", data, (index % 256) * 4, value)
        put_cluster(raw, cluster, data)

    def entry(mode, length, cluster, name):
        return mc.dirent(mode, length, cluster, 0, mc.tod(when), name)

    DIR, FILE = mc.MODE_DOT, 0x8497
    # root (clusters 0, 1): ".", "..", SAVE1, SAVE2 -> four entries, so two clusters, FAT entry 0 = 0x80000001
    # SAVE1 (clusters 2, 3): ".", "..", data (1500 bytes: clusters 4, 5), icon (100 bytes: cluster 6); SAVE2 (cluster 7): empty
    def good_card():
        raw = bytearray(mc.make_card(when))
        put_cluster(raw, 41 + 0, entry(DIR, 4, 0, b".") + entry(mc.MODE_DOTDOT, 0, 0, b".."))
        put_cluster(raw, 41 + 1, entry(DIR, 4, 2, b"SAVE1") + entry(DIR, 2, 7, b"SAVE2"))
        put_cluster(raw, 41 + 2, entry(DIR, 0, 0, b".") + entry(mc.MODE_DOTDOT, 0, 0, b".."))
        put_cluster(raw, 41 + 3, entry(FILE, 1500, 4, b"data") + entry(FILE, 100, 6, b"icon"))
        put_cluster(raw, 41 + 4, b"A" * 1024)
        put_cluster(raw, 41 + 5, b"B" * 476)
        put_cluster(raw, 41 + 6, b"C" * 100)
        put_cluster(raw, 41 + 7, entry(DIR, 0, 0, b".") + entry(mc.MODE_DOTDOT, 0, 0, b".."))
        for index, value in enumerate((0x80000001, 0xFFFFFFFF, 0x80000003, 0xFFFFFFFF, 0x80000005, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)):
            set_fat(raw, index, value)
        return raw

    print("9. the card checker (mcformat.py check)")
    try:
        code, text = run_check(mc.make_card(when))
        check("an empty formatted card passes", code == 0 and "RESULT: fine" in text, text)
        card = good_card()
        code, text = run_check(card)
        check("a root directory of two clusters (FAT entry 0 = 0x80000001) passes", code == 0 and "root directory chain of 2" in text
              and "2 files" in text, text)
        gt4 = ROOT / "games" / "gt4" / "play" / "gt4.ps2"
        if gt4.exists():
            code, text = run_check(gt4.read_bytes())
            check("the card Gran Turismo 4 wrote passes", code == 0 and "RESULT: fine" in text, text)

        damaged = bytearray(card)
        damaged[(41 * 2) * mc.RAW_PAGE + 200] ^= 0x1E   # one byte of the root directory's first page, four bits wrong
        # (a byte turned over completely, 0xFF, would go unseen: the ECC of [mymc]/[mcman] cannot see it, so it is not tested)
        code, text = run_check(damaged)
        check("one damaged byte in a page fails (bad ECC)", code != 0 and "BAD ECC: page 82" in text, text)

        damaged = bytearray(card)
        set_fat(damaged, 3, 0x80000002)   # SAVE1's chain 2 -> 3 -> 2
        code, text = run_check(damaged)
        check("a FAT chain that loops fails", code != 0 and "loops back to cluster 2" in text, text)

        damaged = bytearray(card)
        set_fat(damaged, 5, 0x7FFFFFFF)   # the file "data" runs 4 -> 5, and 5 is now free
        code, text = run_check(damaged)
        check("a chain that reaches a free cluster fails", code != 0 and "cluster 5, which the FAT marks as free" in text, text)

        damaged = bytearray(card)
        set_fat(damaged, 0, 0xFFFFFFFF)   # the root directory cut to one cluster, but it has four entries
        code, text = run_check(damaged)
        check("a root directory chain that is too short fails", code != 0 and "says it has 4 entries" in text, text)

        damaged = bytearray(card)
        set_fat(damaged, 4, 0x80000006)   # "data" now runs 4 -> 6, a cluster that "icon" owns
        code, text = run_check(damaged)
        check("two files sharing a cluster fail", code != 0 and "also used by" in text, text)

        damaged = bytearray(card)
        set_fat(damaged, 1, 0x80000000 + 9000)   # a chain pointing outside the card
        code, text = run_check(damaged)
        check("a chain that points outside the card fails", code != 0 and "outside the card" in text, text)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--game", default="gow2")
    ap.add_argument("--exe")
    ap.add_argument("--program")
    ap.add_argument("--disc")
    ap.add_argument("--checker-only", action="store_true", help="run only the card checker cases (case 9), which need no game")
    ap.add_argument("--no-window", action="store_true", help="leave out the cases that open the game window")
    a = ap.parse_args()
    if a.checker_only:
        checker_cases()
        print("\nRESULT: " + ("all passed" if not failures else f"{len(failures)} failed: " + "; ".join(failures)))
        return 1 if failures else 0
    exe = Path(a.exe) if a.exe else ROOT / "build" / f"native-{a.game}" / ("ps2run.exe" if os.name == "nt" else "ps2run")
    program = Path(a.program) if a.program else find_program(a.game)
    disc = Path(a.disc) if a.disc else (ROOT / "games" / a.game / "disc.iso" if not a.program else None)
    if not exe.exists():
        sys.exit(f"{exe} is missing: build the game first (scripts/port.sh)")

    work = Path(tempfile.mkdtemp(prefix="memcard-test-"))
    try:
        card = work / "card.ps2"

        print("1. frame limit right after a change")
        code, log = run(exe, program, disc, card, 60, "60")
        check("run ended normally", code == 0, f"exit code {code}")
        check("card file exists", card.exists())
        data = card.read_bytes() if card.exists() else b""
        check("card file is a whole card (8650752 bytes)", len(data) == CARD_BYTES, f"{len(data)} bytes")
        check("marker is in the file", data[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER)
        check("the rest of the card is still blank (0xFF)", len(data) == CARD_BYTES and set(data[:MARKER_AT]) == {0xFF})
        check("no temporary file left", not (work / "card.ps2.tmp").exists())
        halted, written = index_of(log, "[ee] halted: frame limit reached"), index_of(log, "[mc] card file written")
        check("written after the run was ended, not by the 50-frame timer", halted is not None and written is not None and written > halted,
              f"halted line {halted}, written line {written}")
        first_card = data

        print("2. the game stops right after a change")
        card.unlink()
        code, log = run(exe, program, disc, card, 100000, "61,stop")
        data = card.read_bytes() if card.exists() else b""
        check("run ended normally", code == 0, f"exit code {code}")
        check("marker is in the file", data[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER and b"frame 61" in data[MARKER_AT:MARKER_AT + 64])
        halted, written = index_of(log, "[ee] halted: memory card test"), index_of(log, "[mc] card file written")
        check("written after the stop", halted is not None and written is not None and written > halted, f"halted line {halted}, written line {written}")

        print("3. an existing card file is changed in one page only")
        card.write_bytes(first_card)  # the card of case 1: marker "frame 60"
        code, log = run(exe, program, disc, card, 100000, "62,stop")
        data = card.read_bytes()
        check("marker of the new run is there", b"frame 62" in data[MARKER_AT:MARKER_AT + 64])
        same = data[:MARKER_AT] == first_card[:MARKER_AT] and data[MARKER_AT + 64:] == first_card[MARKER_AT + 64:]
        check("everything outside the marker is the same as before", same and len(data) == CARD_BYTES)

        print("4. the write cannot get its temporary file: the old card stays")
        before = card.read_bytes()
        (work / "card.ps2.tmp").mkdir()  # a folder in the way: the temporary file cannot be created
        code, log = run(exe, program, disc, card, 100000, "63,stop")
        check("the runner says it cannot write", index_of(log, "[mc] cannot write") is not None)
        check("the old card file is untouched", card.read_bytes() == before)
        check("run still ended normally", code == 0, f"exit code {code}")
        (work / "card.ps2.tmp").rmdir()

        print("5. the normal write 50 frames after a change still happens")
        card.unlink()
        code, log = run(exe, program, disc, card, 130, "60")
        written, halted = index_of(log, "[mc] card file written"), index_of(log, "[ee] halted: frame limit reached")
        check("written before the run ends", written is not None and halted is not None and written < halted, f"written line {written}, halted line {halted}")
        check("the file has the marker", card.exists() and card.read_bytes()[MARKER_AT:MARKER_AT + len(MARKER)] == MARKER)

        if os.name == "nt" and not a.no_window:
            case_window_closed(exe, program, disc, card, work)
            case_ctrl_break(exe, program, disc, card)
        if os.name == "nt":
            case_crash(exe, program, disc, card, first_card)
        checker_cases()
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print("\nRESULT: " + ("all passed" if not failures else f"{len(failures)} failed: " + "; ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
