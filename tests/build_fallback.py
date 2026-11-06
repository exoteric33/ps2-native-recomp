#!/usr/bin/env python3
"""Regression for public issue 50: the build survives a generated unit that makes the compiler crash.

scripts/build_units.sh runs ninja; when a unit crashes the compiler it builds that one unit again, a step lower (same flags once,
-O1, -O0, -O0 -mno-avx2), and cmake/lowopt.cmake gives the flags to that file only. No game data is used: the "units" are tiny
files that crash the real clang on purpose with `#pragma clang __debug crash` when a macro says so (__OPTIMIZE__ is set from -O1
up, __AVX2__ with -mavx2), plus a launcher that fakes one crash that goes away at the second try.

    python tests/build_fallback.py                       # the clang++ in PATH (scripts/cxx.sh style)
    PS2_TEST_CXX=/path/to/clang++ python tests/build_fallback.py   # another compiler, e.g. a wrapper that runs the Windows clang++.exe under Wine

Needs bash, cmake, ninja and clang++. Exit 0 when everything passes.
"""
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
cxx = os.environ.get('PS2_TEST_CXX') or shutil.which('clang++')
bash = shutil.which('bash')
for tool, found in (('clang++', cxx), ('cmake', shutil.which('cmake')), ('ninja', shutil.which('ninja')), ('bash', bash)):
    if not found:
        raise SystemExit('%s is needed for this test' % tool)

x86 = platform.machine().lower() in ('x86_64', 'amd64')
checks = []


def check(name, ok, detail=''):
    checks.append(ok)
    print('%s  %s%s' % ('PASS' if ok else 'FAIL', name, (' (%s)' % detail) if detail and not ok else ''))


def posix(p):
    """Paths for bash and cmake: forward slashes (Git Bash on Windows takes C:/... too)."""
    return str(p).replace('\\', '/')


with tempfile.TemporaryDirectory(prefix='ps2-build-fallback-') as temp:
    work = Path(temp)
    gen = work / 'generated'
    gen.mkdir()
    ok_unit = 'namespace gen { int %s() { return %d; } }\n'
    crash_if = '#if %s\n#pragma clang __debug crash\n#endif\n'
    units = {
        'unit_0001.cpp': ok_unit % ('u1', 1),                                   # fine
        'unit_0002.cpp': crash_if % 'defined(__OPTIMIZE__)' + ok_unit % ('u2', 2),   # crashes at -O3 and -O1, fine at -O0
        'unit_0003.cpp': crash_if % 'defined(__AVX2__)' + ok_unit % ('u3', 3),       # crashes while -mavx2 is on
        'unit_0004.cpp': ok_unit % ('u4', 4),                                   # fine, the launcher fakes one crash
        'unit_0005.cpp': ok_unit % ('u5', 5),                                   # fine
    }
    for name, text in units.items():
        (gen / name).write_text(text)
    (work / 'main.cpp').write_text('namespace gen { int u1(); int u2(); int u3(); int u4(); int u5(); }\n'
                                   'int main() { return gen::u1() + gen::u2() + gen::u3() + gen::u4() + gen::u5() == 15 ? 0 : 1; }\n')
    (work / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.20)
project(fallback_test CXX)
file(GLOB UNITS CONFIGURE_DEPENDS "${GEN}/*.cpp")
add_library(ps2game STATIC ${UNITS})
include("${LOWOPT}")
ps2_lowopt_apply(ps2game)
add_executable(ps2run main.cpp)
target_link_libraries(ps2run PRIVATE ps2game)
''')
    # a launcher in front of the compiler: the first compile of unit_0004.cpp "crashes" like clang does, the second one works
    launcher = work / 'launcher.sh'
    with open(launcher, 'w', newline='\n') as handle:   # LF only: bash fails on a CRLF script
        handle.write('''#!/bin/sh
case "$*" in
  *unit_0004.cpp*)
    if [ ! -f "$STATE_DIR/unit4-crashed-once" ]; then
      : > "$STATE_DIR/unit4-crashed-once"
      echo "PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/ and include the crash backtrace." >&2
      echo "Stack dump:" >&2
      echo "clang++: error: clang frontend command failed due to signal (use -v to see invocation)" >&2
      exit 1
    fi ;;
