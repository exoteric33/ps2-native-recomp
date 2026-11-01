#!/usr/bin/env python3
"""Regression: a separately callable delay slot followed by untranslated bytes.

The compiler must dispatch an untranslated fallthrough instead of emitting a
goto to a label that does not exist. No game data is used by this fixture.
"""
from pathlib import Path
import argparse
import shutil
import struct
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--recompiler', type=Path, default=root / 'build/tools/eerecomp')
args = parser.parse_args()
# clang only, like scripts/cxx.sh: the runtime headers use clang builtins that GCC lacks
compiler = shutil.which('clang++')
if not compiler:
    raise SystemExit('clang++ required (the runtime headers use clang builtins; GCC cannot compile them)')
with tempfile.TemporaryDirectory(prefix='ps2-codegen-test-') as temp:
    work = Path(temp)
    data = bytearray(0x114)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', data, 16, 2, 8, 1, 0x100000, 52, 0, 0, 52, 32, 1, 40, 0, 0)
    struct.pack_into('<8I', data, 52, 1, 0x100, 0x100000, 0, 20, 20, 5, 4)
    # beq zero,zero,target; nop; invalid; invalid; target: break
    struct.pack_into('<5I', data, 0x100, 0x10000003, 0, 1, 1, 0x0d)
    (work / 'input.elf').write_bytes(data)
    (work / 'entries.txt').write_text('100004\n')
    result = subprocess.run([str(args.recompiler.resolve()), str(work / 'input.elf'), '--entries',
                    str(work / 'entries.txt'), '--out', str(work / 'generated')], check=True, capture_output=True, text=True)
    assert 'code 3 instructions' in result.stdout, result.stdout
    subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-I' + str(root / 'runtime/include'),
                    '-I' + str(work / 'generated'), *map(str, (work / 'generated').glob('*.cpp'))], check=True)
print('Untranslated fallthrough regression passed')
