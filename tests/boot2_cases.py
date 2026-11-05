"""No game data: SYSTEM.CNF files written in odd but valid ways must name the same boot program.

Writes made-up discs with scripts/mkiso.py (tabs, lower case, a leading space, CR alone as line end, a byte order mark, no
folder separator, a name that tries a wildcard) and runs scripts/port.sh up to its disc step (PS2_PORT_STOP=disc) and
scripts/pnach.py's reader on each. Run from the repository root: python3 tests/boot2_cases.py (prints "boot2 cases: 0 failed").
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BOM = b'\xef\xbb\xbf'
# (label, SYSTEM.CNF bytes with %s for the program name, should the program be found)
CASES = [
    ('plain', b'BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\n', True),
    ('no spaces', b'BOOT2=cdrom0:\\%s;1\r\n', True),
    ('tabs', b'BOOT2\t=\tcdrom0:\\%s;1\r\nVER\t= 1.00\r\n', True),
    ('lower case', b'boot2 = cdrom0:\\%s;1\r\n', True),
    ('mixed case', b'Boot2 = CDROM0:\\%s;1\n', True),
    ('leading space', b'  BOOT2 = cdrom0:\\%s;1\r\n', True),
    ('leading tab', b'\tBOOT2 = cdrom0:\\%s;1\n', True),
    ('second line', b'VER = 1.00\r\nBOOT2 = cdrom0:\\%s;1\r\n', True),
    ('CR alone', b'BOOT2 = cdrom0:\\%s;1\rVER = 1.00\rVMODE = NTSC\r', True),
    ('CR alone, second line', b'VER = 1.00\rBOOT2 = cdrom0:\\%s;1\r', True),
    ('byte order mark', BOM + b'BOOT2 = cdrom0:\\%s;1\r\n', True),
    ('byte order mark, lower, tabs', BOM + b'boot2\t=\tcdrom0:\\%s;1\r\n', True),
    ('no version suffix', b'BOOT2 = cdrom0:\\%s\r\n', True),
    ('trailing spaces', b'BOOT2 = cdrom0:\\%s;1   \r\n', True),
    ('last line, no line end', b'VER = 1.00\nBOOT2 = cdrom0:\\%s;1', True),
    ('forward slash', b'BOOT2 = cdrom0:/%s;1\r\n', True),
    ('no BOOT2 line', b'VER = 1.00\r\nBOOT20 = x\r\n', False),
    ('empty value', b'BOOT2 =\r\nVER = 1.00\r\n', False),
    ('wildcard', b'BOOT2 = cdrom0:\\*;1\r\n', False),
    ('folder', b'BOOT2 = cdrom0:\\SUB\\%s;1\r\n', False),
]
failed = 0
checks = 0


def check(ok, message):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print('FAIL: ' + message, file=sys.stderr)


def main():
    elf = os.path.join(ROOT, 'isos', 'autotests', 'cpu-ee-alu', 'TEST.ELF')
    work = tempfile.mkdtemp(prefix='boot2-cases-')
    if not os.path.isfile(elf):
        elf = os.path.join(work, 'TEST.ELF')
        with open(elf, 'wb') as f:
            f.write(b'\x7fELF' + bytes(60))
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    import mkiso
    spec = importlib.util.spec_from_file_location('pnach', os.path.join(ROOT, 'scripts', 'pnach.py'))
    pnach = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(pnach)
    env = dict(os.environ, PS2_PORT_STOP='disc')
    game = 'boot2-case'
    try:
        for label, cnf, good in CASES:
            iso = os.path.join(work, 'case.iso')
            with open(iso, 'wb') as f:
                f.write(mkiso.build(elf, [], cnf))
            shutil.rmtree(os.path.join(ROOT, 'games', game), ignore_errors=True)
            run = subprocess.run(['bash', os.path.join(ROOT, 'scripts', 'port.sh'), game, iso, '--native-only'],
                                 capture_output=True, text=True, env=env)
            boots = [l for l in run.stdout.splitlines() if l.startswith('boots ')]
            check((run.returncode == 0 and bool(boots)) == good, '%s: port.sh %s (%s)' % (
                label, 'stopped' if good else 'went on', (run.stderr.strip().splitlines() or ['no message'])[-1][:100]))
            if good and boots:
                check(os.path.isfile(os.path.join(ROOT, 'games', game, 'disc', 'TEST.ELF')), label + ': program not extracted')
            try:
                found = pnach.read_program(iso)[1]
            except pnach.Problem as problem:
                found = None
                if good and label != 'folder':
                    check(False, '%s: pnach.py failed: %s' % (label, str(problem)[:100]))
            if good:
                check(found == 'TEST.ELF', '%s: pnach.py read %r' % (label, found))
    finally:
        shutil.rmtree(os.path.join(ROOT, 'games', game), ignore_errors=True)
        shutil.rmtree(os.path.join(ROOT, 'generated', game), ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)
    print('boot2 cases: %d failed (%d checks)' % (failed, checks))
    sys.exit(1 if failed else 0)


main()
