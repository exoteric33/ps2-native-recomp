#!/usr/bin/env python3
"""Run the ps2autotests suite through scripts/port.sh and compare the text with the expected output.

    python scripts/autotests.py                       fetch what is missing, run every test, write the summary
    python scripts/autotests.py cpu-ee-alu dma-*      only the tests matching these names or paths (glob or substring)
    python scripts/autotests.py --list                show the tests and whether their files are in place
    python scripts/autotests.py --summary             rebuild summary.md from the results already on disk

The exit code is 1 when a test HANGs, FAILs, has no result or DIFFs without being in KNOWN_DIFF (or --known-diff),
so a script or CI can gate on it; --no-fail always exits 0.

Every test goes through the same six steps as a game (disc, executable, translate, explore, vector units, build)
with `port.sh at-<name> <image> --native-only --frames N`; the text its program prints is cut out of the exploring
run's log (`-- TEST BEGIN` to `-- TEST END`) and diffed against the `.expected` file of the suite. See
docs/autotests.md.

The test programs come from unknownbrackets/ps2autotests (ISC) at one pinned commit and stay out of git:
isos/autotests/<name>/TEST.ELF + expected + test.iso. Results (logs, diffs, result.json per test, summary.md)
go to games/_autotests/ by default, which git ignores too. Python 3 standard library only.
"""
import argparse
import concurrent.futures
import difflib
import fnmatch
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

import ps2tools
# The pieces shared with report.py live in ps2tools.py: running port.sh, Git Bash, the compile-job pool, the tools, the log scan.
from ps2tools import ROOT, RUNTIME_LINE, STEPS, JobPool, die, ensure_tools, fail_note, git_state, read_lines, say, scan_runtime

REPO = 'unknownbrackets/ps2autotests'
COMMIT = '97469ffbed8631277b94e28d01dabd702aa97ef3'
PREFIX = 'at-'

ps2tools.set_program('autotests')


# ---------------------------------------------------------------- fetching

def http_get(url, tries=3):
    for attempt in range(tries):
        try:
            request = urllib.request.Request(url, headers={'User-Agent': 'ps2-native-recomp-autotests'})
            with urllib.request.urlopen(request, timeout=60) as response:
                return response.read()
        except Exception as error:
            if attempt == tries - 1:
                die('cannot download %s: %s' % (url, error))
            time.sleep(1 + attempt)


def isos_dir(options):
    return os.path.abspath(options.dir)


def listing(options):
    """The EE tests of the pinned commit: [(name, 'cpu/ee/alu')], from the git tree (cached on disk)."""
    cache = os.path.join(isos_dir(options), 'tree-%s.json' % options.commit[:7])
    if os.path.exists(cache):
        with open(cache) as f:
            paths = json.load(f)
    else:
        url = 'https://api.github.com/repos/%s/git/trees/%s?recursive=1' % (REPO, options.commit)
        tree = json.loads(http_get(url))
        if tree.get('truncated'):
            die('the git tree listing is truncated')
        files = {entry['path'] for entry in tree['tree'] if entry['type'] == 'blob'}
        paths = sorted(p[len('tests/'):-len('.elf')] for p in files
                       if p.startswith('tests/') and p.endswith('.elf') and p[:-4] + '.expected' in files)
        os.makedirs(isos_dir(options), exist_ok=True)
        with open(cache, 'w') as f:
            json.dump(paths, f)
    tests = [(path.replace('/', '-'), path) for path in paths]
    if len({name for name, _ in tests}) != len(tests):
        die('two tests map to the same name')
    return tests


def test_dir(options, name):
    return os.path.join(isos_dir(options), name)


def have_files(options, name):
    folder = test_dir(options, name)
    return os.path.exists(os.path.join(folder, 'TEST.ELF')) and os.path.exists(os.path.join(folder, 'expected'))