esac
exec "$@"
''')
    env = dict(os.environ, STATE_DIR=posix(work), CLANG_CRASH_DIAGNOSTICS_DIR=posix(work), PS2_JOBS='2')
    env.pop('PS2_LOWOPT', None)

    def configure(build):
        flags = ['-DCMAKE_BUILD_TYPE=Release', '-DGEN=' + posix(gen), '-DLOWOPT=' + posix(root / 'cmake/lowopt.cmake'),
                 '-DCMAKE_CXX_COMPILER=' + posix(cxx), '-DCMAKE_CXX_COMPILER_LAUNCHER=' + posix(bash) + ';' + posix(launcher)]
        if x86:
            flags.append('-DCMAKE_CXX_FLAGS=-mavx2')
        subprocess.run(['cmake', '-S', posix(work), '-B', posix(build), '-G', 'Ninja', *flags], check=True, capture_output=True, env=env)

    def run_ninja(build, extra_env=None):
        """ps2_ninja the way port.sh calls it; returns (exit code, output)."""
        script = ('set -u; . "%s/scripts/build_units.sh"; py=("%s"); ninja_jobs="-j2"; ps2_ninja "%s" ps2run' %
                  (posix(root), posix(sys.executable), posix(build)))
        e = dict(env, **(extra_env or {}))
        r = subprocess.run([bash, '-c', script], capture_output=True, text=True, env=e)
        return r.returncode, r.stdout + r.stderr

    def commands(build):
        out = subprocess.run(['ninja', '-C', posix(build), '-t', 'commands', 'ps2run'], capture_output=True, text=True, env=env).stdout
        return {name: next((l for l in out.splitlines() if name + '.o' in l.split(' -o ')[-1] or name + '.obj' in l.split(' -o ')[-1]), '')
                for name in ('unit_0001.cpp', 'unit_0002.cpp', 'unit_0003.cpp', 'unit_0004.cpp', 'unit_0005.cpp')}

    # 1. the whole ladder
    build = work / 'build'
    configure(build)
    code, out = run_ninja(build)
    lowopt = (build / 'lowopt.txt').read_text() if (build / 'lowopt.txt').exists() else ''
    check('the build finishes although units crash the compiler', code == 0, out[-600:])
    check('the program is made and runs', (build / 'ps2run').exists() or (build / 'ps2run.exe').exists())
    cmds = commands(build)
    check('unit_0002 ends at -O0 without -mno-avx2', 'unit_0002.cpp -O0\n' in lowopt and '-mno-avx2' not in lowopt.split('unit_0002.cpp')[1].split('\n')[0], lowopt)
    if x86:
        check('unit_0003 (crashes with -mavx2) ends at -O0 -mno-avx2', 'unit_0003.cpp -O0 -mno-avx2\n' in lowopt, lowopt)
    check('unit_0004 (one crash that goes away) is not lowered', 'unit_0004.cpp' not in ''.join(l for l in lowopt.splitlines() if not l.startswith('#')), lowopt)
    check('unit_0001 and unit_0005 are never mentioned', 'unit_0001' not in lowopt and 'unit_0005' not in lowopt, lowopt)
    base = cmds['unit_0001.cpp']
    check('a unit that compiles fine keeps exactly the usual flags',
          cmds['unit_0005.cpp'].replace('unit_0005', 'unit_0001') == base and '-O3' in base and base.count('-O') == 1, base)
    c2 = cmds['unit_0002.cpp']
    check('the lowered unit gets its flags behind the usual ones', '-O3' in c2 and '-O0' in c2 and c2.index('-O3') < c2.index('-O0'), c2)
    check('the failed attempts are kept as logs', (build / 'ninja-failed-1.log').exists())
    check('the output names the unit and the step', 'unit_0002.cpp crashed the compiler; building it with -O1' in out and '-O0' in out, out[-800:])
    check('the output keeps the exception/stack lines the old tail -30 cut off', 'Stack dump' in out or 'PLEASE submit' in out, out[:600])

    # 2. a second run changes nothing and builds nothing
    code2, out2 = run_ninja(build)
    check('a second run is quiet and succeeds', code2 == 0 and 'crashed' not in out2, out2[-300:])

    # 3. a unit that crashes at every setting: the build stops, with the reason
    (gen / 'unit_0006.cpp').write_text('#pragma clang __debug crash\nnamespace gen { int u6() { return 6; } }\n')
    build2 = work / 'build2'
    configure(build2)
    code3, out3 = run_ninja(build2)
    check('a unit that always crashes stops the build in the end', code3 != 0, out3[-300:])
    check('... and says so', 'unit_0006.cpp crashes the compiler even at its lowest setting' in out3, out3[-500:])
    (gen / 'unit_0006.cpp').unlink()

    # 4. PS2_LOWOPT=0 keeps the old behaviour: stop at the first crash
    (build / 'lowopt.txt').write_text('')
    build4 = work / 'build4'
    configure(build4)
    code5, out5 = run_ninja(build4, {'PS2_LOWOPT': '0'})
    check('PS2_LOWOPT=0 stops at the first crash and changes no list', code5 != 0 and (build4 / 'lowopt.txt').read_text() == '', out5[-300:])

    # 5. a real error in the generated code is not a crash: no retry, the build stops at once
    for name in ('unit_0002.cpp', 'unit_0003.cpp'):   # only the real error is left
        (gen / name).unlink()
    (gen / 'unit_0007.cpp').write_text('namespace gen { int u7() { return missing_name; } }\n')
    build3 = work / 'build3'
    configure(build3)
    code4, out4 = run_ninja(build3)
    lowopt3 = (build3 / 'lowopt.txt').read_text()
    check('a real compile error stops the build', code4 != 0)
    check('... without any retry or lowered unit', 'crashed the compiler' not in out4 and 'unit_0007' not in lowopt3, out4[-300:] + lowopt3)
    (gen / 'unit_0007.cpp').unlink()

print('%d of %d checks passed' % (sum(checks), len(checks)))
sys.exit(0 if all(checks) else 1)
