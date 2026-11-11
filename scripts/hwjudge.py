#!/usr/bin/env python3
"""The card path's memory judge: run a scene from a state for a fixed number of frames with a fixed clock and one thread,
keep the 4 MB of graphics memory at the end, and compare two such dumps.

    python scripts/hwjudge.py run --game gow2 --state games/gow2/play/gow2_f010150.state --frames 60 --out <dir> --tag wide [--wide] [--env K=V ...]
    python scripts/hwjudge.py diff <a.raw> <b.raw>

`run` prints the hash the runner logs ("[gs] graphics memory hash ...") and leaves <dir>/<tag>.raw and <dir>/<tag>.log.
`--wide` sets the wide routing (PS2_GPU_HW=1 PS2_GPU_HW_INDEP_MAX=all PS2_GPU_HW_SWITCH_MIN=0) with Vulkan and pipelines built
before first use (PS2_GPU_HW_ASYNC=0) and the dual-source fallback (PS2_GPU_HW_DUAL=0), so the run is repeatable. `diff` prints how many 32-bit words differ and in which 8 KiB pages.
Standard library only.
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WIDE = {"PS2_GPU_BACKEND": "vulkan", "PS2_GPU_HW": "1", "PS2_GPU_HW_INDEP_MAX": "all", "PS2_GPU_HW_SWITCH_MIN": "0", "PS2_GPU_HW_ASYNC": "0",
        "PS2_GPU_HW_DUAL": "0"}  # dual-source blending is not repeatable on Intel graphics (hwdraw.cpp, want_dual): the fallback is


def boot_elf(game):
    disc = ROOT / "games" / game / "disc"
    elfs = [p for p in disc.iterdir() if p.is_file() and re.match(r"^[A-Z]{4}_\d{3}\.\d{2}", p.name)]
    if len(elfs) != 1:
        sys.exit(f"cannot tell the boot program in {disc}")
    return elfs[0]


def run(a):
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    m = re.search(r"_f(\d+)", Path(a.state).name)
    start = int(m.group(1)) if m else 0
    exe = Path(a.exe) if a.exe else ROOT / "build" / f"native-{a.game}" / "ps2run.exe"
    raw, log = out / f"{a.tag}.raw", out / f"{a.tag}.log"
    env = dict(os.environ, PS2_ONE_THREAD="1", PS2_DUMP_VRAM_RAW=str(raw), SDL_AUDIODRIVER="dummy")
    if a.wide:
        env.update(WIDE)
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    cmd = [str(exe), str(boot_elf(a.game)), "--disc", str(ROOT / "games" / a.game / "disc.iso"), "--load-state", str(a.state),
           "--frames", str(start + a.frames), "--rtc", "1700000000"]
    if a.moves:
        cmd += ["--press", a.moves]
    with open(log, "w", encoding="utf-8", errors="replace") as f:
        code = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT).returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    h = re.findall(r"graphics memory hash ([0-9a-f]+)", text)
    print(f"{a.tag}: exit {code}, hash {h[-1] if h else 'none'}")


def diff(a):
    x, y = Path(a.a).read_bytes(), Path(a.b).read_bytes()
    if len(x) != len(y):
        sys.exit("the dumps differ in size")
    pages = {}
    words = 0
    for i in range(0, len(x), 4):
        if x[i:i + 4] != y[i:i + 4]:
            words += 1
            pages[i // 8192] = pages.get(i // 8192, 0) + 1
    print(f"{words} words differ in {len(pages)} pages" + (": " + " ".join(f"{p}({n})" for p, n in sorted(pages.items())[:40]) if pages else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--game", default="gow2")
    r.add_argument("--exe")
    r.add_argument("--state", required=True)
    r.add_argument("--frames", type=int, default=60)
    r.add_argument("--moves", help="pad presses, the runner's --press syntax")
    r.add_argument("--out", required=True)
    r.add_argument("--tag", required=True)
    r.add_argument("--wide", action="store_true")
    r.add_argument("--env", action="append", default=[])
    d = sub.add_parser("diff")
    d.add_argument("a")
    d.add_argument("b")
    a = ap.parse_args()
    run(a) if a.cmd == "run" else diff(a)


if __name__ == "__main__":
    main()
