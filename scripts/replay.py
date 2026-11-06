#!/usr/bin/env python3
"""Play a recorded session again, without a window, and check that the game goes the same way (docs/replay.md).

  python scripts/replay.py games/gow2/play/states/quick_2026-10-25_10-00-00_f055800.state
  python scripts/replay.py games/gow2/play/states/quick_..._f055800.state.inputs
  python scripts/replay.py report-folder/recording.state.inputs

A window run writes what the game's controller ports got into <state>.inputs next to every quick save (F5), and run_<time>.inputs
for the start of a run. This starts the game headless from the state, gives it the same pad states at the same frames, and compares a
checkpoint every 15 frames. The answer is one of
  same to the end                               exit 0
  DIFFERENT: the game went another way          exit 3  (between the two frames it names; the recorded input around there is listed)
  the game ended before the recording did       exit 4
  wrong state / file                            exit 5
Nothing is built and nothing is written next to the game: the memory card the recording began with is used from a temporary folder.
The recording's header says which card that is: "card file" = the copy beside the recording (<recording>.ps2), "card state" = the
copy beside the quick save (<state>.ps2), "card none" = the run had no card. A recording from before that line existed has no
answer: the copy beside the state is used when there is one, else no card; --card new|none|FILE says what the run had (a run that
was started with --memcard and a file that did not exist yet had a "new" card).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NAMES = ["select", "l3", "r3", "start", "up", "right", "down", "left", "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"]


def read_header(path):
    head = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if re.match(r"(p\d|h|e) \d", line):
                break
            key, _, value = line.rstrip("\n").partition(" ")
            head[key] = value
    return head


def buttons_text(mask):
    return "+".join(n for i, n in enumerate(NAMES) if mask & (1 << i)) or "nothing"


def find_inputs_and_state(arg, state_arg):
    p = Path(arg)
    if not p.exists():
        sys.exit(f"replay: {arg} does not exist")
    if p.name.endswith(".inputs"):
        inputs = p
        state = None
        stem = p.name[: -len(".inputs")]
        if stem.endswith(".prev"):
            stem = stem[: -len(".prev")]
        if (p.parent / stem).exists() and stem.endswith(".state"):
            state = p.parent / stem
    else:
        inputs = Path(str(p) + ".inputs")
        state = p
        if not inputs.exists():
            sys.exit(f"replay: no recording next to {p} (looked for {inputs.name})")
    head = read_header(inputs)
    start = head.get("start", "")
    if state_arg:
        state = Path(state_arg)
    elif state is None and start.startswith("state "):
        named = start.split()[1]
        for candidate in (inputs.parent / named, ROOT / named):
            if candidate.exists():
                state = candidate
                break
        else:
            sys.exit(f"replay: the recording began from {named}; give it with --state")
    if start.startswith("power-on"):
        state = None
    return inputs, state, head


def resolve(path_text, game, what):
    """A path from the recording's header: as written (relative to the folder the run was started in = the repo), else the game's folder."""
    if path_text:
        for candidate in (Path(path_text), ROOT / path_text):
            if candidate.exists():
                return candidate
    if game:
        base = ROOT / "games" / game
        name = Path(path_text).name if path_text else ""
        guess = base / ("disc.iso" if what == "disc" else f"disc/{name}")
        if guess.exists():
            return guess
    sys.exit(f"replay: cannot find the {what} ({path_text or 'not in the recording'}); give --game <name> or --{what}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("recording", help="a quick save (.state) or a recording (.inputs)")
    ap.add_argument("--state", help="the state to start from, when it is not next to the recording")
    ap.add_argument("--game", help="game folder name under games/ (default: guessed from the program path)")
    ap.add_argument("--game-dir", type=Path, help="game folder for caches and build inputs (default: from the recording)")
    ap.add_argument("--program", help="the game's program (default: from the recording)")
    ap.add_argument("--disc", help="the disc image (default: from the recording)")
    ap.add_argument("--exe", help="ps2run (default: build/native-<game>/ps2run)")
    ap.add_argument("--card", help="the memory card the recording began with: none, new (an empty new card) or a file (default: from the recording)")
    ap.add_argument("--log", help="keep the full log in this file")
    ap.add_argument("--continue", dest="go_on", action="store_true",
                    help="after a checkpoint that differs, play on to the end of the recording (for a recording that cannot replay exactly, "
                         "like the ones made before 2026-10-28, which left the game's clock to the host)")
    ap.add_argument("--rtc", help="the game's clock at emulated time 0, in seconds since 1970 (default: the recording's 'rtc' line)")
    ap.add_argument("--env", action="append", default=[], help="NAME=VALUE for the run (PS2_ONE_THREAD=1, ...)")
    args = ap.parse_args()

    inputs, state, head = find_inputs_and_state(args.recording, args.state)
    program_text = args.program or head.get("program", "")
    game = args.game
    game_dir = args.game_dir
    if game_dir is None:
        recorded_dir = head.get("game-dir", "")
        candidates = [ROOT / "games" / game] if game else []
        if recorded_dir:
            candidates += [Path(recorded_dir), ROOT / recorded_dir]
        game_dir = next((p.resolve() for p in candidates if p.is_dir()), None)
    elif not game_dir.is_dir():
        sys.exit(f"replay: game folder is not a directory: {game_dir}")
    else:
        game_dir = game_dir.resolve()
    if not game:
        m = re.search(r"(?:games|program)[/\\]([^/\\]+)[/\\]", program_text)  # games\gow2\disc\... (the repo) or program\gow2\... (a game folder made for a player)
        game = m.group(1) if m else None
        if not game and game_dir and game_dir.parent.name == "games":
            game = game_dir.name
    disc_boot = not args.program and head.get("disc-program") == "1"
    program = None if disc_boot else (Path(args.program) if args.program else resolve(program_text, game, "program"))
    disc = Path(args.disc) if args.disc else resolve(head.get("disc", ""), game, "disc")
    exe = Path(args.exe) if args.exe else None
    if exe is None:
        for name in ("ps2run.exe", "ps2run"):
            for base in ([ROOT / "build" / f"native-{game}"] if game else []) + sorted((ROOT / "build").glob("native-*")):
                if (base / name).exists():
                    exe = base / name
                    break
            if exe:
                break
    if exe is None or not exe.exists():
        sys.exit("replay: no ps2run found; give --exe (build one with scripts/port.sh, this script builds nothing)")

    work = Path(tempfile.mkdtemp(prefix="replay_"))
    try:
        cmd = [str(exe), *([] if disc_boot else [str(program)]), "--disc", str(disc), "--replay", str(inputs), "--quick-dir", str(work)]
        if game_dir:
            cmd += ["--game-dir", str(game_dir)]
        if args.go_on:
            cmd += ["--replay-continue"]
        if args.rtc:
            cmd += ["--rtc", args.rtc]
        if state:
            cmd += ["--load-state", str(state)]
        # the card as it was when the recording began; the copy in the temporary folder takes the changes the game makes
        kind = head.get("card", "")
        card = None
        if args.card:
            if args.card not in ("none", "new"):
                card = Path(args.card)
                if not card.exists():
                    sys.exit(f"replay: the card {args.card} does not exist")
            kind = args.card if args.card in ("none", "new") else "file"
        elif kind == "file":
            card = Path(str(inputs) + ".ps2")
        elif kind == "state":
            card = Path(str(state) + ".ps2") if state else None
        elif kind == "":  # an older recording: no line about the card
            card = Path(str(state) + ".ps2") if state and Path(str(state) + ".ps2").exists() else None
            kind = "state" if card else "none"
        if kind in ("file", "state") and (card is None or not card.exists()):
            sys.exit(f"replay: the card of the recording is missing ({card}); --card none|new|FILE replays without it, but a game that "
                     "reads the card will then most likely go another way")
        if card is not None and kind in ("file", "state"):
            shutil.copyfile(card, work / "card.ps2")
        if kind != "none":
            cmd += ["--memcard", str(work / "card.ps2")]
        env = dict(os.environ)
        for item in args.env:
            k, _, v = item.partition("=")
            env[k] = v
        log_path = Path(args.log) if args.log else work / "replay.log"
        print(f"replaying {inputs.name}" + (f" from {state.name}" if state else " from the power-on"))
        with open(log_path, "wb") as log:
            code = subprocess.run(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, env=env).returncode
        text = log_path.read_text(encoding="utf-8", errors="replace")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    lines = [l for l in text.splitlines() if l.startswith("[replay]")]
    for l in lines:
        print(l)
    bad = [l for l in text.splitlines() if re.search(r"unhandled|missing|cannot|\[state\] cannot", l)]
    if bad:
        print("also in the log:")
        for l in bad[:5]:
            print("  " + l[:160])
    if code == 0:
        print("RESULT: same to the end")
    elif code == 3:
        m = re.search(r"between frame (\d+) and frame (\d+)", text)
        print("RESULT: the game went another way" + (f" between frame {m.group(1)} and frame {m.group(2)}" if m else ""))
        if m:
            lo, hi = int(m.group(1)), int(m.group(2))
            print(f"recorded pad input from frame {lo - 30} to {hi}:")
            shown = 0
            for line in inputs.read_text(encoding="utf-8", errors="replace").splitlines():
                mm = re.match(r"p(\d) (\d+) ([0-9A-F]{4}) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d)", line)
                if mm and lo - 30 <= int(mm.group(2)) <= hi:
                    print(f"  frame {mm.group(2)} port {mm.group(1)}: {buttons_text(int(mm.group(3), 16))}, sticks right {mm.group(4)} {mm.group(5)} left {mm.group(6)} {mm.group(7)}")
                    shown += 1
            if not shown:
                print("  (no input changed in that time)")
        # the two causes that are not the program's fault (docs/replay.md)
        if "card" not in head:
            print("note: this recording has no 'card' line (made before 2026-10-27). If the run had a memory card (--memcard), the game "
                  "saw it and the replay did not: try --card new, or --card FILE for a card that already held saves.")
        print(f"note: recorded {head.get('recorded', 'on a date the recording does not say')}. A change in the program that alters what the "
              "game does (docs/replay.md, 'Changes that make older recordings obsolete') makes every older recording go another way; "
              "replay it with the ps2run.exe it was made with to tell.")
    elif code == 4:
        print("RESULT: the game ended before the recording did (see the log)")
    elif code == 5:
        print("RESULT: wrong state or unusable recording")
    else:
        print(f"RESULT: ps2run ended with code {code}")
    if code not in (0, 3, 4, 5) or (code == 4 and not lines):
        print(text[-1500:])
    return code


if __name__ == "__main__":
    sys.exit(main())
