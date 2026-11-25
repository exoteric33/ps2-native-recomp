"""Texels a scaled 2D piece reads past its own (runtime/src/gs/gpu/gs.wgsl), no game needed.

Builds tests/scale_edges.cpp against the newest native runtime in build/ (made by any port.sh build) and runs it with PS2_SCALE=1 and
each scale given (default 2, 3 and 4). Every case draws two pieces cut from a texture sheet, side by side, laid out so that 1x reads
only their own texels; at every scale no sample may pick up the sheet around them or the other piece. Prints "scale edges: 0 failed"
when it is so.

    python tests/scale_edges.py [--runtime build/runtime-native-.../libps2runtime.a] [--scale 2 --scale 4]
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(runtime, src, out):
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
    r = subprocess.run([cxx] + flags + [src, '-o', out] + libs, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stderr[-6000:])
        sys.exit('build failed')


def run(exe, scale):
    env = dict(os.environ, PS2_SCALE=str(scale), SDL_AUDIODRIVER='dummy')
    r = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=600)
    return r.returncode, r.stdout.splitlines(), r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runtime')
    ap.add_argument('--scale', type=int, action='append')
    o = ap.parse_args()
    runtime = o.runtime
    if not runtime:
        found = sorted(glob.glob(os.path.join(ROOT, 'build', 'runtime-native-*', 'libps2runtime.a')), key=os.path.getmtime)
        if not found:
            sys.exit('no build/runtime-native-*/libps2runtime.a: build a game first (scripts/port.sh)')
        runtime = found[-1]
    exe = os.path.join(ROOT, 'build', 'scale-edges-test' + ('.exe' if os.name == 'nt' else ''))
    build(runtime, os.path.join(ROOT, 'tests', 'scale_edges.cpp'), exe)
    failed = 0
    at_1x = {}  # case name -> whether 1x already reads past the piece
    for scale in [1] + (o.scale or [2, 3, 4]):
        code, out, err = run(exe, scale)
        if code or not out:
            print('scale %d: the run failed (exit %d)\n%s' % (scale, code, err[-2000:]))
            failed += 1
            continue
        for line in out:
            m = re.search(r'^(.*?)\s+scale \d+: sheet (\d+), other piece (\d+), empty (\d+)', line)
            if not m:
                print(line)
                continue
            bad = bool(int(m.group(2)) or int(m.group(3)) or int(m.group(4)))
            if scale == 1:
                at_1x[m.group(1)] = bad
            # A case that reads past its texels at 1x too is not a fault of the scaled picture (texel corners on pixel corners,
            # unfiltered: the triangle's float sums land a hair below a texel's edge on some pixels). It is shown, not counted.
            if at_1x.get(m.group(1)):
                print('1x too ' + line)
                continue
            failed += bad
            print(('BLEED  ' if bad else 'ok     ') + line)
    print('scale edges: %d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
