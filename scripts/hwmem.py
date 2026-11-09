#!/usr/bin/env python3
"""Does the card path's memory grow?

    python3 scripts/hwmem.py run stand --frames 20000 --out stand-wide.log           # wide routing, headless, scripted pad presses
    python3 scripts/hwmem.py run ship --mode painter --out ship-painter.log           # today's drawing (PS2_GPU_HW off)
    python3 scripts/hwmem.py run stand --window 9 --env PS2_NO_LIMIT=1 --out w.log     # in a window on Hyprland workspace 9, game speed limit off
    python3 scripts/hwmem.py table stand-wide.log stand-painter.log                   # numbers at 0, 1, 2 ... minutes and the slope per minute after the first minute

`run` starts the game from a save state with PS2_GPU_HW_MEMLOG on, so the program prints one `[memlog] key=value ...` line every 300 frames (the keys are the names in the table the `table` command prints), and keeps the
scene playing with a fixed series of presses (attack, jump, grab, stick moves; never start or select). `table` reads those lines.
Run it from a worktree whose games/<name>/ holds the built game (bash scripts/port.sh <name> isos/<name>.iso --native-only --no-explore) and the states.
"""
import argparse
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENES = {  # name: (game, boot file, state, frame the state was saved at)
    "hall": ("gow2", "SCUS_974.81", "gow2_f010150.state", 10150),  # the first hall
    "stand": ("gow2", "SCUS_974.81", "gow2_f022800.state", 22800),  # the hall stand-in
    "arena": ("gow2", "SCUS_974.81", "gow2_f055800.state", 55800),
    "ship": ("gow1", "SCUS_973.99", "gow1_f008700.state", 8700),
}
WIDE = {"PS2_GPU_HW": "1", "PS2_GPU_HW_INDEP_MAX": "all", "PS2_GPU_HW_SWITCH_MIN": "0"}


def presses(first, count):
    """A fixed series of pad presses (absolute frame numbers) that keeps a fight going."""
    out, seed, f = [], 12345, first + 30
    buttons = ["square", "square", "triangle", "circle", "cross", "square+triangle", "r1"]
    sticks = ["lleft", "lright", "lup", "ldown"]
    while f < first + count:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        r = seed >> 8
        if r % 3 == 0:
            out.append(f"--press={f}:{sticks[r % 4]}:{20 + r % 40}")
        else:
            out.append(f"--press={f}:{buttons[r % len(buttons)]}:6")
        f += 12 + (r >> 4) % 30
    return out


def run(a):
    game, boot, state, first = SCENES[a.scene]
    exe = a.exe or os.path.join(ROOT, "build", f"native-{game}", "ps2run")
    env = dict(os.environ)
    for k in list(env):
        if k.startswith("PS2_GPU_HW"):
            del env[k]
    if a.mode == "wide":
        env.update(WIDE)
    elif a.mode != "painter":
        sys.exit("--mode wide or painter")
    env["PS2_GPU_HW_MEMLOG"] = str(a.every)
    env.setdefault("SDL_AUDIODRIVER", "dummy")  # the runs make no sound
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    cmd = [exe, f"games/{game}/disc/{boot}", "--disc", f"games/{game}/disc.iso", "--load-state", f"games/{game}/play/{state}", "--frames", str(first + a.frames)]
    for p in presses(first, a.frames):
        cmd += p.split("=", 1)[0:1] + [p.split("=", 1)[1]]
    start = time.time()
    if a.show:  # a plain window (any system); it takes the focus
        cmd.append("--window")
    if a.window:  # a window on workspace N, not focused (Hyprland), the way games/gow2/play/linux-play-arena.sh starts it
        pidfile = a.out + ".pid"
        if os.path.exists(pidfile):
            os.remove(pidfile)
        quoted = " ".join("'" + c + "'" for c in cmd[1:] + ["--window"])
        envs = " ".join(f"{k}='{v}'" for k, v in env.items() if k.startswith(("PS2_", "SDL_")))
        line = f"echo $$ > '{pidfile}'; cd '{ROOT}'; exec env {envs} '{cmd[0]}' {quoted} 2> '{a.out}' > /dev/null"
        for attempt in range(3):  # hyprctl failed once in a long series
            done = subprocess.run(["hyprctl", "eval", f"hl.exec_cmd([[{line}]], {{ workspace = '{a.window} silent' }})"], capture_output=True, text=True)
            if done.returncode == 0:
                break
            print(f"hyprctl failed ({done.returncode}): {(done.stdout + done.stderr).strip()[:200]}", file=sys.stderr)
            time.sleep(3)
        else:
            sys.exit("hyprctl did not start the game")
        while not os.path.exists(pidfile):
            time.sleep(0.2)
        pid = int(open(pidfile).read())
        while os.path.exists(f"/proc/{pid}"):
            time.sleep(2)
        rc = "?"
    else:
        with open(a.out, "w") as log:
            rc = subprocess.call(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL, stderr=log)
    print(f"{a.scene} {a.mode}{' window' if a.window else ''}: exit {rc}, {time.time() - start:.0f} s wall, log {a.out}")


