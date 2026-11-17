"""The depth round trip (runtime/src/gs/gs.cpp), no game needed.

Builds tests/depth_trip.cpp against the newest native runtime in build/ (made by any port.sh build), runs it with PS2_DEPTH_TRIP=0
(every depth download waits: the exact path) and =1, and wants the same line for every case; with =1 the trip log must show uploads
put back from the copy. Then measures the trip case on both paths (--timing). Prints "depth trip: 0 failed" when all is well.

    python tests/depth_trip.py [--runtime build/runtime-native-.../libps2runtime.a] [--timing ROUNDS] [--scale N]
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(runtime, out):
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
    cmd = [cxx] + flags + [os.path.join(ROOT, 'tests', 'depth_trip.cpp'), '-o', out] + libs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit('build failed')


def run(exe, trip, args=(), scale=None, log='2'):
    env = dict(os.environ, PS2_DEPTH_TRIP=str(trip), PS2_DEPTH_TRIP_LOG=log)
    if scale:
        env['PS2_SCALE'] = str(scale)
    r = subprocess.run([exe] + list(args), capture_output=True, text=True, env=env, timeout=600)
    return r.returncode, r.stdout.splitlines(), r.stderr.splitlines()


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
    if code0 or code1 or len(exact) != 7 or len(trip) != 7:
        print('a run failed: exit %d / %d, %d / %d lines' % (code0, code1, len(exact), len(trip)))
        failed += 1
    notes = [l for l in log if l.startswith('[depth-trip]')]
    for l in notes[-1:]:
        print(l)
    if not notes or ' 0 uploads put back' in notes[-1]:
        print('the trip was never taken')
        failed += 1
    if o.timing:
        for trip in (0, 1):
            _, out, _ = run(exe, trip, ['timing', str(o.timing)], scale=o.scale, log='0')
            print('PS2_DEPTH_TRIP=%d %s' % (trip, out[-1] if out else '(no output)'))
    print('depth trip: %d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
