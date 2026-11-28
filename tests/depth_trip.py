"""The depth round trip (runtime/src/gs/gs.cpp), no game needed.

Builds tests/depth_trip.cpp against the newest native runtime in build/ (made by any port.sh build), runs it with PS2_DEPTH_TRIP=0
(every depth download waits: the exact path) and =1, and wants the same line for every case; with =1 the trip log must show uploads
put back from the copy. It also runs it with no PS2_DEPTH_TRIP set (the default: on on Windows, where the page guards exist, off
elsewhere) and wants the =1 log on Windows and no trip elsewhere; and on Windows a build with host/crash.cpp that faults on purpose while a
trip is armed (`crash`): the crash report must still be written. Then measures the trip case on both paths (--timing). Prints
"depth trip: 0 failed" when all is well.

    python tests/depth_trip.py [--runtime build/runtime-native-.../libps2runtime.a] [--timing ROUNDS] [--scale N]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(runtime, out, crash=False):
    cxx = shutil.which('clang++')
    if not cxx:
        sys.exit('no clang++ on PATH')
    wgpu = os.path.join(ROOT, 'third_party', 'wgpu-native')
    flags = ['-DPS2_GPU', '-I' + os.path.join(ROOT, 'runtime', 'include'), '-I' + os.path.join(ROOT, 'runtime', 'src'),
             '-I' + os.path.join(wgpu, 'include'), '-mavx2', '-ffp-contract=off', '-O2', '-std=gnu++20']
    libs = [runtime, os.path.join(wgpu, 'lib', 'libwgpu_native.a')]
    if os.name == 'nt':
        libs += ['-static-libgcc', '-static-libstdc++', '-lws2_32', '-luserenv', '-lntdll', '-lsynchronization', '-lpthread', '-lm',
                 '-lkernel32', '-luser32', '-lgdi32', '-lshell32', '-lole32', '-loleaut32', '-luuid', '-ladvapi32', '-lwinmm', '-ld3dcompiler', '-lopengl32']
    else:
        libs += ['-ldl', '-lpthread', '-lm']
    sources = [os.path.join(ROOT, 'tests', 'depth_trip.cpp')]
    if crash:  # the runner's crash report (host/crash.cpp) linked in, for `depth_trip crash`
        flags += ['-DDEPTH_TRIP_CRASH', '-I' + os.path.join(ROOT, 'host')]
        sources.append(os.path.join(ROOT, 'host', 'crash.cpp'))
    cmd = [cxx] + flags + sources + ['-o', out] + libs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit('build failed')


def run(exe, trip, args=(), scale=None, log='2'):
    env = dict(os.environ, PS2_DEPTH_TRIP_LOG=log)
    env.pop('PS2_DEPTH_TRIP', None)
    if trip is not None:  # None: the default
        env['PS2_DEPTH_TRIP'] = str(trip)
    if scale:
        env['PS2_SCALE'] = str(scale)
    r = subprocess.run([exe] + list(args), capture_output=True, text=True, env=env, timeout=600)
    return r.returncode, r.stdout.splitlines(), r.stderr.splitlines()


def notes_of(log):
    return [l for l in log if l.startswith('[depth-trip]')]


# A real crash while a trip is armed must reach host/crash.cpp's report (the guard's vectored handler passes on what is not a trip).
def crash_check(runtime, scale):
    exe = os.path.join(ROOT, 'build', 'depth-trip-crash-test.exe')
    build(runtime, exe, crash=True)
    failed = 0
    reads = {}
    for trip in (0, None):
        code, out, err = run(exe, trip, ['crash'], scale=scale)
        reads[trip] = [l for l in out if l.startswith('crash: the guarded buffer read')]
        report = [l for l in err if l.startswith('[crash] ')]
        hook = any(l.startswith("[test] the crash report's after hook ran (crashed 1)") for l in err)
        stops = notes_of(err)
        ok = (code & 0xFFFFFFFF) == 0xC0000005 and report and 'access violation writing address 0x10' in report[0] and hook and \
            not any('swallowed' in l for l in out) and 'a trip is armed' in ' '.join(out)
        if trip is None:  # the default: the trip ran, the read stopped in the guard
            ok = ok and stops and ' 0 guard stops' not in stops[-1]
        failed += not ok
        print(('same  ' if ok else 'DIFF  ') + 'crash (PS2_DEPTH_TRIP=%s): exit 0x%08X, %s%s' % (
            'default' if trip is None else trip, code & 0xFFFFFFFF, report[0] if report else 'no crash report',
            '' if trip is not None else ', ' + (stops[-1].split(': ', 1)[-1] if stops else 'no trip')))
        if not ok:
            for l in (out + err)[-12:]:
                print('      ' + l)
    if reads[0] != reads[None] or not reads[0]:
        print('DIFF  crash: the guarded buffer read %s with the trip, %s without' % (reads[None], reads[0]))
        failed += 1
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runtime')
    ap.add_argument('--timing', type=int, default=200)
    ap.add_argument('--scale', type=int)
    o = ap.parse_args()
    runtime = o.runtime
    if not runtime:
        found = sorted(glob.glob(os.path.join(ROOT, 'build', 'runtime-native-*', 'libps2runtime.a')), key=os.path.getmtime)
        if not found:
            sys.exit('no build/runtime-native-*/libps2runtime.a: build a game first (scripts/port.sh)')
        runtime = found[-1]
    exe = os.path.join(ROOT, 'build', 'depth-trip-test' + ('.exe' if os.name == 'nt' else ''))
    build(runtime, exe)
    failed = 0
    code0, exact, _ = run(exe, 0, scale=o.scale)
    code1, trip, log = run(exe, 1, scale=o.scale)
    for a, b in zip(exact, trip):
        same = a == b
        failed += not same
        print(('same  ' if same else 'DIFF  ') + b + ('' if same else '\n      exact: ' + a))
    # the default (no switch): the same lines; on Windows the trip as with =1, elsewhere no trip at all
    coded, default, dlog = run(exe, None, scale=o.scale)
    dnotes = [l for l in dlog if l.startswith('[depth-trip]')]
    want = notes_of(log) if os.name == 'nt' else []
    ok = coded == 0 and default == exact and (dnotes[-1:] == want[-1:])
    failed += not ok
    print(('same  ' if ok else 'DIFF  ') + 'default (no PS2_DEPTH_TRIP): ' + (dnotes[-1] if dnotes else 'no trip') +
          ('' if ok else '\n      wanted: ' +(want[-1] if want else 'no trip') + ', exit %d, %d lines' % (coded, len(default))))
    if code0 or code1 or len(exact) != 7 or len(trip) != 7:
        print('a run failed: exit %d / %d, %d / %d lines' % (code0, code1, len(exact), len(trip)))
        failed += 1
    notes = [l for l in log if l.startswith('[depth-trip]')]
    for l in notes[-1:]:
        print(l)
    if not notes or ' 0 uploads put back' in notes[-1]:
        print('the trip was never taken')
        failed += 1
    # the depth sent back through VIF1 and GIF chains, in all three thread modes (issue #122: a deadlock and an uncaught round trip)
    vcode0, vexact, _ = run(exe, 0, ['vif'], scale=o.scale)
    vcode1, vtrip, _ = run(exe, 1, ['vif'], scale=o.scale)
    # memory and buffer hashes must be equal; with the trip on, every threaded mode puts the depth back from the copy, with no guard stop
    for a, b in zip(vexact, vtrip):
        if 'memory' not in b:
            ok = b.endswith(' 0 failed') and a.endswith(' 0 failed')
        else:
            ok = a.split('  put back')[0] == b.split('  put back')[0]
            if 'one thread' not in b:
                ok = ok and 'put back from the copy 1' in b and 'guard stops 0' in b
        ok = ok and not b.startswith('HANG')
        failed += not ok
        print(('same  ' if ok else 'DIFF  ') + 'vif: ' + b + ('' if ok else '\n      exact: ' + a))
    # the default (no switch): the =1 lines on Windows, the =0 lines elsewhere
    vcoded, vdefault, _ = run(exe, None, ['vif'], scale=o.scale)
    vwant = vtrip if os.name == 'nt' else vexact
    ok = vcoded == 0 and vdefault == vwant
    failed += not ok
    print(('same  ' if ok else 'DIFF  ') + 'vif, default (no PS2_DEPTH_TRIP): ' + (vdefault[-1] if vdefault else '(no output)'))
    if vcode0 or vcode1 or not vtrip or len(vexact) != len(vtrip):
        print('the vif run failed: exit %d / %d, %d / %d lines' % (vcode0, vcode1, len(vexact), len(vtrip)))
        failed += 1
    if os.name == 'nt':
        failed += crash_check(runtime, o.scale)
    if o.timing:
        for trip in (0, 1):
            _, out, _ = run(exe, trip, ['timing', str(o.timing)], scale=o.scale, log='0')
            print('PS2_DEPTH_TRIP=%d %s' % (trip, out[-1] if out else '(no output)'))
    print('depth trip: %d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
