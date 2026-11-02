#!/usr/bin/env python3
"""Boot and live-translate a synthetic game from ISO after removing its ELF.

Build eerecomp and ps2runtime, then run:
  python3 tests/disc_boot.py --recompiler build/tools/eerecomp --runtime build/ci/libps2runtime.a
No game data or GPU is needed. All generated files live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, expected=0, **kwargs):
    result = subprocess.run([str(arg) for arg in command], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180, **kwargs)
    if result.returncode != expected:
        raise RuntimeError(f"{command[0]} exited {result.returncode}, wanted {expected}:\n{result.stdout[-8000:]}")
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--recompiler', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='disc boot test ') as temporary:
        work = Path(temporary)
        elf = work / 'BOOT.ELF'
        # Store a waiting loop in a zero-file-size code window, then jump into it.
        # Only live EE compilation can serve that window; analysis must also work without a host ELF path.
        code = [0x3c080020, 0x3c091000, 0x3529ffff, 0xad090000, 0x24090000, 0xad090004, 0x01000008, 0]
        data = bytearray(0x100 + 4 * len(code))
        data[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, 0x100000, 52, 0, 0, 52, 32, 2, 40, 0, 0)
        struct.pack_into('<8I', data, 52, 1, 0x100, 0x100000, 0, 4 * len(code), 4 * len(code), 5, 4)
        struct.pack_into('<8I', data, 84, 1, 0, 0x200000, 0, 0, 0x1000, 7, 4)
        struct.pack_into('<' + 'I' * len(code), data, 0x100, *code)
        elf.write_bytes(data)
        iso = work / 'disc image.iso'
        run([sys.executable, ROOT / 'scripts/mkiso.py', iso, elf])
        generated = work / 'generated'
        run([args.recompiler.resolve(), elf, '--out', generated])
        build = work / 'build'
        run(['cmake', '-S', ROOT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_CXX_COMPILER={args.cxx}', f'-DPS2_GENERATED={generated}',
             f'-DPS2_RUNTIME_LIB={args.runtime.resolve()}'])
        run(['ninja', '-C', build, '-j2', 'ps2run'])
        exe = build / ('ps2run.exe' if os.name == 'nt' else 'ps2run')
        cwd = work / 'empty run folder'
        cwd.mkdir()
        env = os.environ.copy()
        env.update(PS2_GS='cpu', PS2_VU_LIVE='0', PS2_EE_LIVE_BG='0', PS2_EE_LIVE_QUICK='off')
        env['PS2_EE_LIVE'] = str(work / 'external-cache')
        external = run([exe, elf, '--disc', iso, '--frames', '30'], cwd=cwd, env=env)
        assert 'stopped after 30 frames' in external, external
        # The linked runner contains translated code, but no copy of the ELF and no remaining host file.
        elf.unlink()
        env['PS2_EE_LIVE'] = str(work / 'disc-cache')
        recording = work / 'run.inputs'
        disc = run([exe, '--disc', iso, '--frames', '30', '--record', recording], cwd=cwd, env=env)
        assert '[elf] read cdrom0:\\BOOT.ELF;1 from disc (288 bytes)' in disc, disc
        assert 'stopped after 30 frames' in disc and 'switched in after' in disc, disc
        assert 'disc-program 1\n' in recording.read_text(), recording.read_text()
        assert not elf.exists() and not list(cwd.glob('*.ELF')), 'runner extracted a boot ELF'
        cached = run([exe, '--disc', iso, '--frames', '30'], cwd=cwd, env=env)
        assert 'stopped after 30 frames' in cached and 'loaded 1 translated code windows' in cached, cached
        assert 'switched in after' not in cached, cached
        env['PS2_EE_LIVE'] = str(work / 'external-cache')
        shared = run([exe, '--disc', iso, '--frames', '30'], cwd=cwd, env=env)
        assert 'loaded 1 translated code windows' in shared and 'switched in after' not in shared, shared
        replay = run([sys.executable, ROOT / 'scripts/replay.py', recording, '--exe', exe, '--disc', iso], cwd=cwd, env=env)
        assert 'RESULT: same to the end' in replay, replay
        run([exe, '--frames', '30'], expected=2, cwd=cwd, env=env)
    print('ISO boot: legacy ELF, absent-ELF boot, live analysis, warm/shared cache and replay passed')


if __name__ == '__main__':
    main()
