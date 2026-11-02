#!/usr/bin/env python3
"""How fast does a game run in the runner's window on this PC? Repeatable speed measurements (Windows only).

    python scripts/speedcheck.py --scene title                           # one scene
    python scripts/speedcheck.py --all --backends vulkan,d3d12           # every scene on both graphics APIs (about 14 minutes)
    python scripts/speedcheck.py --scene arena --scale 2                 # the heaviest scene at twice the resolution
    python scripts/speedcheck.py --all --backends vulkan,d3d12 --picture # one small picture per scene and API, to see that both draw alike
    python scripts/speedcheck.py --table games/gow2/speed/speed-results.jsonl    # earlier results as a table
    python scripts/speedcheck.py --profile-summary profile.txt           # a PS2_PROFILE file added up by part of the console
    python scripts/speedcheck.py --scene arena --sample                  # also look where each busy thread is, every millisecond (all threads)
    python scripts/speedcheck.py --sample-report games/gow2/speed/sample-<run>.json --exe build/native-gow2/ps2run.exe   # name the functions

What a run does: it starts the game in a window the way the player does, waits a few seconds for the scene to settle, then reads the
speed the window's title bar shows (once a second: frames per second, CPU, GPU, sound queue) for 30 to 35 seconds, and takes the CPU
time of every thread of the runner at the start and the end, so you can see which part is busy. One line per run goes into
`<out>/speed-results.jsonl` (default `games/<game>/speed`) and is printed. Then it closes the window the way a player does.
The window title is the only speed display the runner has; there is no frame counter in the log.

Read the numbers only when the run is clean. A run is marked CONTAMINATED when another `ps2run.exe` was running at the same time
(somebody is playing) or when the rest of the PC used more than 15 % of its CPU (a build, a browser): throw it away and repeat.
The script never stops anything but the game it started itself. It reads and does not change the power state: mains or battery,
battery level, the Windows power mode (the slider in Settings) and the power plan go into every result, because they change the
numbers a lot. Plug in and choose "Best performance" before you take numbers you want to quote.

Scenes for God of War II (save states from scripts/play.py in games/gow2/play; the fight pattern games/gow2/play/fight.txt is pressed):
    title      power-on, Start at frame 300: the title menu
    film       the opening film and scenes after it (starts from a state the script makes by itself the first time, `--make-states`)
    hall       the first fight, the first hall (gow2_f010150.state)
    fight2     the second fight behind the gate (gow2_f022800.state)
    colossus   the Colossus attack starts (gow2_f036840.state)
    arena      the fight in front of the Colossus (gow2_f055800.state)
Your own scene: `--from STATE --from-frame N --moves "0:square:3@14*100"` (the move syntax of scripts/play.py).

More: `--backends default,vulkan,d3d12` (default means "whatever the runner picks"; the log says which), `--scale 2`, `--repeat N`,
`--seconds` and `--warmup`, `--no-limit` (no speed limit: the game goes as fast as it can, with the sound out of step; the same as `--env PS2_NO_LIMIT=1`),
`--env KEY=VALUE` for the runner (`PS2_ONE_THREAD=1`, `PS2_PROFILE=file`, ...), `--frames N` to let the run
end by itself (PS2_PROFILE writes its file only then), `--exe` to run another copy of ps2run.exe than the one in build/ (a copy keeps the
build folder free while you measure), `--affinity` and `--priority` for experiments. Standard library only (Pillow is not needed).
docs/speed.md has the results of the laptop and how to read them.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

FIGHT = "fight.txt"  # in games/<game>/play; the pattern play.py users know (docs/gow2-play.md, "Fight pattern")
TITLE = re.compile(r"([\d.]+) fps\s+CPU (-?\d+)%\s+GPU (-?\d+)%\s+RAM (\d+) MB\s+audio (\d+) ms")
OTHER_LOAD_LIMIT = 15.0  # percent of all the PC's threads; above it the run is flagged

# name: (state file or None for the power-on, state's frame, moves, what is measured, seconds to settle, seconds measured)
SCENES = {
    "title": (None, 0, "300:start:8", "power-on, Start at frame 300: the title menu", 8, 30),
    "film": ("film", 0, "", "the opening film and scenes (state made by --make-states)", 4, 30),
    "hall": ("gow2_f010150.state", 10150, "@" + FIGHT, "the first fight in the first hall", 10, 35),
    "fight2": ("gow2_f022800.state", 22800, "@" + FIGHT, "the second fight behind the gate", 10, 35),
    "colossus": ("gow2_f036840.state", 36840, "@" + FIGHT, "the Colossus attack starts", 10, 35),
    "arena": ("gow2_f055800.state", 55800, "@" + FIGHT, "the arena in front of the Colossus", 10, 35),
}
# the power-on moves that reach the opening film (docs/gow2-play.md) and the frame where --make-states saves it
FILM_MOVES = "300:start:8 600:cross:8 900:up:8 960:cross:8"
FILM_FRAME = 1000

# ---------------------------------------------------------------------------------------------------------------- Windows
if os.name != "nt":
    sys.exit("speedcheck.py reads the window title and the thread times with the Windows API; it runs on Windows only.")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
u32 = ctypes.WinDLL("user32", use_last_error=True)
ntdll = ctypes.WinDLL("ntdll")
H = ctypes.c_void_p


class FILETIME(ctypes.Structure):
    _fields_ = [("lo", wt.DWORD), ("hi", wt.DWORD)]

    def seconds(self):
        return ((self.hi << 32) | self.lo) / 1e7


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD), ("th32OwnerProcessID", wt.DWORD),
                ("tpBasePri", wt.LONG), ("tpDeltaPri", wt.LONG), ("dwFlags", wt.DWORD)]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD), ("th32DefaultHeapID", ctypes.c_size_t),
                ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", wt.LONG),
                ("dwFlags", wt.DWORD), ("szExeFile", wt.WCHAR * 260)]


class POWER_STATUS(ctypes.Structure):
    _fields_ = [("ACLineStatus", ctypes.c_ubyte), ("BatteryFlag", ctypes.c_ubyte), ("BatteryLifePercent", ctypes.c_ubyte),
                ("SystemStatusFlag", ctypes.c_ubyte), ("BatteryLifeTime", wt.DWORD), ("BatteryFullLifeTime", wt.DWORD)]


class GUID(ctypes.Structure):
    _fields_ = [("a", wt.DWORD), ("b", wt.WORD), ("c", wt.WORD), ("d", ctypes.c_ubyte * 8)]

    def __str__(self):
        return "%08x-%04x-%04x-%s-%s" % (self.a, self.b, self.c, bytes(self.d[:2]).hex(), bytes(self.d[2:]).hex())


def _sig(fn, restype, *argtypes):
    fn.restype = restype
    fn.argtypes = list(argtypes)


_sig(k32.CreateToolhelp32Snapshot, H, wt.DWORD, wt.DWORD)
_sig(k32.Thread32First, wt.BOOL, H, ctypes.POINTER(THREADENTRY32))
_sig(k32.Thread32Next, wt.BOOL, H, ctypes.POINTER(THREADENTRY32))
_sig(k32.Process32FirstW, wt.BOOL, H, ctypes.POINTER(PROCESSENTRY32W))
_sig(k32.Process32NextW, wt.BOOL, H, ctypes.POINTER(PROCESSENTRY32W))
_sig(k32.OpenThread, H, wt.DWORD, wt.BOOL, wt.DWORD)
_sig(k32.CloseHandle, wt.BOOL, H)
_sig(k32.OpenProcess, H, wt.DWORD, wt.BOOL, wt.DWORD)
_sig(k32.SetProcessAffinityMask, wt.BOOL, H, ctypes.c_size_t)
_sig(k32.SetPriorityClass, wt.BOOL, H, wt.DWORD)
_sig(k32.GetThreadTimes, wt.BOOL, H, *[ctypes.POINTER(FILETIME)] * 4)
_sig(k32.GetSystemTimes, wt.BOOL, *[ctypes.POINTER(FILETIME)] * 3)
_sig(k32.GetSystemPowerStatus, wt.BOOL, ctypes.POINTER(POWER_STATUS))
_sig(k32.LocalFree, H, H)
_sig(u32.EnumWindows, wt.BOOL, ctypes.c_void_p, wt.LPARAM)
_sig(u32.GetWindowThreadProcessId, wt.DWORD, wt.HWND, ctypes.POINTER(wt.DWORD))
_sig(u32.IsWindowVisible, wt.BOOL, wt.HWND)
_sig(u32.GetWindowTextW, ctypes.c_int, wt.HWND, wt.LPWSTR, ctypes.c_int)
_sig(u32.PostMessageW, wt.BOOL, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
_sig(u32.SetForegroundWindow, wt.BOOL, wt.HWND)
_sig(u32.GetForegroundWindow, wt.HWND)
_sig(u32.IsIconic, wt.BOOL, wt.HWND)
_sig(u32.ShowWindow, wt.BOOL, wt.HWND, ctypes.c_int)
try:
    _sig(k32.GetThreadDescription, ctypes.c_long, H, ctypes.POINTER(ctypes.c_wchar_p))
except AttributeError:  # Windows before 10 1607: no thread names
    k32.GetThreadDescription = None
WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
_sig(u32.EnumWindows, wt.BOOL, WNDENUMPROC, wt.LPARAM)
NCPU = os.cpu_count() or 1


def processes_named(name):
    """Process ids of every running program with that file name (case does not matter)."""
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    found = []
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name.lower():
            found.append(entry.th32ProcessID)
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return found


def thread_cpu(pid):
    """{thread id: (name, cpu seconds, start address)} of one process. Threads the runner named show their name; the others
    show their start address, which `llvm-nm -n ps2run.exe` turns into a function name."""
    snap = k32.CreateToolhelp32Snapshot(0x4, 0)
    out = {}
    entry = THREADENTRY32()
    entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            handle = k32.OpenThread(0x0840, False, entry.th32ThreadID)  # query information + query limited information
            if handle:
                times = [FILETIME() for _ in range(4)]
                if k32.GetThreadTimes(handle, *[ctypes.byref(t) for t in times]):
                    name = ""
                    if k32.GetThreadDescription:
                        text = ctypes.c_wchar_p()
                        if k32.GetThreadDescription(handle, ctypes.byref(text)) >= 0 and text.value:
                            name = text.value
                            k32.LocalFree(ctypes.cast(text, H))
                    start = ctypes.c_size_t(0)
                    ntdll.NtQueryInformationThread(H(handle), 9, ctypes.byref(start), ctypes.sizeof(start), None)  # ThreadQuerySetWin32StartAddress
                    out[entry.th32ThreadID] = (name, times[2].seconds() + times[3].seconds(), start.value or 0)
                k32.CloseHandle(handle)
        ok = k32.Thread32Next(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return out


def module_of(pid, address):
    """'module+0xoffset' for an address inside a process (a thread's start address), so an unnamed thread can be told apart:
    ps2run.exe+0x... is the runner's own code (llvm-nm -n ps2run.exe names it), anything else is a library such as SDL2 or a driver."""
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _sig(psapi.EnumProcessModulesEx, wt.BOOL, H, ctypes.POINTER(H), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD)
    _sig(psapi.GetModuleInformation, wt.BOOL, H, H, ctypes.c_void_p, wt.DWORD)
    _sig(psapi.GetModuleBaseNameW, wt.DWORD, H, H, wt.LPWSTR, wt.DWORD)
    handle = k32.OpenProcess(0x0410, False, pid)  # query information + read memory
    if not handle:
        return hex(address)
    try:
        mods = (H * 1024)()
        needed = wt.DWORD()
        psapi.EnumProcessModulesEx(handle, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
        for i in range(min(1024, needed.value // ctypes.sizeof(H))):
            info = (ctypes.c_size_t * 3)()  # MODULEINFO: base, size, entry point
            if psapi.GetModuleInformation(handle, mods[i], info, ctypes.sizeof(info)) and info[0] <= address < info[0] + info[1]:
                name = ctypes.create_unicode_buffer(260)
                psapi.GetModuleBaseNameW(handle, mods[i], name, 260)
                return f"{name.value}+{address - info[0]:#x}"
    finally:
        k32.CloseHandle(handle)
    return hex(address)


def system_times():
    """(busy seconds, total seconds) of all the PC's threads together since boot."""
    idle, kernel, user = FILETIME(), FILETIME(), FILETIME()
    k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kernel), ctypes.byref(user))
    total = kernel.seconds() + user.seconds()  # the kernel time already contains the idle time
    return total - idle.seconds(), total


def window_of(pid):
    """The game's main window: the visible top-level window of that process."""
    found = []

    def visit(hwnd, _):
        owner = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and u32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    u32.EnumWindows(WNDENUMPROC(visit), 0)
    return found[0] if found else None


def window_title(hwnd):
    buf = ctypes.create_unicode_buffer(512)
    u32.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


def bring_to_front(hwnd):
    """The game window in front, not minimised: a window nobody sees can be slowed down by Windows."""
    if u32.IsIconic(hwnd):
        u32.ShowWindow(hwnd, 9)  # SW_RESTORE
    u32.SetForegroundWindow(hwnd)
    return u32.GetForegroundWindow() == hwnd


def power_state():
    """Mains or battery, battery level, the Windows power mode (the slider) and the power plan. Read only."""
    status = POWER_STATUS()
    k32.GetSystemPowerStatus(ctypes.byref(status))
    info = {"mains": {0: False, 1: True}.get(status.ACLineStatus), "battery_percent": status.BatteryLifePercent if status.BatteryLifePercent <= 100 else None}
    modes = {"00000000-0000-0000-0000-000000000000": "Balanced", "961cc777-2547-4f9d-8174-7d86181b8a7a": "Best power efficiency",
             "3af9b8d9-7c97-431d-ad78-34a8bfea439f": "Better performance", "ded574b5-45a0-4f42-8737-46345c09c238": "Best performance",
             "ded574b5-45a0-4f42-8737-46345c09c238 ": "Best performance"}
    try:
        powrprof = ctypes.WinDLL("powrprof")
        guid = GUID()
        if powrprof.PowerGetEffectiveOverlayScheme(ctypes.byref(guid)) == 0:
            info["mode"] = modes.get(str(guid), str(guid))
    except OSError:
        pass
    try:
        plan = subprocess.run(["powercfg", "/getactivescheme"], capture_output=True, text=True, timeout=10).stdout
        match = re.search(r"\((.+)\)", plan)
        info["plan"] = match.group(1) if match else plan.strip()
    except (OSError, subprocess.SubprocessError):
        pass
    return info


def cpu_clocks():
    """The busiest logical processors and the speed they run at right now (Windows' own counters, through PowerShell)."""
    script = ("Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation | Where-Object { $_.Name -notmatch '_Total' } | "
              "ForEach-Object { '{0},{1},{2},{3}' -f $_.PercentProcessorTime, $_.ProcessorFrequency, $_.PercentProcessorPerformance, ($_.Name -replace '^0,','') }")
    try:
        text = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    cores = []
    for line in text.split():
        try:
            busy, base, perf, index = line.split(",")
            cores.append((float(busy), float(base) * float(perf) / 100.0, int(index)))
        except ValueError:
            continue
    cores.sort(reverse=True)
    return [(int(b), int(mhz), index) for b, mhz, index in cores[:4]]


# ---------------------------------------------------------------------------------------------------------------- sampling
_sig(k32.SuspendThread, wt.DWORD, H)
_sig(k32.ResumeThread, wt.DWORD, H)
_sig(k32.GetThreadContext, wt.BOOL, H, ctypes.c_void_p)


def modules_of(pid):
    """[(name, base, size)] of the modules loaded in a process."""
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    _sig(psapi.EnumProcessModulesEx, wt.BOOL, H, ctypes.POINTER(H), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD)
    _sig(psapi.GetModuleInformation, wt.BOOL, H, H, ctypes.c_void_p, wt.DWORD)
    _sig(psapi.GetModuleBaseNameW, wt.DWORD, H, H, wt.LPWSTR, wt.DWORD)
    handle = k32.OpenProcess(0x0410, False, pid)
    found = []
    if not handle:
        return found
    try:
        mods = (H * 1024)()
        needed = wt.DWORD()
        psapi.EnumProcessModulesEx(handle, mods, ctypes.sizeof(mods), ctypes.byref(needed), 3)
        for i in range(min(1024, needed.value // ctypes.sizeof(H))):
            info = (ctypes.c_size_t * 3)()
            if psapi.GetModuleInformation(handle, mods[i], info, ctypes.sizeof(info)):
                name = ctypes.create_unicode_buffer(260)
                psapi.GetModuleBaseNameW(handle, mods[i], name, 260)
                found.append((name.value, info[0], info[1]))
    finally:
        k32.CloseHandle(handle)
    return found


class Sampler:
    """Looks about every millisecond where each of the busiest threads of the runner is (the instruction pointer; the thread is
    stopped for a few microseconds). The runner's own PS2_PROFILE sees the emulation thread only; this sees the vector-unit
    thread and the window thread as well. Where the samples fall is turned into function names by --sample-report."""

    def __init__(self, pid, interval=0.001):
        self.pid, self.interval, self.stop_flag = pid, interval, False
        self.counts = {}  # tid -> {rip: n}
        self.modules = []  # taken while the runner is alive (afterwards the list can come back empty)
        self.attempts = 0
        self.thread = None

    def start(self):
        import threading
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def stop(self):
        self.stop_flag = True
        if self.thread:
            self.thread.join(timeout=10)

    def run(self):
        # busy threads: the ones that used 3 % of a core or more in one second
        self.modules = modules_of(self.pid)
        first = thread_cpu(self.pid)
        time.sleep(1.0)
        second = thread_cpu(self.pid)
        busy = [tid for tid, (name, cpu, start) in second.items() if cpu - first.get(tid, (name, 0.0, start))[1] >= 0.03]
        handles = {}
        for tid in busy:
            h = k32.OpenThread(0x004A, False, tid)  # suspend/resume, get context, query information
            if h:
                handles[tid] = h
        raw = ctypes.create_string_buffer(1232 + 16)
        at = (ctypes.addressof(raw) + 15) & ~15  # a CONTEXT must be 16-byte aligned
        flags = ctypes.c_uint32.from_address(at + 0x30)
        rip = ctypes.c_uint64.from_address(at + 0xF8)
        for tid in handles:
            self.counts[tid] = {}
        while not self.stop_flag:
            for tid, h in handles.items():
                if k32.SuspendThread(h) == 0xFFFFFFFF:
                    continue
                flags.value = 0x100001  # CONTEXT_CONTROL
                ok = k32.GetThreadContext(h, ctypes.c_void_p(at))
                where = rip.value
                k32.ResumeThread(h)
                if ok:
                    d = self.counts[tid]
                    d[where] = d.get(where, 0) + 1
            self.attempts += 1
            time.sleep(self.interval)
        for h in handles.values():
            k32.CloseHandle(h)


def save_samples(sampler, pid, path, threads_info):
    """The samples as a JSON file: modules (to turn an address into module + offset) and per thread the counts per address."""
    data = {"modules": sampler.modules or modules_of(pid), "rounds": sampler.attempts,
            "threads": {str(tid): {"info": threads_info.get(tid, {}), "samples": {hex(a): n for a, n in c.items()}} for tid, c in sampler.counts.items()}}
    Path(path).write_text(json.dumps(data), encoding="utf-8")


def nm_symbols(exe):
    """[(rva, name)] sorted by address: the functions the linker left in the runner (llvm-nm from the llvm-mingw tools)."""
    raw = Path(exe).read_bytes()[:4096]
    pe = int.from_bytes(raw[0x3C:0x40], "little")
    image_base = int.from_bytes(raw[pe + 24 + 24:pe + 24 + 32], "little")
    out = subprocess.run(["llvm-nm", "-n", "-C", "--defined-only", str(exe)], capture_output=True, text=True, errors="replace").stdout
    symbols = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in ("T", "t", "W", "w", "i"):
            try:
                symbols.append((int(parts[0], 16) - image_base, parts[2]))
            except ValueError:
                pass
    return symbols


SAMPLE_GROUPS = [
    ("compiled vector programs (gen::vu1_)", re.compile(r"^gen::vu1_")),
    ("game code (gen::unit_)", re.compile(r"^gen::")),
    ("vector unit: interpreter, macro code, run/lookup", re.compile(r"^(void )?ps2::vu::|ps2::vu0|^ps2::vuops")),
    ("vector interface (vif)", re.compile(r"ps2::vif::")),
    ("dma", re.compile(r"ps2::dmac::|ps2::dma")),
    ("gif: packet walking, handing to the graphics thread", re.compile(r"ps2::gif::")),
    ("queue (hand-over between threads)", re.compile(r"ps2::gs::queue::")),
    ("graphics chip emulation (gs.cpp, raster.cpp)", re.compile(r"ps2::gs::")),
    ("graphics driver (wgpu, gpu.cpp)", re.compile(r"wgpu|^ps2::gpu|naga|dxil|d3d")),
    ("sound / second processor (iop, spu2, sif, cdvd)", re.compile(r"ps2::(iop|spu2|sif|cdvd|sio2)")),
    ("memory copies (memcpy, memset, ...)", re.compile(r"memcpy|memset|memmove|memcmp")),
]


def sample_report(path, exe, top=22):
    """Turn a sample file into names: per thread the share of the samples by part of the console and the busiest functions."""
    import bisect
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    symbols = nm_symbols(exe)
    rvas = [s[0] for s in symbols]
    mods = data["modules"]
    exe_name = Path(exe).name.lower()
    base = next((b for n, b, sz in mods if n.lower() == exe_name), None)
    if base is None:
        sys.exit("the runner's module is not in the file")

    def name_of(address):
        for n, b, sz in mods:
            if b <= address < b + sz:
                if n.lower() == exe_name:
                    i = bisect.bisect_right(rvas, address - b) - 1
                    return symbols[i][1] if i >= 0 else f"ps2run+{address - b:#x}"
                return "[" + n.lower() + "]"
        return "[unknown]"

    print(f"{data['rounds']} rounds of sampling")
    for tid, t in sorted(data["threads"].items(), key=lambda kv: -sum(kv[1]["samples"].values())):
        samples = {int(a, 16): n for a, n in t["samples"].items()}
        total = sum(samples.values())
        info = t.get("info", {})
        start = info.get("start_address", 0)
        start_name = name_of(start) if start else ""
        print(f"\n=== thread {tid}  {info.get('name') or ''} starts in {start_name[:80]}  cpu {info.get('cpu_percent', '?')} % of a core, {total} samples")
        by_name = {}
        for a, n in samples.items():
            nm = name_of(a)
            by_name[nm] = by_name.get(nm, 0) + n
        groups = {}
        for nm, n in by_name.items():
            if nm.startswith("["):
                key = "outside the runner: " + nm
            else:
                for g, pattern in SAMPLE_GROUPS:
                    if pattern.search(nm):
                        key = g
                        break
                else:
                    key = "runner, other"
            groups[key] = groups.get(key, 0) + n
        for g, n in sorted(groups.items(), key=lambda kv: -kv[1])[:10]:
            print(f"  {n / total * 100:5.1f} %  {g}")
        print("  -- busiest functions")
        for nm, n in sorted(by_name.items(), key=lambda kv: -kv[1])[:top]:
            print(f"  {n / total * 100:5.1f} %  {nm[:150]}")


# ---------------------------------------------------------------------------------------------------------------- scenes
def scene_state(args, name):
    state, frame, moves = SCENES[name][:3]
    if state is None:
        return None, 0, moves
    for folder in (args.states, args.root / "games" / args.game / "play"):
        path = folder / (state if state.endswith(".state") else f"{args.game}_{state}_f{FILM_FRAME:06d}.state")
        if path.exists():
            return path, frame or FILM_FRAME, moves
    return None, frame, moves


def moves_to_args(text, start, root, game):
    """The move syntax of scripts/play.py as ps2run's --press and --axis arguments."""
    import play  # scripts/play.py

    tokens = []
    for token in text.split():
        if token.startswith("@") and not Path(token[1:]).is_absolute():
            token = "@" + str(root / "games" / game / "play" / token[1:])
        tokens.append(token)
    return play.parse_moves(" ".join(play.read_moves(" ".join(tokens))), start)


# ---------------------------------------------------------------------------------------------------------------- one run
def build_run(args, scene, backend, scale):
    """The command line, the environment and the scene's state for one run: (cmd, env, state, start frame) or a string saying why not."""
    root = args.root
    exe = Path(args.exe) if args.exe else root / "build" / f"native-{args.game}" / "ps2run.exe"
    core = root / "games" / args.game / "core.elf"  # port.sh unpacks a game whose real program sits in a disc file (Gran Turismo 4) to here
    program = core if core.exists() else next(iter(sorted((root / "games" / args.game / "disc").glob("[A-Z][A-Z][A-Z][A-Z]_[0-9][0-9][0-9].[0-9][0-9]"))), None)
    if not exe.exists() or program is None:
        sys.exit(f"no runner at {exe} or no game program in games/{args.game}/disc (or games/{args.game}/core.elf): build the game first (scripts/port.sh)")
    if scene in SCENES:
        state, start, moves = scene_state(args, scene)
        if SCENES[scene][0] and state is None:
            return f"no state for the scene {scene} (docs/speed.md)"
    else:
        state, start, moves = (Path(args.from_state) if args.from_state else None), args.from_frame, args.moves or ""
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    card = out / "speed-card.ps2"
    if args.memcard:  # a fresh copy of the given card for every run: the game writes to its card
        shutil.copyfile(args.memcard, card)
    cmd = [str(exe), str(program), "--disc", str(root / "games" / args.game / "disc.iso"), "--window", "--memcard", str(card),
           "--quick-dir", str(out / "quick")]
    if state:
        cmd += ["--load-state", str(state)]
    cmd += moves_to_args(moves, start, root, args.game) if moves else []
    cmd += args.extra
    env = dict(os.environ)
    for key in ("PS2_GPU_BACKEND", "PS2_SCALE"):
        env.pop(key, None)
    if backend != "default":
        env["PS2_GPU_BACKEND"] = backend
    if scale != 1:
        env["PS2_SCALE"] = str(scale)
    for pair in args.env:
        key, _, value = pair.partition("=")
        env[key] = value
    return cmd, env, state, start


def take_picture(args, scene, backend):
    """A short run that presses the report key (F9) by itself at a fixed frame and keeps the picture it writes: for looking at whether a
    graphics API draws the scene right. Returns the picture's path."""
    built = build_run(args, scene, backend, args.scale)
    if isinstance(built, str):
        return built
    cmd, env, state, start = built
    at = start + (900 if scene == "title" else 400)
    cmd += ["--frames", str(at + 30)]
    env["PS2_TEST_KEYS"] = f"{at}:report"
    reports = args.out / "quick" / "reports"
    before = set(reports.glob("*")) if reports.is_dir() else set()
    subprocess.run(cmd, cwd=args.root, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    made = sorted(set(reports.glob("*")) - before) if reports.is_dir() else []
    pictures = args.out / "pictures"
    pictures.mkdir(exist_ok=True)
    if not made or not (made[-1] / "picture.png").exists():
        return "no picture was written"
    target = pictures / f"{scene}-{backend}-x{args.scale}.png"
    target.write_bytes((made[-1] / "picture.png").read_bytes())
    for folder in made:  # a report is 55 MB; only the picture is wanted
        for f in folder.glob("*"):
            f.unlink()
        folder.rmdir()
    return str(target)


def run_once(args, scene, backend, scale, label):
    warmup, seconds = (SCENES[scene][4], SCENES[scene][5]) if scene in SCENES else (10, 35)
    args = argparse.Namespace(**{**vars(args), "warmup": args.warmup if args.warmup is not None else warmup, "seconds": args.seconds if args.seconds is not None else seconds})
    root, out = args.root, args.out
    built = build_run(args, scene, backend, scale)
    if isinstance(built, str):
        return {"scene": scene, "backend": backend, "skipped": built}
    cmd, env, state, start = built
    if args.frames:
        cmd += ["--frames", str(args.frames)]
    vu1 = root / "games" / args.game / "vu1"
    vu1_before = {p.name for p in vu1.glob("*.bin")} if vu1.is_dir() else set()
    logfile = out / f"run-{label}.log"
    result = {"label": label, "scene": scene, "backend_asked": backend, "scale": scale, "power": power_state(), "time": time.strftime("%Y-%m-%d %H:%M:%S"),
              "env": args.env, "exe": cmd[0], "contaminated": []}
    rivals = lambda mine: [p for p in processes_named("ps2run.exe") if p != mine]  # noqa: E731
    if rivals(0):
        result["contaminated"].append(f"ps2run.exe already running before the start (pids {rivals(0)})")
    with open(logfile, "wb") as log:
        proc = subprocess.Popen(cmd, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT)
    if args.priority:  # an experiment with the game we started: does Windows give it more speed with a higher priority?
        handle = k32.OpenProcess(0x0200, False, proc.pid)
        if handle:
            k32.SetPriorityClass(handle, {"above": 0x8000, "high": 0x80}[args.priority])
            k32.CloseHandle(handle)
        result["priority"] = args.priority
    if args.affinity:  # only the game we started: which logical processors it may use (an experiment, not a setting)
        handle = k32.OpenProcess(0x0200, False, proc.pid)  # PROCESS_SET_INFORMATION
        if handle:
            k32.SetProcessAffinityMask(handle, ctypes.c_size_t(int(args.affinity, 16)))
            k32.CloseHandle(handle)
        result["affinity"] = args.affinity
    try:
        t_start = time.time()
        hwnd = None
        while time.time() - t_start < 90 and hwnd is None and proc.poll() is None:
            hwnd = window_of(proc.pid)
            time.sleep(0.2)
        if hwnd is None:
            result["error"] = f"no window after 90 s (exit code {proc.poll()}); see {logfile}"
            return result
        result["front"] = bring_to_front(hwnd)
        t0 = time.time()
        samples, last_seen, last_sample = [], None, 0.0
        warm_done, mid_done = False, False
        thread_a = sys_a = thread_b = sys_b = None
        sampler = gpu_counter = None
        wall_a = wall_b = 0.0
        next_check = 0.0
        while proc.poll() is None:
            now = time.time() - t0
            if args.frames and now > args.seconds + 600:
                break
            if not args.frames and now >= args.warmup + args.seconds:
                break
            title = window_title(hwnd)
            match = TITLE.search(title)
            if match and (title != last_seen or time.time() - last_sample >= 0.95):
                last_seen, last_sample = title, time.time()
                if now >= args.warmup or args.frames:
                    samples.append((now, float(match.group(1)), int(match.group(2)), int(match.group(3)), int(match.group(4)), int(match.group(5))))
                    if title.count("(") or "Saved" in title:
                        result["contaminated"].append("a notice in the title: " + title[:60])
            if now >= next_check:
                next_check = now + 2.0
                other = rivals(proc.pid)
                if other:
                    result["contaminated"].append(f"another ps2run.exe is running (pids {other}) at {now:.0f} s")
            if not warm_done and now >= args.warmup:
                warm_done = True
                thread_a, sys_a, wall_a = thread_cpu(proc.pid), system_times(), time.time()
                if not args.no_gpu_engine:  # Windows' own counter of how busy the 3D engine is with the runner's work (the title bar's GPU % is wrong on some drivers)
                    gpu_counter = subprocess.Popen(["typeperf", f"\\GPU Engine(pid_{proc.pid}_*engtype_3D)\\Utilization Percentage", "-si", "1", "-sc", str(int(args.seconds) + 5)],
                                                   stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                if args.sample:
                    sampler = Sampler(proc.pid)
                    sampler.start()
            if warm_done and not mid_done and now >= args.warmup + args.seconds * 0.6:
                mid_done = True
                result["clocks"] = cpu_clocks()
            time.sleep(0.2)
        thread_b, sys_b, wall_b = thread_cpu(proc.pid), system_times(), time.time()
        if sampler is not None:
            sampler.stop()
        if gpu_counter is not None:
            gpu_counter.terminate()  # our own helper process
            try:
                text = (gpu_counter.communicate(timeout=5)[0] or b"").decode("cp1252", errors="replace")  # the German summary lines hold non-ASCII letters
            except subprocess.TimeoutExpired:
                gpu_counter.kill()
                text = ""
            sums = []
            for line in text.splitlines()[1:]:
                cells = [c.strip('"') for c in line.split('","')]
                try:
                    sums.append(sum(float(c) for c in cells[1:] if c.strip()))
                except ValueError:
                    pass  # the summary lines of typeperf
            if sums:
                result["gpu_3d"] = {"mean": round(statistics.mean(sums), 1), "max": round(max(sums), 1), "n": len(sums)}
        result["title_last"] = window_title(hwnd)
        if thread_a is not None and wall_b > wall_a:
            wall = wall_b - wall_a
            threads = []
            for tid, (name, cpu, start_address) in thread_b.items():
                base = thread_a.get(tid, (name, 0.0, start_address))[1]
                threads.append({"tid": tid, "name": name, "start": start_address, "cpu_percent": round((cpu - base) / wall * 100.0, 1)})
            threads.sort(key=lambda t: -t["cpu_percent"])
            if sampler is not None:
                sample_path = out / f"sample-{label}.json"
                save_samples(sampler, proc.pid, sample_path, {t["tid"]: {"name": t["name"], "cpu_percent": t["cpu_percent"], "start_address": t["start"]} for t in threads})
                result["sample_file"] = str(sample_path)
            result["threads"] = [t for t in threads if t["cpu_percent"] >= 1.0][:8]
            for t in result["threads"]:
                t["start"] = module_of(proc.pid, t["start"]) if not t["name"] else t["name"]
            mine = sum(t["cpu_percent"] for t in threads) / 100.0 * wall
            busy = sys_b[0] - sys_a[0]
            result["pc_cpu_percent"] = round(busy / max(sys_b[1] - sys_a[1], 1e-9) * 100.0, 1)
            result["other_cpu_percent"] = round(max(0.0, busy - mine) / max(sys_b[1] - sys_a[1], 1e-9) * 100.0, 1)
            if result["other_cpu_percent"] > OTHER_LOAD_LIMIT:
                result["contaminated"].append(f"the rest of the PC used {result['other_cpu_percent']} % of its CPU")
        if rivals(proc.pid):
            result["contaminated"].append("another ps2run.exe was running at the end")
        fps = [s[1] for s in samples]
        if fps:
            result["fps"] = {"mean": round(statistics.mean(fps), 1), "median": round(statistics.median(fps), 1), "min": min(fps), "max": max(fps),
                             "p10": round(sorted(fps)[len(fps) // 10], 1), "seconds_at_58_or_more": sum(f >= 58.0 for f in fps), "n": len(fps)}
            result["title_cpu_percent"] = round(statistics.mean(s[2] for s in samples))
            result["title_gpu_percent"] = round(statistics.mean(s[3] for s in samples))
            result["audio_ms_mean"] = round(statistics.mean(s[5] for s in samples))
            result["series"] = [round(f) for f in fps]
            for t in result.get("threads", []):  # work per frame: the game is capped at its disc's rate, so percent of a core alone hides how much a frame costs
                t["ms_per_frame"] = round(t["cpu_percent"] * 10.0 / max(result["fps"]["mean"], 1e-9), 2)
        return result
    finally:
        if proc.poll() is None:
            if hwnd:
                u32.PostMessageW(hwnd, 0x0010, 0, 0)  # WM_CLOSE: the game ends the way a player ends it
            try:
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()  # our own process only
                proc.wait()
        text = logfile.read_text(errors="replace") if logfile.exists() else ""
        match = re.search(r"\[gpu\] (.*?) on (Vulkan|Direct3D 12|\w+)", text)
        if match:
            result["gpu"], result["backend"] = match.group(1), match.group(2)
        match = re.search(r"\[gpu\] graphics API: (.*)", text)  # why this API: the default, PS2_GPU_BACKEND, or a fallback
        if match:
            result["api_note"] = match.group(1).strip()
        match = re.search(r"\[gpu\] presenting with (\w+)", text)
        if match:
            result["present_mode"] = match.group(1)
        result["new_vector_programs"] = len({p.name for p in vu1.glob("*.bin")} - vu1_before) if vu1.is_dir() else 0
        result["log_flags"] = sorted({m.group(0)[:80] for m in re.finditer(r"(?im)^.*(unhandled|missing|crash|cannot|error|\[ee\] halted: (?!window closed|frame limit)).*$", text)})[:5]
        result["exit_code"] = proc.returncode


def describe(r):
    """One printed line for one result."""
    if r.get("skipped"):
        return f"{r['scene']:9} skipped: {r['skipped']}"
    if r.get("error"):
        return f"{r['scene']:9} ERROR: {r['error']}"
    f = r.get("fps")
    power = r.get("power", {})
    where = "mains" if power.get("mains") else "BATTERY" if power.get("mains") is False else "power unknown"
    text = (f"{r['scene']:9} {r.get('backend', r['backend_asked']):12} x{r['scale']}  "
            + (f"fps mean {f['mean']:5.1f}  median {f['median']:5.1f}  min {f['min']:5.1f}  p10 {f['p10']:5.1f}  >=58: {f['seconds_at_58_or_more']}/{f['n']} s" if f else "no speed read")
            + f"  | title CPU {r.get('title_cpu_percent', '?')}% GPU {r.get('title_gpu_percent', '?')}% audio {r.get('audio_ms_mean', '?')} ms")
    threads = ", ".join(f"{t['name'] or t['start']} {t['cpu_percent']:.0f}%" + (f" ({t['ms_per_frame']:.1f} ms/frame)" if "ms_per_frame" in t else "") for t in r.get("threads", [])[:5])
    text += f"\n          threads (% of one thread, CPU ms per frame; the unnamed one is the vector units): {threads}   rest of PC {r.get('other_cpu_percent', '?')}%"
    if r.get("gpu_3d"):
        text += f"\n          graphics chip 3D engine (Windows counter): mean {r['gpu_3d']['mean']}%, max {r['gpu_3d']['max']}% over {r['gpu_3d']['n']} s"
    text += f"\n          {where}, {power.get('battery_percent', '?')}% battery, mode {power.get('mode', '?')}, plan {power.get('plan', '?')}"
    if r.get("clocks"):
        text += "; busiest logical CPUs (number: busy %, MHz; 0-3 are the two fast cores, the rest the small ones): " + ", ".join(f"#{i}: {b}%/{m}" for b, m, i in r["clocks"])
    if r.get("api_note") or r.get("present_mode"):
        text += f"\n          graphics API: {r.get('api_note', '?')}; presenting with {r.get('present_mode', '?')}"
    if r["contaminated"]:
        text += "\n          CONTAMINATED, throw away: " + "; ".join(sorted(set(r["contaminated"]))[:3])
    if r.get("new_vector_programs"):
        text += f"\n          {r['new_vector_programs']} NEW vector programs were recorded (the build did not know them)"
    return text


def table(path):
    """The results file as a markdown table (clean runs only)."""
    rows = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    print("| scene | graphics API | scale | fps mean | median | slowest second | seconds at 58+ | power | extras |")
    print("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        f = r.get("fps")
        if not f or r.get("contaminated"):
            continue
        p = r.get("power", {})
        print(f"| {r['scene']} | {r.get('backend', r['backend_asked'])} | {r['scale']} | {f['mean']} | {f['median']} | {f['min']} | {f['seconds_at_58_or_more']} of {f['n']} | "
              f"{'mains' if p.get('mains') else 'battery'}, {p.get('mode', '?')} | "
              + ", ".join(r.get("env", []) + ([f"affinity {r['affinity']}"] if r.get("affinity") else []) + ([f"priority {r['priority']}"] if r.get("priority") else [])) + " |")


# which part of the console each function of a PS2_PROFILE file belongs to (checked in this order)
PROFILE_GROUPS = [
    ("game code (recompiled EE code)", re.compile(r"^gen::unit_")),
    ("vector programs, compiled", re.compile(r"^gen::vu1_")),
    ("vector unit interpreter and VU0 macro code", re.compile(r"^(void )?ps2::vu::|^ps2::vu0")),
    ("graphics chip: drawing and vertex handling", re.compile(r"^ps2::gs::|^ps2::gif::|wgpu|^ps2::(present|render)")),
    ("vector interface / DMA / memory copies", re.compile(r"^(void )?ps2::(vif|dma)::|^ps2::(st|ld)(8|16|32|64|128)|^memcpy|^memset")),
    ("sound and the IOP (second processor)", re.compile(r"^ps2::(iop|spu2|sif|cdvd)")),
]


def profile_summary(path):
    """Add up a PS2_PROFILE file (docs/gow2.md) by the part of the console: where the emulation thread spends its time."""
    totals, inside, outside, waiting_modules = {}, 0.0, 0.0, {}
    section = ""
    header = ""
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.startswith("# ") and "samples of the emulation thread" in line:
            header = line
        if line.startswith("# where the thread was, in "):
            section = "in"
        elif line.startswith("# where the thread was, outside"):
            section = "out"
        elif line.startswith("# the function"):
            section = ""
        m = re.match(r"\s*([\d.]+)%\s+(\d+)\s+(.*)", line)
        if not m or not section:
            continue
        percent, name = float(m.group(1)), m.group(3)
        if section == "out":
            outside += percent
            waiting_modules[name.split("!")[0]] = waiting_modules.get(name.split("!")[0], 0.0) + percent
            continue
        inside += percent
        for group, pattern in PROFILE_GROUPS:
            if pattern.search(name):
                totals[group] = totals.get(group, 0.0) + percent
                break
        else:
            totals["other runtime code (kernel calls, memory, helpers)"] = totals.get("other runtime code (kernel calls, memory, helpers)", 0.0) + percent
    print(header)
    inside_total = None
    match = re.search(r"# (\d+) samples .*, (\d+) of them inside", header)
    if match:
        inside_total = int(match.group(2)) / int(match.group(1)) * 100.0
    for group, percent in sorted(totals.items(), key=lambda kv: -kv[1]):
        print(f"  {percent:5.1f} %  {group}")
    if inside_total is not None:  # the file lists only the busiest functions; the rest of the runner's own time is a long tail of small ones
        print(f"  {max(0.0, inside_total - inside):5.1f} %  runner code in many small functions the file does not list")
    print(f"  {outside:5.1f} %  outside the runner: waiting and system/driver code ("
          + ", ".join(f"{m} {v:.1f}" for m, v in sorted(waiting_modules.items(), key=lambda kv: -kv[1])[:4]) + ")")


def make_states(args):
    """The states that need the power-on: the opening film. One run per state, in a window, saved at a fixed frame."""
    out = args.states
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"{args.game}_film_f{FILM_FRAME:06d}.state"
    if target.exists():
        print(f"{target} exists")
        return
    root = args.root
    exe = Path(args.exe) if args.exe else root / "build" / f"native-{args.game}" / "ps2run.exe"
    program = next(iter(sorted((root / "games" / args.game / "disc").glob("[A-Z][A-Z][A-Z][A-Z]_[0-9][0-9][0-9].[0-9][0-9]"))))
    cmd = [str(exe), str(program), "--disc", str(root / "games" / args.game / "disc.iso"), "--window", "--memcard", str(out / "state-card.ps2"), "--quick-dir",
           str(out / "quick"), "--frames", str(FILM_FRAME + 5), "--save-state", f"{FILM_FRAME}:{target}"]
    cmd += moves_to_args(FILM_MOVES, 0, root, args.game)
    print("running the game to the film (about", FILM_FRAME // 30, "seconds) ...")
    subprocess.run(cmd, cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)
    print(target, "written" if target.exists() else "NOT written")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], epilog="Extra ps2run options go after --.")
    ap.add_argument("--root", type=Path, default=HERE.parent, help="the folder with games/ and build/ (default: this repo)")
    ap.add_argument("--game", default="gow2")
    ap.add_argument("--exe", help="the runner (default: build/native-<game>/ps2run.exe)")
    ap.add_argument("--out", type=Path, default=None, help="folder for the results file, logs and own states (default: games/<game>/speed)")
    ap.add_argument("--scene", action="append", help=f"one of {', '.join(SCENES)} (repeatable)")
    ap.add_argument("--all", action="store_true", help="every scene")
    ap.add_argument("--from", dest="from_state", help="your own scene: a state file")
    ap.add_argument("--from-frame", type=int, default=0, help="the frame of that state (the number in its name)")
    ap.add_argument("--moves", help="your own scene: buttons, scripts/play.py syntax")
    ap.add_argument("--name", default="custom", help="the name of your own scene in the results (default: custom)")
    ap.add_argument("--memcard", type=Path, help="a memory card to start from (a fresh copy for every run, so the file itself is never changed), for a state that needs a saved profile")
    ap.add_argument("--backends", default="default", help="comma list of default, vulkan, d3d12")
    ap.add_argument("--scale", type=int, default=1, help="PS2_SCALE, the internal resolution (1 to 3)")
    ap.add_argument("--seconds", type=float, help="seconds measured (default: 30 or 35, depends on the scene)")
    ap.add_argument("--warmup", type=float, help="seconds to let the scene settle first (default: 4 to 10, depends on the scene)")
    ap.add_argument("--frames", type=int, help="end the run at this frame by itself instead of after --seconds (for PS2_PROFILE runs)")
    ap.add_argument("--repeat", type=int, default=1, help="times each run is taken")
    ap.add_argument("--env", action="append", default=[], help="extra environment variable KEY=VALUE for the runner (repeatable)")
    ap.add_argument("--no-limit", action="store_true", help="PS2_NO_LIMIT=1: no speed limit (no wait for the sound device or the clock), to see what the game could do; the sound is out of step")
    ap.add_argument("--affinity", help="hex mask of the logical processors the game may use, for experiments (for example F = only the two fast cores)")
    ap.add_argument("--priority", choices=["above", "high"], help="run the game with a higher process priority, for experiments")
    ap.add_argument("--make-states", action="store_true", help="make the film state (power-on run, once) and stop")
    ap.add_argument("--table", help="print this results file as a markdown table and stop")
    ap.add_argument("--picture", action="store_true", help="instead of timing: take one picture of every scene and backend (a short run each)")
    ap.add_argument("--profile-summary", help="add up a PS2_PROFILE file by part of the console and stop")
    ap.add_argument("--no-gpu-engine", action="store_true", help="do not read Windows' 3D engine counter of the runner (by default it is read once a second with typeperf)")
    ap.add_argument("--sample", action="store_true", help="look where each busy thread of the runner is, about every millisecond, and keep it in sample-<run>.json")
    ap.add_argument("--sample-report", help="name the functions in such a file (with --exe, default build/native-<game>/ps2run.exe) and stop")
    ap.add_argument("extra", nargs="*")
    args = ap.parse_args()
    if args.no_limit and "PS2_NO_LIMIT=1" not in args.env:
        args.env.append("PS2_NO_LIMIT=1")  # goes into every result line's "env" as well, so a table shows which runs had no limit
    if args.table:
        return table(args.table)
    if args.profile_summary:
        return profile_summary(args.profile_summary)
    if args.sample_report:
        return sample_report(args.sample_report, args.exe or args.root / "build" / f"native-{args.game}" / "ps2run.exe")
    args.out = args.out or args.root / "games" / args.game / "speed"
    args.states = args.out / "states"
    if args.make_states:
        return make_states(args)
    scenes = list(SCENES) if args.all else (args.scene or ([] if args.from_state or args.moves else ["title"]))
    if args.from_state or args.moves:
        scenes.append("custom")
    results_file = args.out / "speed-results.jsonl"
    args.out.mkdir(parents=True, exist_ok=True)
    print(f"results go to {results_file}")
    for scene in scenes:
        if scene not in SCENES and scene != "custom":
            sys.exit(f"unknown scene {scene}; choose from {', '.join(SCENES)}")
    if "film" in scenes and scene_state(args, "film")[0] is None:
        make_states(args)  # the film scene starts from a state that only a power-on run can make (once, about a minute)
    for scene in scenes:
        for backend in args.backends.split(","):
            if args.picture:
                print(scene, backend, take_picture(args, scene, backend), flush=True)
                continue
            for n in range(args.repeat):
                shown = args.name if scene == "custom" else scene
                label = f"{shown}-{backend}-x{args.scale}-{time.strftime('%H%M%S')}"
                result = run_once(args, scene, backend, args.scale, label)
                result["scene"] = shown
                print(describe(result), flush=True)
                with open(results_file, "a", encoding="utf-8") as f:
                    f.write(json.dumps(result) + "\n")
                time.sleep(3)
    return 0


if __name__ == "__main__":
    sys.exit(main())