def parse(path):
    rows = []
    for line in open(path, errors="replace"):
        if not line.startswith("[memlog] "):
            continue
        row = {}
        for kv in line[9:].split():
            k, _, v = kv.partition("=")
            try:
                row[k] = float(v.split("/")[0])  # w_buf=24/24: allocated / kept by us
            except ValueError:
                pass
        rows.append(row)
    return rows


KEYS = ["rss", "rss_anon", "rss_file", "rss_shmem", "threads", "submits", "inflight", "max_inflight", "card_tex", "card_tex_mb", "card_buf", "card_buf_mb", "card_bg", "card_rpipe", "w_buf", "w_tex", "w_view", "w_bg", "w_rpipe", "w_cpipe", "w_cmdbuf", "w_shader",
        "w_layout", "w_playout", "live_targets", "live_retired", "live_heights", "live_pictures_mb", "live_pipes", "live_vtx_mb", "live_setups_mb", "live_params_mb", "live_ops", "live_runs_mb",
        "made_tex", "made_buf", "made_bg", "made_rpipe", "made_enc", "made_rp", "made_cp"]


def table(a):
    import glob
    for path in [p for pattern in a.logs for p in (sorted(glob.glob(pattern)) or [pattern])]:  # the shell on Windows does not expand run-*.log
        rows = parse(path)
        if len(rows) < 2:
            print(f"{path}: {len(rows)} lines")
            continue
        total = rows[-1]["t"]
        marks = [0.0]
        step = a.step
        while marks[-1] + step < total:
            marks.append(marks[-1] + step)
        picks = []
        for m in marks:
            picks.append(min(rows, key=lambda r: abs(r["t"] - m)))
        picks.append(rows[-1])
        print(f"\n{path}: {len(rows)} lines, {total:.0f} s, frames {rows[0]['frame']:.0f} to {rows[-1]['frame']:.0f}")
        print(f"{'':18}" + "".join(f"{('t=%.0f' % p['t']):>9}" for p in picks) + f"{'slope/min':>11}")
        late = [r for r in rows if r["t"] >= a.after]
        for k in KEYS:
            if k not in rows[-1]:
                continue
            vals = [p.get(k, 0.0) for p in picks]
            slope = 0.0
            if len(late) >= 3:  # least-squares slope per minute over the lines after the warm-up
                xs, ys = [r["t"] for r in late], [r.get(k, 0.0) for r in late]
                mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
                den = sum((x - mx) ** 2 for x in xs)
                slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den * 60 if den else 0.0
            print(f"{k:18}" + "".join(f"{v:9.1f}" if v != int(v) or "mb" in k or k == "rss" else f"{int(v):9d}" for v in vals) + f"{slope:11.2f}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("scene", choices=sorted(SCENES))
    r.add_argument("--mode", default="wide")
    r.add_argument("--frames", type=int, default=20000, help="frames after the state (about 55 a second here: 20000 is about 6 minutes)")
    r.add_argument("--every", type=int, default=300, help="frames between lines")
    r.add_argument("--exe", help="ps2run to use (default build/native-<game>/ps2run)")
    r.add_argument("--window", help="open a window on this Hyprland workspace (not focused) instead of running headless")
    r.add_argument("--show", action="store_true", help="open a plain window (it takes the focus); on the laptop speedcheck.py does this better")
    r.add_argument("--out", required=True)
    r.add_argument("--env", action="append", default=[], help="NAME=value for the game")
    t = sub.add_parser("table")
    t.add_argument("logs", nargs="+")
    t.add_argument("--step", type=float, default=60.0, help="seconds between columns")
    t.add_argument("--after", type=float, default=60.0, help="the slope is fitted over the lines after this many seconds")
    a = p.parse_args()
    run(a) if a.cmd == "run" else table(a)


if __name__ == "__main__":
    main()