def fetch(options, tests):
    """Download the missing programs and expected files from raw.githubusercontent.com at the pinned commit."""
    missing = [(name, path) for name, path in tests if not have_files(options, name)]
    license_file = os.path.join(isos_dir(options), 'LICENSE.md')
    if not os.path.exists(license_file):
        os.makedirs(isos_dir(options), exist_ok=True)
        with open(license_file, 'wb') as f:
            f.write(http_get('https://raw.githubusercontent.com/%s/%s/LICENSE.md' % (REPO, options.commit)))
    if not missing:
        return

    def one(item):
        name, path = item
        base = 'https://raw.githubusercontent.com/%s/%s/tests/%s' % (REPO, options.commit, path)
        elf = http_get(base + '.elf')
        expected = http_get(base + '.expected')
        if elf[:4] != b'\x7fELF':
            die('%s.elf is not an ELF file' % path)
        folder = test_dir(options, name)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, 'expected'), 'wb') as f:
            f.write(expected)
        with open(os.path.join(folder, 'SOURCE'), 'w') as f:
            f.write('%s tests/%s at %s\n' % (REPO, path, options.commit))
        with open(os.path.join(folder, 'TEST.ELF'), 'wb') as f:  # last, so a half-fetched test counts as missing
            f.write(elf)
        return name

    say('fetching %d test programs from %s at %s' % (len(missing), REPO, options.commit[:7]))
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for _ in pool.map(one, missing):
            pass


def select(tests, patterns):
    if not patterns:
        return tests
    chosen = []
    for name, path in tests:
        for pattern in patterns:
            pattern = pattern.strip('/')
            if any(c in pattern for c in '*?['):
                hit = fnmatch.fnmatch(name, pattern) or fnmatch.fnmatch(path, pattern)
            else:
                hit = pattern in (name, path) or pattern in name
            if hit:
                chosen.append((name, path))
                break
    return chosen


# ---------------------------------------------------------------- running one test

POOL = JobPool(0)


def run_port(options, name, iso, log_path):
    """Run port.sh; returns (exit code or None on timeout, [(step number, seconds into the run)], seconds)."""
    args = [PREFIX + name, iso, '--native-only', '--frames', str(options.frames)]
    if options.rounds:
        args += ['--rounds', str(options.rounds)]
    return ps2tools.run_port(args, log_path, options.timeout, POOL)


def strip_blank_tail(lines):
    lines = list(lines)
    while lines and lines[-1] == '':
        lines.pop()
    return lines


def extract_text(lines):
    """The program's text: -- TEST BEGIN to -- TEST END (or to the end of the log), without runtime log lines."""
    begin = next((i for i, line in enumerate(lines) if line.startswith('-- TEST BEGIN')), None)
    if begin is None:
        return None, False, 0
    end = next((i for i in range(begin, len(lines)) if lines[i].startswith('-- TEST END')), None)
    region = lines[begin:(end + 1 if end is not None else len(lines))]
    text = [line for line in region if not RUNTIME_LINE.match(line)]
    return strip_blank_tail(text), end is not None, len(region) - len(text)


def count_differences(expected, got):
    matcher = difflib.SequenceMatcher(None, expected, got, autojunk=False)
    return sum(max(i2 - i1, j2 - j1) for tag, i1, i2, j1, j2 in matcher.get_opcodes() if tag != 'equal')


