"""No game data: scripts/disc.py on damaged or crafted ISO 9660 images.

A name like ../X or /X must not be written outside the destination folder, a truncated image must stop with an error instead of
hanging, and directories that point back at a parent must not be walked for ever. Run from the repository root:
python tests/disc_py_input.py (prints "disc.py input: 0 failed").
"""
import os
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048
HERE = os.path.dirname(os.path.abspath(__file__))
DISC_PY = os.path.join(HERE, '..', 'scripts', 'disc.py')
failed = 0
checks = 0


def check(ok, message):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print('FAIL: ' + message, file=sys.stderr)


def record(lba, size, directory, name):
    # ISO 9660 (ECMA-119 9.1): length, extended attribute length, extent (both byte orders), size (both), date, flags, ..., name
    name = name.encode('latin-1') if isinstance(name, str) else name
    length = (33 + len(name) + 1) & ~1
    rec = bytearray(length)
    rec[0] = length
    struct.pack_into('<I', rec, 2, lba)
    struct.pack_into('>I', rec, 6, lba)
    struct.pack_into('<I', rec, 10, size)
    struct.pack_into('>I', rec, 14, size)
    rec[25] = 2 if directory else 0
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def image(path, root_records, sectors=40, extra=None):
    img = bytearray(sectors * SECTOR)
    pvd = 16 * SECTOR
    img[pvd:pvd + 7] = b'\x01CD001\x01'
    img[pvd + 40:pvd + 72] = b'TEST'.ljust(32)
    struct.pack_into('<I', img, pvd + 80, sectors)
    root = record(20, SECTOR, True, b'\x00')
    img[pvd + 156:pvd + 156 + len(root)] = root
    data = record(20, SECTOR, True, b'\x00') + record(20, SECTOR, True, b'\x01') + b''.join(root_records)
    img[20 * SECTOR:20 * SECTOR + len(data)] = data
    for sector, content in (extra or {}).items():
        img[sector * SECTOR:sector * SECTOR + len(content)] = content
    with open(path, 'wb') as f:
        f.write(img)


def disc_py(*args, timeout=30):
    try:
        run = subprocess.run([sys.executable, '-I', DISC_PY] + list(args), capture_output=True, text=True, timeout=timeout)
        return run.returncode, run.stdout + run.stderr
    except subprocess.TimeoutExpired:
        return None, 'timed out'


def main():
    with tempfile.TemporaryDirectory() as tmp:
        # names that leave the folder: ../, a leading /, a backslash, a drive colon; and one good file
        iso = os.path.join(tmp, 'names.iso')
        image(iso, [record(30, 5, False, '../UP.BIN;1'), record(30, 5, False, '/ABS.BIN;1'), record(30, 5, False, '..\\BS.BIN;1'),
                    record(30, 5, False, 'C:DRIVE.BIN;1'), record(30, 5, False, '..'), record(31, 5, False, 'OK.BIN;1')],
              extra={30: b'EVIL!', 31: b'GOOD!'})
        target = os.path.join(tmp, 'out', 'disc')
        os.makedirs(target)
        code, text = disc_py('extract', iso, target)
        check(code == 0, 'crafted names: extract still works (%s)' % text.strip()[-200:])
        check(os.listdir(target) == ['OK.BIN'], 'crafted names: only the good file is in the folder (%s)' % os.listdir(target))
        check(sorted(os.listdir(tmp)) == ['names.iso', 'out'] and os.listdir(os.path.join(tmp, 'out')) == ['disc'],
              'crafted names: nothing was written outside the folder')
        check(not os.path.exists(os.path.join(os.path.splitdrive(tmp)[0] + os.sep, 'ABS.BIN')), 'crafted names: nothing at the drive root')
        with open(os.path.join(target, 'OK.BIN'), 'rb') as f:
            check(f.read() == b'GOOD!', 'crafted names: the good file has its contents')

        # a file that runs past the end of the image (an interrupted rip): an error, not a hang
        iso = os.path.join(tmp, 'truncated.iso')
        image(iso, [record(30, 200000, False, 'BIG.BIN;1')], extra={30: b'x' * SECTOR})
        out = os.path.join(tmp, 'trunc-out')
        code, text = disc_py('extract', iso, out, 'BIG.BIN', timeout=20)
        check(code not in (0, None) and 'ends early' in text, 'truncated image: stops with an error (%s, %s)' % (code, text.strip()[-200:]))
        check(not os.path.exists(os.path.join(out, 'BIG.BIN')), 'truncated image: no half file is left behind')

        # a directory whose extent lies past the end of the image
        iso = os.path.join(tmp, 'truncated-dir.iso')
        image(iso, [record(5000, SECTOR, True, 'GONE')])
        code, text = disc_py('list', iso, timeout=20)
        check(code not in (0, None) and 'ends early' in text, 'directory past the end: stops with an error (%s)' % text.strip()[-200:])

        # 40 directories that all point back at the root: used to recurse until Python gave up
        iso = os.path.join(tmp, 'loop.iso')
        image(iso, [record(20, SECTOR, True, 'D%02d' % i) for i in range(40)] + [record(31, 5, False, 'OK.BIN;1')], extra={31: b'GOOD!'})
        code, text = disc_py('list', iso, timeout=20)
        check(code == 0 and 'OK.BIN' in text, 'looping directories: the listing ends (%s, %s)' % (code, text.strip()[-200:]))

        # a record whose length runs past the end of its directory
        iso = os.path.join(tmp, 'long-record.iso')
        # small records fill the sector (a zero length would skip to the next sector), then one says 200 bytes near the end
        fill = [record(31, 5, False, 'F')] * ((SECTOR - 120 - 110) // 34 + 1)
        image(iso, [record(31, 5, False, 'OK.BIN;1')] + fill, extra={31: b'GOOD!'})
        with open(iso, 'r+b') as f:
            f.seek(20 * SECTOR + 110 + 34 * len(fill))
            f.write(bytes([200]))
        code, text = disc_py('list', iso, timeout=20)
        check(code == 0 and 'OK.BIN' in text and 'damaged directory' in text, 'record past the end: warned, the rest listed (%s)' % text.strip()[-200:])

    print('disc.py input: %d failed of %d checks' % (failed, checks))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
