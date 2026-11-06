# Sourced by port.sh: ps2_ninja <build folder> [ninja arguments...] runs ninja on a game's build folder and survives a compiler
# that crashes on one generated unit (public issue 50: clang-23 died on unit_0096.cpp of Chronicles of Narnia).
#
# What it does when ninja fails:
#   1. It shows what went wrong in a few lines that keep the useful part: the failed commands, the exception code, the
#      numbered "Stack dump" lines (they name the compiler pass), the first frames, then the end of the log. Before, only the last
#      30 lines of ninja's output were shown and the exception code was cut off.
#   2. If the failed file made the *compiler* crash (a signal or exception, "PLEASE submit a bug report", "Stack dump", "LLVM ERROR";
#      a normal "error:" is a real mistake in the code and stops the build as before), it builds that one file again, a step lower
#      each time, with the rest of the build going on:
#         same flags once more (a crash that depends on memory or timing goes away)  ->  -O1  ->  -O0  ->  -O0 -mno-avx2
#      The retries use half the jobs, which also helps when a PC runs short of memory, and go on past a failed file (ninja -k 0)
#      so that one round finds all the units that crash.
#   3. The steps are kept in <build folder>/lowopt.txt, which cmake/lowopt.cmake reads, so the next rounds build that unit the same way.
#      A file that compiles fine is never touched: no list, no change to any flag.
# PS2_LOWOPT=0 turns the retries off (the build then stops at the first crash as it always did).
# Needs: $py (an array with a Python 3, set by port.sh) and, for the job count, $ninja_jobs (e.g. "-j5", may be empty).

# ps2_ninja_decide <ninja.log> <lowopt.txt>: reads the log, finds the files that crashed the compiler, moves each one a step down in
# lowopt.txt and says so. Exit 0: something changed, build again. Exit 1: nothing left to try. Exit 2: no compiler crash in the log.
ps2_ninja_decide() {
  "${py[@]}" -I - "$@" << 'EOF'
import os, re, sys
log_path, list_path = sys.argv[1], sys.argv[2]
crash = re.compile(r'frontend command failed|PLEASE submit a bug report|^Stack dump:|LLVM ERROR|error in backend|unable to execute command|Exception Code:')
lines = open(log_path, errors='replace').read().splitlines()
blocks, cur = [], None
for line in lines:
    if line.startswith('FAILED:'):
        cur = (line, [])
        blocks.append(cur)
    elif re.match(r'\[\d+/\d+\] ', line) or line.startswith('ninja:'):
        cur = None
    elif cur is not None:
        cur[1].append(line)
crashed = []   # (file name, did the failed command have -mavx2)
for head, body in blocks:
    if not any(crash.search(l) for l in body):
        continue
    m = re.search(r'(\S+\.(?:o|obj))(?:\s|$)', head)
    if not m or 'ps2game.dir' not in m.group(1):
        continue   # only generated sources: the cmake side knows no others
    name = re.sub(r'\.(?:o|obj)$', '', m.group(1).replace('\\', '/').rsplit('/', 1)[-1])
    avx2 = any('-mavx2' in l.split() for l in body[:4])
    if name not in [c[0] for c in crashed]:
        crashed.append((name, avx2))
if not crashed:
    sys.exit(2)
entries, retried, others = {}, set(), []
if os.path.exists(list_path):
    for line in open(list_path).read().splitlines():
        parts = line.split(None, 1)
        if line.startswith('# retried '):
            retried.add(line[len('# retried '):].strip())
        elif len(parts) == 2 and not line.startswith('#'):
            entries[parts[0]] = parts[1].strip()
        elif line.strip():
            others.append(line)
changed = False
for name, avx2 in crashed:
    flags = entries.get(name, '')
    if not flags and name not in retried:
        retried.add(name)
        print('[port] %s crashed the compiler; trying it once more' % name)
        changed = True
        continue
    if not flags:
        nxt = '-O1'
    elif '-mno-avx2' in flags:
        nxt = None
    elif '-O0' in flags:
        nxt = '-O0 -mno-avx2' if avx2 else None
    else:
        nxt = '-O0'
    if nxt is None:
        print('[port] %s crashes the compiler even at its lowest setting (%s)' % (name, flags or 'normal flags'))
        continue
    entries[name] = nxt
    print('[port] %s crashed the compiler; building it with %s (the rest of the game is built as before)' % (name, nxt))
    changed = True
if changed:
    with open(list_path, 'w', newline='\n') as out:
        for line in others:
            out.write(line + '\n')
        for name in sorted(retried):
            out.write('# retried %s\n' % name)
        for name in sorted(entries):
            out.write('%s %s\n' % (name, entries[name]))
sys.exit(0 if changed else 1)
EOF
}

# ps2_ninja_report <ninja.log>: the few lines of a failed ninja run that matter, in about 40 lines.
ps2_ninja_report() {
  local log=$1
  grep -E '^FAILED:' "$log" | cut -c1-240 | head -n 4 || true
  grep -E 'Exception Code|^Stack dump|^[0-9]+\.[[:space:]]|LLVM ERROR|error in backend|frontend command failed|PLEASE submit|unable to execute' "$log" | cut -c1-300 | head -n 14 || true
  grep -E '^ ?#[0-9]+ ' "$log" | head -n 6 | cut -c1-200 || true
  tail -n 14 "$log" | cut -c1-300
}

ps2_ninja() {
  local dir=$1 log rc round=0 jobs="$ninja_jobs" half keep=
  shift
  log="$dir/ninja.log"
  half=$(( ${PS2_JOBS:-$(nproc 2> /dev/null || echo 4)} / 2 ))
  [ "$half" -ge 2 ] || half=2
  while :; do
    rc=0
    ninja -C "$dir" $jobs $keep "$@" > "$log" 2>&1 || rc=$?
    [ "$rc" != 0 ] || return 0
    round=$((round + 1))
    ps2_ninja_report "$log"
    cp "$log" "$dir/ninja-failed-$round.log" 2> /dev/null || true
    [ "${PS2_LOWOPT:-1}" != 0 ] && [ "$round" -le 10 ] || return "$rc"
    if ps2_ninja_decide "$log" "$dir/lowopt.txt"; then
      jobs="-j$half"
      keep="-k 0"   # go on past a failed file, so one round finds every unit that crashes the compiler
    else
      return "$rc"
    fi
  done
}
