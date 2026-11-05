"""No game data: scripts/disc.py turns a CD dumped as BIN and CUE (2352-byte sectors) into a plain .iso.

A small disc is made with scripts/mkiso.py, written out again as raw CD sectors (mode 1 and mode 2 form 1, the two kinds a PS2 CD
dump has), and `disc.py iso` must give back exactly the bytes of the original. Also checked: a cue sheet with and without quotes,
a .zip holding the .bin, a dump with an audio track after the data track, a plain 2048-byte image given as .bin, and the errors
(no file named by the cue sheet, a zip with no image in it). Run from the repository root:
python tests/disc_bincue.py (prints "disc bin/cue: 0 failed").
"""
import os
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DISC_PY = os.path.join(HERE, '..', 'scripts', 'disc.py')
MKISO = os.path.join(HERE, '..', 'scripts', 'mkiso.py')
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'
failed = 0
checks = 0


def check(ok, message):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print('FAIL: ' + message, file=sys.stderr)


def raw_sectors(iso, mode, first=0):
    """The image as a CD dump: every 2048-byte sector in a 2352-byte one (the error codes are left zero, nobody reads them)."""
    out = bytearray()
    for n in range(len(iso) // 2048):
        minute, second, frame = divmod(n + first + 150, 75 * 60)[0], divmod(divmod(n + first + 150, 75)[0], 60)[1], (n + first + 150) % 75
        header = SYNC + bytes([minute, second, frame, mode])
        data = iso[n * 2048:(n + 1) * 2048]
        if mode == 1:
            out += header + data + bytes(288)
        else:
            out += header + bytes([0, 0, 8, 0, 0, 0, 8, 0]) + data + bytes(280)
    return bytes(out)


def run(*args):
    return subprocess.run([sys.executable, '-I', DISC_PY] + list(args), capture_output=True, text=True)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        boot = os.path.join(tmp, 'GAME.ELF')
        with open(boot, 'wb') as f:
            f.write(b'\x7fELF' + bytes(4000))
        iso_path = os.path.join(tmp, 'disc.iso')
        made = subprocess.run([sys.executable, '-I', MKISO, iso_path, 'GAME.ELF'], capture_output=True, text=True, cwd=tmp)
        check(made.returncode == 0 and os.path.exists(iso_path), 'mkiso.py made the test disc: ' + made.stderr)
        iso = open(iso_path, 'rb').read()
        check(len(iso) % 2048 == 0 and len(iso) > 20 * 2048, 'the test disc has whole sectors')

        for mode in (1, 2):
            folder = os.path.join(tmp, 'mode%d' % mode)
            os.mkdir(folder)
            with open(os.path.join(folder, 'My Game.bin'), 'wb') as f:
                f.write(raw_sectors(iso, mode))
            for style, line in (('quoted', 'FILE "My Game.bin" BINARY'), ('plain', 'FILE My_Game.bin BINARY')):
                name = 'My Game.bin' if style == 'quoted' else 'My_Game.bin'
                if style == 'plain':
                    os.replace(os.path.join(folder, 'My Game.bin'), os.path.join(folder, name))
                cue = os.path.join(folder, style + '.cue')
                with open(cue, 'w') as f:
                    f.write(line + '\r\n  TRACK 01 MODE%d/2352\r\n    INDEX 01 00:00:00\r\n' % (1 if mode == 1 else 2))
                target = os.path.join(folder, style + '.iso')
                done = run('iso', cue, target)
                check(done.returncode == 0, 'mode %d, %s cue sheet: disc.py failed: %s' % (mode, style, done.stderr.strip()))
                check(os.path.exists(target) and open(target, 'rb').read() == iso, 'mode %d, %s cue sheet: the .iso is the original' % (mode, style))
                check(not os.path.exists(target + '.part'), 'mode %d: no .part file is left' % mode)
            # the .bin named directly, without the cue sheet
            direct = os.path.join(folder, 'direct.iso')
            done = run('iso', os.path.join(folder, 'My_Game.bin'), direct)
            check(done.returncode == 0 and open(direct, 'rb').read() == iso, 'mode %d: the .bin alone gives the original' % mode)

            # the same .bin inside a .zip (together with a cue sheet and a note, which are not images)
            archive = os.path.join(folder, 'dump.zip')
            with zipfile.ZipFile(archive, 'w') as z:
                z.writestr('readme.txt', 'x')
                z.write(os.path.join(folder, 'My_Game.bin'), 'My_Game.bin')
            zipped = os.path.join(folder, 'zipped.iso')
            done = run('iso', archive, zipped)
            check(done.returncode == 0 and open(zipped, 'rb').read() == iso, 'mode %d: a .zip holding the .bin gives the original' % mode)

        # an audio track after the data track in the same .bin: the copy ends where the sync pattern ends
        mixed = os.path.join(tmp, 'mixed.bin')
        with open(mixed, 'wb') as f:
            f.write(raw_sectors(iso, 2) + bytes(range(256)) * 2352)
        out = os.path.join(tmp, 'mixed.iso')
        done = run('iso', mixed, out)
        check(done.returncode == 0 and open(out, 'rb').read() == iso, 'a dump with an audio track after the data keeps the data track only')

        # a plain image under the name .bin / a .zip with an .iso: copied as it is
        plain = os.path.join(tmp, 'plain.bin')
        with open(plain, 'wb') as f:
            f.write(iso)
        out = os.path.join(tmp, 'plain.iso')
        done = run('iso', plain, out)
        check(done.returncode == 0 and open(out, 'rb').read() == iso, 'a 2048-byte image named .bin is copied unchanged')
        isozip = os.path.join(tmp, 'iso.zip')
        with zipfile.ZipFile(isozip, 'w') as z:
            z.write(iso_path, 'inner/disc.iso')
        out = os.path.join(tmp, 'iso-from-zip.iso')
        done = run('iso', isozip, out)
        check(done.returncode == 0 and open(out, 'rb').read() == iso, 'a .zip holding an .iso still works')

        # errors: no file is left behind
        lonely = os.path.join(tmp, 'lonely.cue')
        with open(lonely, 'w') as f:
            f.write('FILE "missing.bin" BINARY\n')
        out = os.path.join(tmp, 'lonely.iso')
        done = run('iso', lonely, out)
        check(done.returncode != 0 and not os.path.exists(out) and not os.path.exists(out + '.part'), 'a cue sheet naming a missing file stops with an error')
        empty = os.path.join(tmp, 'empty.zip')
        with zipfile.ZipFile(empty, 'w') as z:
            z.writestr('readme.txt', 'x')
        out = os.path.join(tmp, 'empty.iso')
        done = run('iso', empty, out)
        check(done.returncode != 0 and 'inside the archive' in done.stderr and not os.path.exists(out), 'a .zip with no image in it stops with an error')

        # the converted image is readable as a disc
        listed = run('list', iso_path)
        check(listed.returncode == 0 and 'GAME.ELF' in listed.stdout, 'disc.py can list the converted disc')

    print('disc bin/cue: %d failed (%d checks)' % (failed, checks))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
