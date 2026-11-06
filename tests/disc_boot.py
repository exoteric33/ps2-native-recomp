#!/usr/bin/env python3
"""Boot and live-translate a synthetic game from ISO after removing its ELF.

Build eerecomp and ps2runtime, then run:
  python3 tests/disc_boot.py --recompiler build/tools/eerecomp --runtime build/ci/libps2runtime.a
No game data or GPU is needed. All generated files live in a temporary directory.
"""
import argparse
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, expected=0, **kwargs):
    result = subprocess.run([str(arg) for arg in command], text=True, encoding="utf-8", errors="replace", stdout=subprocess.PIPE,
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
        assert 'disc-program 1\n' in recording.read_text(encoding="utf-8"), recording.read_text(encoding="utf-8")
        assert not elf.exists() and not list(cwd.glob('*.ELF')), 'runner extracted a boot ELF'
        cached = run([exe, '--disc', iso, '--frames', '30'], cwd=cwd, env=env)
        assert 'stopped after 30 frames' in cached and 'loaded 1 translated code windows' in cached, cached
        assert 'switched in after' not in cached, cached
        env['PS2_EE_LIVE'] = str(work / 'external-cache')
        shared = run([exe, '--disc', iso, '--frames', '30'], cwd=cwd, env=env)
        assert 'loaded 1 translated code windows' in shared and 'switched in after' not in shared, shared
        replay = run([sys.executable, ROOT / 'scripts/replay.py', recording, '--exe', exe, '--disc', iso], cwd=cwd, env=env)
        assert 'RESULT: same to the end' in replay, replay
        # A portable runner and an image outside the game folder must still use the game's caches and build inputs.
        game = work / 'game folder ü'
        portable = work / 'portable' / exe.name
        portable.parent.mkdir()
        shutil.copy2(exe, portable)
        for dll in exe.parent.glob('*.dll'):  # Windows: the runner needs SDL2.dll (and the shader compiler DLLs) beside it
            shutil.copy2(dll, portable.parent)
        (game / 'vu1').mkdir(parents=True)
        (game / 'overlays').mkdir()
        (game / 'entries.txt').write_text('')
        env.update(PS2_EE_LIVE='1', PS2_VU_LIVE='1')
        game_recording = work / 'game-dir.inputs'
        selected = run([portable, '--disc', iso, '--game-dir', game, '--frames', '30', '--record', game_recording], cwd=cwd, env=env)
        assert 'stopped after 30 frames' in selected and 'switched in after' in selected, selected
        assert str(game / 'ee-live') in selected and str(game / 'vu1-live') in selected, selected
        assert list((game / 'ee-live').glob('*.*')), 'EE cache was not written in the game folder'
        assert (game / 'entries.txt').read_text(encoding="utf-8").strip(), 'live entries were not added to the game folder'
        assert list((game / 'overlays').glob('*.bin')), 'live overlay was not captured in the game folder'
        assert f'game-dir {game}\n' in game_recording.read_text(encoding="utf-8"), game_recording.read_text(encoding="utf-8")
        selected_replay = run([sys.executable, ROOT / 'scripts/replay.py', game_recording, '--exe', portable,
                              '--disc', iso, '--log', work / 'game-replay.log'], cwd=cwd, env=env)
        assert 'RESULT: same to the end' in selected_replay, selected_replay
        assert 'loaded 1 translated code windows' in (work / 'game-replay.log').read_text(encoding="utf-8"), selected_replay
        legacy = cwd / 'separate program.elf'
        legacy.write_bytes(data)
        legacy_run = run([portable, legacy, '--disc', iso, '--game-dir', game, '--frames', '30'], cwd=cwd, env=env)
        assert 'loaded 1 translated code windows' in legacy_run and 'switched in after' not in legacy_run, legacy_run
        legacy.unlink()
        env.update(PS2_EE_LIVE=str(work / 'external-cache'), PS2_VU_LIVE=str(work / 'override-vu-cache'))
        overrides = run([portable, '--disc', iso, '--game-dir', game, '--frames', '30'], cwd=cwd, env=env)
        assert str(work / 'external-cache') in overrides and str(work / 'override-vu-cache') in overrides, overrides
        assert not (work / 'ee-live').exists() and not (work / 'vu1-live').exists(), 'cache leaked into the ISO folder'
        assert not (cwd / 'ee-live').exists() and not (cwd / 'vu1-live').exists(), 'cache leaked into the working folder'
        invalid = run([exe, '--disc', iso, '--game-dir', work / 'missing', '--frames', '30'], expected=1, cwd=cwd, env=env)
        assert 'not a directory' in invalid, invalid
        run([exe, '--disc', iso, '--game-dir', iso, '--frames', '30'], expected=1, cwd=cwd, env=env)
        run([exe, '--frames', '30'], expected=2, cwd=cwd, env=env)
    print('ISO boot: legacy ELF, absent-ELF boot, live analysis, warm/shared cache, game directory and replay passed')


if __name__ == '__main__':
    main()