def first_difference(expected, got):
    matcher = difflib.SequenceMatcher(None, expected, got, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag != 'equal':
            want = expected[i1] if i1 < i2 else '(nothing)'
            have = got[j1] if j1 < j2 else '(nothing)'
            return i1 + 1, want, have
    return None


def build_dir(name):
    return os.path.join(ROOT, 'build', 'native-' + PREFIX + name)


def run_test(options, name, path, out_root, rev):
    out = os.path.join(out_root, name)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    folder = test_dir(options, name)
    elf, iso = os.path.join(folder, 'TEST.ELF'), os.path.join(folder, 'test.iso')
    game = os.path.join(ROOT, 'games', PREFIX + name)
    result = {'name': name, 'path': path, 'frames': options.frames, 'rev': rev, 'finished': time.strftime('%Y-%m-%d %H:%M:%S')}

    if options.clean:
        for victim in (game, os.path.join(ROOT, 'generated', PREFIX + name), build_dir(name)):
            shutil.rmtree(victim, ignore_errors=True)
    if not os.path.exists(iso) or os.path.getmtime(iso) < os.path.getmtime(elf):
        made = subprocess.run([sys.executable, os.path.join(ROOT, 'scripts', 'mkiso.py'), iso, elf], capture_output=True, text=True)
        if made.returncode != 0:
            result.update(status='FAIL', step='image', note=(made.stderr or made.stdout).strip()[:160])
            return finish(out, result)
        shutil.rmtree(game, ignore_errors=True)  # a new image: do not let port.sh reuse the old one

    port_log = os.path.join(out, 'port.log')
    code, steps, seconds = run_port(options, name, iso, port_log)
    result['seconds'] = round(seconds, 1)
    result['step_seconds'] = {STEPS[n]: round((steps[i + 1][1] if i + 1 < len(steps) else seconds) - t, 1) for i, (n, t) in enumerate(steps)}
    last_step = steps[-1][0] if steps else 0
    explore_log = os.path.join(game, 'explore.log')
    log_fresh = os.path.exists(explore_log) and os.path.getmtime(explore_log) >= time.time() - seconds - 5
    if log_fresh:
        shutil.copyfile(explore_log, os.path.join(out, 'explore.log'))
    result.update(port_code=code, last_step=last_step, timeout=options.timeout)
    return judge(options, name, out, result)


def judge(options, name, out, result):
    """Classify a finished port.sh run from what it left in `out` (port.log, explore.log) and fill in `result`."""
    code, last_step = result['port_code'], result['last_step']
    port_log = os.path.join(out, 'port.log')
    lines = read_lines(os.path.join(out, 'explore.log'))
    syscalls, missing, warnings, halted, frames = scan_runtime(lines)
    result.update(syscalls=syscalls, iop_missing=missing, warnings=warnings, halted=halted, ran_frames=frames,
                  rounds=sum(1 for l in read_lines(port_log) if l.startswith('round ')))

    timed_out = code is None
    if (timed_out and not (last_step == 4 and lines)) or (not timed_out and code != 0):
        step = STEPS.get(last_step, 'start')
        why = 'timeout after %ds' % result['timeout'] if timed_out else fail_note(port_log)
        if last_step == 4 and not timed_out:
            step = 'build'  # the exploring step only fails while it builds the runner (the run itself is `|| true`)
        result.update(status='FAIL', step=step, note=why)
        return finish(out, result)

    with open(os.path.join(test_dir(options, name), 'expected'), 'rb') as f:
        expected = strip_blank_tail(f.read().decode('latin-1').replace('\r\n', '\n').split('\n'))
    got, complete, dropped = extract_text(lines)
    result['dropped_log_lines'] = dropped
    for stale in ('test.out', 'test.diff'):
        if os.path.exists(os.path.join(out, stale)):
            os.remove(os.path.join(out, stale))
    if got is not None:
        with open(os.path.join(out, 'test.out'), 'w', encoding='latin-1', newline='\n') as f:
            f.write('\n'.join(got) + '\n')
    result.update(expected_lines=len(expected), got_lines=len(got or []))

    reason = halted if halted and halted != 'frame limit reached' else None
    if timed_out:
        reason = 'timeout after %ds' % result['timeout']
    if not complete:
        if got is None:
            note = 'no output at all'
        else:
            note = 'stops after %d of %d lines, last: %s' % (len(got), len(expected), (got[-1] if got else '')[:60])
        if reason:
            note += '; ' + reason
        elif frames is None:
            note += '; the runner died (no stop line)'
        else:
            note += '; ran to the frame limit'
        result.update(status='HANG', note=note)
    else:
        differences = count_differences(expected, got)
        if differences:
            diff = list(difflib.unified_diff(expected, got, 'expected', 'got', lineterm='', n=2))
            with open(os.path.join(out, 'test.diff'), 'w', encoding='latin-1', newline='\n') as f:
                f.write('\n'.join(diff) + '\n')
            at, want, have = first_difference(expected, got)
            result.update(status='DIFF', differences=differences, note='%d line%s differ; first at line %d: want `%s` got `%s`' % (
                differences, '' if differences == 1 else 's', at, want.strip()[:50], have.strip()[:50]))
        else:
            note = '%d lines' % len(expected)
            if halted and halted != 'Exit':
                note += '; no Exit (%s)' % halted
            result.update(status='PASS', note=note)
    return finish(out, result)


def reclassify(options, tests, out_root):
    """Judge the logs of earlier runs again (after changing the rules above), without running anything."""
    for name, path in tests:
        file = os.path.join(out_root, name, 'result.json')
        if not os.path.exists(file):
            continue
        with open(file) as f:
            result = json.load(f)
        if 'port_code' not in result:
            say('%s: result.json predates --reclassify, run the test again' % name)
            continue
        for key in ('note', 'step', 'differences'):
            result.pop(key, None)
        judge(options, name, os.path.join(out_root, name), result)


def finish(out, result):
    with open(os.path.join(out, 'result.json'), 'w') as f:
        json.dump(result, f, indent=1, sort_keys=True)
    return result


# ---------------------------------------------------------------- summary

ORDER = ['PASS', 'DIFF', 'HANG', 'FAIL']
# Tests whose DIFF is known and does not fail the run (docs/autotests.md says why); --known-diff adds more.
KNOWN_DIFF = {'vu/lower/efu'}


def load_results(out_root, tests):
    results = []
    for name, path in tests:
        file = os.path.join(out_root, name, 'result.json')
        if os.path.exists(file):
            with open(file) as f:
                results.append(json.load(f))
    return results


def summarize(out_root, tests, options):
    results = load_results(out_root, tests)
    counts = {s: sum(1 for r in results if r['status'] == s) for s in ORDER}
    revs = sorted({r['rev'] for r in results})
    frames = sorted({r['frames'] for r in results})
    seconds = sum(r.get('seconds', 0) for r in results)
    out = ['# ps2autotests results', '',
           '%d of %d tests have a result: **%s**.' % (len(results), len(tests), ', '.join('%d %s' % (counts[s], s) for s in ORDER)), '',
           'Runtime revision %s, frame limit %s, ps2autotests at %s. Sum of the per-test times %d s.' % (
               ', '.join(revs) or '?', ', '.join(map(str, frames)) or '?', options.commit[:7], seconds), '',
           '| Test | Result | Note |', '|---|---|---|']
    for r in results:
        out.append('| `%s` | %s | %s |' % (r['path'], r['status'] if r['status'] != 'FAIL' else 'FAIL (%s)' % r.get('step', '?'), r.get('note', '').replace('|', '\\|')))
    # what the logs showed, by how many tests it showed up in
    seen = {}
    for r in results:
        for label, table in (('unhandled syscall', r.get('syscalls', {})), ('iop missing', r.get('iop_missing', {})), ('warning', r.get('warnings', {}))):
            for key, count in table.items():
                seen.setdefault((label, key), []).append((r['path'], count))
    if seen:
        out += ['', '## Seen in the logs', '', '| Kind | What | Tests | Calls |', '|---|---|---|---|']
        for (label, key), hits in sorted(seen.items(), key=lambda kv: (-len(kv[1]), kv[0])):
            out.append('| %s | `%s` | %d | %d |' % (label, key.replace('|', '\\|'), len(hits), sum(c for _, c in hits)))
    out.append('')
    text = '\n'.join(out)
    with open(os.path.join(out_root, 'summary.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    return text, counts


def verdict(out_root, tests, options):
    """The exit code: 1 when a test HANGs, FAILs, has no result or DIFFs without being a known DIFF."""
    known = KNOWN_DIFF | {p.strip().strip('/') if '/' in p else p.strip().replace('-', '/') for p in options.known_diff}
    results = {r['path']: r for r in load_results(out_root, tests)}
    bad = []
    for name, path in tests:
        r = results.get(path)
        if r is None:
            bad.append('%s no result' % path)
        elif r['status'] in ('HANG', 'FAIL') or (r['status'] == 'DIFF' and path not in known):
            bad.append('%s %s' % (path, r['status']))
        elif r['status'] == 'PASS' and path in KNOWN_DIFF:
            say('note: %s passes now; take it out of KNOWN_DIFF in scripts/autotests.py' % path)
    if bad:
        say('not as expected (%d): %s' % (len(bad), ', '.join(bad)))
    return 1 if bad and not options.no_fail else 0


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description='Run the ps2autotests suite through port.sh and compare the text with the expected output.')
    parser.add_argument('tests', nargs='*', help='names or paths (cpu-ee-alu, cpu/ee/alu), globs (dma-*) or substrings; default all')
    parser.add_argument('--frames', type=int, default=600, help='frame limit of the exploring run (default 600)')
    parser.add_argument('--timeout', type=int, default=600, help='seconds one port.sh run may take before it is killed (default 600)')
    parser.add_argument('--jobs', '-j', type=int, default=4, help='tests at the same time (default 4)')
    parser.add_argument('--tokens', type=int, default=-1, help='compile jobs all the tests share (Windows; default: cores minus tests at a time; 0 = every build uses its own default)')
    parser.add_argument('--rounds', type=int, default=0, help='pass --rounds N to port.sh (exploring runs at most)')
    parser.add_argument('--clean', action='store_true', help='delete the test\'s games/, generated/ and build/ folders first (a cold run)')
    parser.add_argument('--out', default=os.path.join(ROOT, 'games', '_autotests'), help='results folder (default games/_autotests)')
    parser.add_argument('--dir', default=os.path.join(ROOT, 'isos', 'autotests'), help='where the test programs live (default isos/autotests)')
    parser.add_argument('--commit', default=COMMIT, help='ps2autotests commit to fetch (default %s)' % COMMIT[:7])
    parser.add_argument('--list', action='store_true', help='list the selected tests and exit')
    parser.add_argument('--fetch', action='store_true', help='only download the selected tests and exit')
    parser.add_argument('--reclassify', action='store_true', help='judge the logs of the last run again with the current rules (nothing is run), then rebuild summary.md')
    parser.add_argument('--summary', action='store_true', help='only rebuild summary.md from the results on disk')
    parser.add_argument('--known-diff', action='append', default=[], metavar='PATH', help='a test (vu/lower/efu or vu-lower-efu) whose DIFF does not fail the run, repeatable; always known: %s' % ', '.join(sorted(KNOWN_DIFF)))
    parser.add_argument('--no-fail', action='store_true', help='exit 0 whatever the results (by default a HANG, FAIL, missing result or unknown DIFF exits 1)')
    options = parser.parse_args()

    tests = listing(options)
    chosen = select(tests, options.tests)
    if not chosen:
        die('no test matches %s' % ' '.join(options.tests))
    out_root = os.path.abspath(options.out)
    if options.list:
        for name, path in chosen:
            say('%-40s %-34s %s' % (name, path, 'in place' if have_files(options, name) else 'not fetched'))
        return 0
    if options.reclassify:
        reclassify(options, chosen, out_root)
        options.summary = True
    if options.summary:
        say(summarize(out_root, chosen, options)[0])
        return verdict(out_root, chosen, options)
    fetch(options, chosen)
    if options.fetch:
        return 0
    ensure_tools()
    os.makedirs(out_root, exist_ok=True)
    rev = git_state()
    started = time.time()
    say('running %d tests, %d at a time, %d frames each, results in %s' % (len(chosen), options.jobs, options.frames, out_root))
    done = 0
    lock = threading.Lock()
    global POOL
    tokens = options.tokens if options.tokens >= 0 else max(2, (os.cpu_count() or 4) - options.jobs)
    POOL = JobPool(tokens)
    interrupted = False

    def job(item):
        nonlocal done
        result = run_test(options, item[0], item[1], out_root, rev)
        with lock:
            done += 1
            say('[%d/%d] %-4s %-38s %5.0fs  %s' % (done, len(chosen), result['status'], item[1], result.get('seconds', 0), result.get('note', '')[:90]))
        return result

    try:
        with concurrent.futures.ThreadPoolExecutor(max(1, options.jobs)) as pool:
            list(pool.map(job, chosen))
    except KeyboardInterrupt:
        say('interrupted')
        interrupted = True
    POOL.close()
    text, counts = summarize(out_root, chosen, options)
    say('')
    say('%s  (%d s wall clock; %s)' % (', '.join('%d %s' % (counts[s], s) for s in ORDER), time.time() - started, os.path.join(out_root, 'summary.md')))
    code = verdict(out_root, chosen, options)
    return 1 if interrupted and not options.no_fail else code  # results on disk may then be older ones


if __name__ == '__main__':
    sys.exit(main())
