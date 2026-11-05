"""No game data: a two-layer disc whose second layer gives its root directory the size 0 in the volume descriptor.

Some DVD-9 images do that and keep the true size only in the root's own "." record (public pull request 48). Writes
made-up two-layer images with scripts/mkiso.py (layer 1 is a second mkiso image, its sectors 16 and up, laid behind layer 0)
and checks that scripts/disc.py and the runner (ps2run's "[disc] ... files, ... layer 1 at" line) find the files of layer 1.
Bad variants must not crash or index rubbish: a "." record that points elsewhere, a first record that is too short, a huge size,
a root sector past the end of the image. The runner part uses PS2RUN=<path> or build/native-at-cpu-ee-alu/ps2run and is skipped
(with a line) when there is none. Run from the repository root: python3 tests/disc_layer1_cases.py (prints "disc layer 1 cases: 0 failed").
"""
import contextlib
import io
import os
import re
import struct
import subprocess
import sys
import tempfile

SECTOR = 2048
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
failed = 0
checks = 0


def check(ok, message):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print('FAIL: ' + message, file=sys.stderr)


def two_layers(work, pvd_root_size=0, patch=None, keep1=None):
    """Layer 0: SYSTEM.CNF, B0.ELF, L0A.BIN. Layer 1: SYSTEM.CNF, B1.ELF, L1A.BIN, L1B.BIN (4 layer-1 files).
    patch(layer1_bytes) may damage layer 1 (its sector 20 is the root directory); keep1 keeps only that many sectors of layer 1."""
    import mkiso
    names = {}
    for name, data in (('B0.ELF', b'\x7fELF' + bytes(60)), ('L0A.BIN', b'a' * 100), ('B1.ELF', b'\x7fELF' + bytes(60)),
                       ('L1A.BIN', b'b' * 5000), ('L1B.BIN', b'c' * 10)):
        names[name] = os.path.join(work, name)
        with open(names[name], 'wb') as f:
            f.write(data)
    one = mkiso.build(names['B0.ELF'], [names['L0A.BIN']])
    two = bytearray(mkiso.build(names['B1.ELF'], [names['L1A.BIN'], names['L1B.BIN']]))
    struct.pack_into('<I', two, 16 * SECTOR + 166, pvd_root_size)  # the volume descriptor's root size ...
    struct.pack_into('>I', two, 16 * SECTOR + 170, pvd_root_size)  # ... both byte orders; the "." record in sector 20 keeps 2048
    if patch:
        patch(two)
    image = bytes(one) + bytes(two[16 * SECTOR:])  # layer 1 starts with its own volume descriptor right behind layer 0
    if keep1:
        image = bytes(one) + bytes(two[16 * SECTOR:(16 + keep1) * SECTOR])
    path = os.path.join(work, 'two.iso')
    with open(path, 'wb') as f:
        f.write(image)
    return path


def py_files(path):
    import disc
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            files = [(p, l.index) for p, l, _, _ in disc.Image(path).files()]
    except SystemExit as e:
        return None, 'exit: %s' % e
    return files, err.getvalue()


def run_runner(runner, elf, path):
    p = subprocess.run([runner, elf, '--disc', path, '--frames', '1'], capture_output=True, timeout=120)
    text = (p.stdout + p.stderr).decode('latin-1')
    m = re.search(r'\[disc\] \S+: (\d+) files, (\d+) sectors, layer 1 at (\d+)', text)
    return p.returncode, (int(m.group(1)), int(m.group(3))) if m else None, text


def main():
    sys.path.insert(0, os.path.join(ROOT, 'scripts'))
    runner = os.path.abspath(os.environ.get('PS2RUN') or os.path.join(ROOT, 'build', 'native-at-cpu-ee-alu', 'ps2run.exe' if os.name == 'nt' else 'ps2run'))
    elf = os.path.join(ROOT, 'isos', 'autotests', 'cpu-ee-alu', 'TEST.ELF')
    have_runner = os.path.isfile(runner) and os.path.isfile(elf)
    if not have_runner:
        print('disc layer 1 cases: no runner (%s) or test program, runner part skipped' % runner)
    work = tempfile.mkdtemp(prefix='disc-layer1-')
    # (label, image arguments, layer 1 files expected from disc.py, runner file count: 3 names on layer 0 side + layer 1 names)
    cases = []

    def damage_dot_extent(img):
        struct.pack_into('<I', img, 20 * SECTOR + 2, 21)
        struct.pack_into('>I', img, 20 * SECTOR + 6, 21)

    def short_first(img):
        img[20 * SECTOR] = 20

    def huge_size(img):
        struct.pack_into('<I', img, 20 * SECTOR + 10, 0xFFFFFFFF)
        struct.pack_into('>I', img, 20 * SECTOR + 14, 0xFFFFFFFF)

    def root_far_away(img):
        struct.pack_into('<I', img, 16 * SECTOR + 158, 0x7FFFFF00)
        struct.pack_into('>I', img, 16 * SECTOR + 162, 0x7FFFFF00)

    # layer 0 alone has 3 files: SYSTEM.CNF, B0.ELF, L0A.BIN; the runner's index is by path, so layer 1's SYSTEM.CNF replaces layer 0's
    # and the count is 3 + 3 (B1.ELF, L1A.BIN, L1B.BIN) = 6 when layer 1 is read, 3 when it is not
    cases.append(('size 0 in the volume descriptor', dict(), 4, 6))
    cases.append(('size 10 in the volume descriptor', dict(pvd_root_size=10), 4, 6))
    cases.append(('true size in the volume descriptor (nothing changed)', dict(pvd_root_size=SECTOR), 4, 6))
    cases.append(('"." record points elsewhere', dict(patch=damage_dot_extent), 0, 3))
    cases.append(('first record too short', dict(patch=short_first), 0, 3))
    cases.append(('huge size in the "." record', dict(patch=huge_size), 0, 3))
    cases.append(('root sector past the end of the image', dict(patch=root_far_away), 0, 3))
    cases.append(('root sector cut off the image', dict(keep1=1), 0, None))
    for label, kwargs, want_l1, want_count in cases:
        path = two_layers(work, **kwargs)
        files, info = py_files(path)
        if want_l1 or label != 'root sector cut off the image':
            check(files is not None, '%s: disc.py stops (%s)' % (label, info))
        if files is not None:
            l0 = sorted(p for p, i in files if i == 0)
            l1 = sorted(p for p, i in files if i == 1)
            check(l0 == ['B0.ELF', 'L0A.BIN', 'SYSTEM.CNF'], '%s: layer 0 files are as before, got %s' % (label, l0))
            if want_l1:
                check(l1 == ['B1.ELF', 'L1A.BIN', 'L1B.BIN', 'SYSTEM.CNF'], '%s: disc.py finds layer 1 files, got %s' % (label, l1))
            else:
                check(l1 == [], '%s: disc.py finds no layer 1 files, got %s' % (label, l1))
        if have_runner and want_count is not None:
            rc, line, text = run_runner(runner, elf, path)
            check(rc is not None and rc >= 0, '%s: the runner does not crash (code %s)' % (label, rc))
            check(line is not None and line[0] == want_count, '%s: runner says %s files, wanted %s' % (label, line and line[0], want_count))
        elif have_runner:
            rc, line, text = run_runner(runner, elf, path)
            check(rc >= 0 and line is not None, '%s: the runner does not crash and still opens the image (code %s)' % (label, rc))
    print('disc layer 1 cases: %d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
