"""No game data: scripts/chd.py (and scripts/extract_disc.py with it) turn .chd disc images into plain images.

The small .chd files in tests/chd_fixtures.py were written by MAME's chdman from made-up data (a tiny disc made with
scripts/mkiso.py plus text, noise, zeros, repeated blocks and a sine wave), one per way chdman can compress:
  DVD  : no compression, zlib, lzma, huff, flac, zstd, and chdman's own default (lzma, zlib, huff, flac)
  CD   : cdzl, cdlz, cdzs, cdfl and the default, in mode 1 and mode 2 form 1, with an audio track behind the data track
Each must unpack to exactly the bytes it was made from. Also checked: a few random reads, the .part file is gone, the toolkit's
`extract_disc.py inspect` reads the same files, and damaged or unsupported files give a clear error and leave no image behind.

    python tests/chd_check.py                       prints "chd: 0 failed"
    python tests/chd_check.py --chdman PATH         also makes fresh .chd files with a chdman you have (bigger data, more hunks)
    python tests/chd_check.py --make-fixtures PATH  rewrites tests/chd_fixtures.py with that chdman (the data is the same every time)

The native unpacker (recomp/chdunpack, built into build/tools by scripts/setup.sh and port.sh) must give the same bytes and the same
refusals as scripts/chd.py for every file: it is found in build/tools, or named with --chdunpack PATH. Without it those checks are
skipped and the output says so.
"""
import base64
import hashlib
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'scripts'))
import chd  # noqa: E402

MKISO = os.path.join(HERE, '..', 'scripts', 'mkiso.py')
EXTRACT = os.path.join(HERE, '..', 'scripts', 'extract_disc.py')
failed = 0
checks = 0
NATIVE = None  # the path of chdunpack, when there is one


def check(ok, message):
    global failed, checks
    checks += 1
    if not ok:
        failed += 1
        print('FAIL: ' + message, file=sys.stderr)


# --------------------------------------------------------------------------------------------------------------------
# The made-up data (the same bytes every run)
# --------------------------------------------------------------------------------------------------------------------

def noise(rnd, n):
    return bytes(rnd.getrandbits(8) for _ in range(n))


def make_disc(tmp):
    """A small valid ISO 9660 disc (mkiso.py) whose one file is half text, half noise."""
    rnd = random.Random(5)
    words = [b'alpha', b'beta', b'gamma', b'delta', b'sector', b'hunk']
    text = b''.join(rnd.choice(words) + b' ' for _ in range(700))[:3000]
    boot = os.path.join(tmp, 'GAME.ELF')
    with open(boot, 'wb') as f:
        f.write(b'\x7fELF' + text + noise(rnd, 1000))
    iso_path = os.path.join(tmp, 'disc.iso')
    done = subprocess.run([sys.executable, '-I', MKISO, iso_path, 'GAME.ELF'], capture_output=True, text=True, cwd=tmp)
    if done.returncode:
        raise RuntimeError('mkiso.py failed: ' + done.stderr)
    return open(iso_path, 'rb').read()


def dvd_image(iso):
    """The disc, then more sectors: zeros, text, noise, one 4 KiB block four times (the file format shares those), 16-bit audio."""
    rnd = random.Random(11)
    words = [b'level', b'sound', b'disc', b'game']
    audio = bytearray()
    for i in range(1024):
        audio += struct.pack('<hh', int(9000 * math.sin(i * 0.05) + rnd.randint(-30, 30)), int(7000 * math.sin(i * 0.031) + rnd.randint(-30, 30)))
    block = noise(rnd, 2048)
    table = b''.join(struct.pack('<I', (i * 7) & 0xFFFF) for i in range(512))
    out = iso + bytes(2048) + b''.join(rnd.choice(words) + b' ' for _ in range(900))[:3072] + noise(rnd, 2048) + block * 4 + bytes(audio)[:4096] + table
    return out + bytes(-len(out) % 2048)


def cd_audio(frames):
    rnd = random.Random(13)
    pcm = bytearray()
    for i in range(frames * 588):
        pcm += struct.pack('<hh', int(10000 * math.sin(i * 0.02) + rnd.randint(-30, 30)), int(8000 * math.sin(i * 0.013) + rnd.randint(-30, 30)))
    return bytes(pcm)


# CD sectors with their error codes, as a real dump has them (ECMA-130: EDC and the P and Q parities)
_EDC = []
for _i in range(256):
    _j = _i
    for _ in range(8):
        _j = (_j >> 1) ^ (0xD8018001 if _j & 1 else 0)
    _EDC.append(_j)
_F = [0] * 256
_B = [0] * 256
for _i in range(256):
    _v = ((_i << 1) ^ (0x11D if _i & 0x80 else 0)) & 0xFF
    _F[_i] = _v
    _B[_i ^ _v] = _i
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def _edc(buf):
    c = 0
    for b in buf:
        c = (c >> 8) ^ _EDC[(c ^ b) & 0xFF]
    return c


def _ecc(src, major_count, minor_count, major_mult, minor_inc):
    size = major_count * minor_count
    out = bytearray(major_count * 2)
    for major in range(major_count):
        index = (major >> 1) * major_mult + (major & 1)
        a = b = 0
        for _ in range(minor_count):
            t = src[index]
            index += minor_inc
            if index >= size:
                index -= size
            a ^= t
            b ^= t
            a = _F[a]
        a = _B[_F[a] ^ b]
        out[major] = a
        out[major + major_count] = a ^ b
    return out


def _bcd(n):
    return int('%02d' % n, 16)


def raw_sector(lba, data, mode):
    s = bytearray(2352)
    s[0:12] = SYNC
    n = lba + 150
    s[12:15] = bytes([_bcd(n // 4500), _bcd(n // 75 % 60), _bcd(n % 75)])
    s[15] = mode
    if mode == 1:
        s[16:2064] = data
        s[2064:2068] = struct.pack('<I', _edc(s[0:2064]))
        src = bytes(s[12:2076])
        s[2076:2248] = _ecc(src, 86, 24, 2, 86)
        s[2248:2352] = _ecc(bytes(s[12:2248]), 52, 43, 86, 88)
    else:
        s[16:24] = bytes([0, 0, 8, 0, 0, 0, 8, 0])
        s[24:2072] = data
        s[2072:2076] = struct.pack('<I', _edc(s[16:2072]))
        t = bytearray(s)
        t[12:16] = bytes(4)  # the address is left out of the parities in mode 2
        src = bytes(t[12:2076])
        p = _ecc(src, 86, 24, 2, 86)
        s[2076:2248] = p
        s[2248:2352] = _ecc(src + bytes(p), 52, 43, 86, 88)
    return bytes(s)


def cd_files(tmp, iso, mode):
    """(.cue path, the data track as 2048-byte sectors): the disc plus noise, 14 sectors of it zeros, then 6 frames of audio."""
    rnd = random.Random(17 + mode)
    data = bytearray(iso)
    data += noise(rnd, 2048 * 2) + bytes(2048 * 8) + b''.join(b'cd ' + bytes([i % 7]) for i in range(2048 * 3 // 4))[:2048 * 3]
    data += bytes(-len(data) % 2048)
    total = len(data) // 2048
    binfile = os.path.join(tmp, 'cd%d.bin' % mode)
    with open(binfile, 'wb') as f:
        for n in range(total):
            f.write(raw_sector(n, bytes(data[n * 2048:(n + 1) * 2048]), mode))
        f.write(cd_audio(4))
    cue = os.path.join(tmp, 'cd%d.cue' % mode)
    with open(cue, 'w') as f:
        f.write('FILE "cd%d.bin" BINARY\n  TRACK 01 MODE%d/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 %02d:%02d:%02d\n'
                % (mode, mode, total // 4500, total // 75 % 60, total % 75))
    return cue, bytes(data)


# --------------------------------------------------------------------------------------------------------------------
# Making the fixtures with a real chdman
# --------------------------------------------------------------------------------------------------------------------

DVD_KINDS = {'zlib': 'zlib', 'lzma': 'lzma', 'huff': 'huff', 'flac': 'flac', 'zstd': 'zstd', 'default': None}
CD_FIXTURES = {(1, 'cdzl'), (1, 'cdlz'), (2, 'cdzs'), (2, 'cdfl'), (2, 'default')}  # enough to cover both sector forms and every codec
CD_KINDS = {'cdzl': 'cdzl', 'cdlz': 'cdlz', 'cdzs': 'cdzs', 'cdfl': 'cdfl', 'default': None}


def run_chdman(chdman, args):
    done = subprocess.run([chdman] + args, capture_output=True, text=True)
    if done.returncode:
        raise RuntimeError('chdman failed: %s %s' % (' '.join(args), done.stdout[-300:] + done.stderr[-300:]))


def build_all(chdman, tmp, iso, dvd, big=False):
    """Returns {name: (chd path, expected image bytes)}."""
    made = {}
    src = os.path.join(tmp, 'dvd.iso')
    with open(src, 'wb') as f:
        f.write(dvd)
    for kind, codec in DVD_KINDS.items():
        out = os.path.join(tmp, 'dvd-%s.chd' % kind)
        run_chdman(chdman, ['createdvd', '-i', src, '-o', out, '-f', '-hs', '2048'] + (['-c', codec] if codec else []))
        made['dvd-' + kind] = (out, dvd)
    for mode in (1, 2):
        cue, data = cd_files(tmp, iso, mode)
        for kind, codec in CD_KINDS.items():
            if (mode, kind) not in CD_FIXTURES:
                continue
            out = os.path.join(tmp, 'cd%d-%s.chd' % (mode, kind))
            run_chdman(chdman, ['createcd', '-i', cue, '-o', out, '-f', '-hs', '9792'] + (['-c', codec] if codec else []))
            made['cd%d-%s' % (mode, kind)] = (out, data)
    return made


def make_fixtures(chdman, target):
    with tempfile.TemporaryDirectory() as tmp:
        iso = make_disc(tmp)
        made = build_all(chdman, tmp, iso, dvd_image(iso))
        lines = ['"""Small .chd files made by MAME\'s chdman from made-up data: see tests/chd_check.py (python tests/chd_check.py --make-fixtures PATH-TO-CHDMAN).',
                 'They hold no game data. Each entry: the sha1 of the image it unpacks to, then the base64 text of the .chd file."""', '', 'FIXTURES = {']
        for name in sorted(made):
            raw = open(made[name][0], 'rb').read()
            text = base64.b64encode(raw).decode('ascii')
            lines.append("    '%s': (" % name)
            lines.append("        '%s'," % hashlib.sha1(made[name][1]).hexdigest())
            for i in range(0, len(text), 100):
                lines.append("        '%s'" % text[i:i + 100])
            lines.append('    ),')
        lines.append('}')
        with open(target, 'w', newline='\n') as f:
            f.write('\n'.join(lines) + '\n')
    print('wrote %s' % target)


# --------------------------------------------------------------------------------------------------------------------
# The checks
# --------------------------------------------------------------------------------------------------------------------

def check_image(label, chd_path, expect=None, sha1=None):
    out = chd_path + '.iso'
    try:
        n = chd.convert(chd_path, out, jobs=1)
        got = open(out, 'rb').read()
        if expect is not None:
            check(got == expect, '%s: the unpacked image is not the original (%d bytes against %d)' % (label, len(got), len(expect)))
        if sha1 is not None:
            check(hashlib.sha1(got).hexdigest() == sha1, '%s: the unpacked image is not the original (sha1)' % label)
        check(n == len(got), '%s: wrong number of bytes reported' % label)
    except Exception as e:  # noqa: BLE001
        check(False, '%s: %s: %s' % (label, type(e).__name__, e))
        return
    check(not os.path.exists(out + '.part'), '%s: a .part file is left behind' % label)
    # random reads, the way the disc toolkit asks for sectors
    img = chd.ChdImage(chd_path)
    try:
        check(img.total_sectors == len(got) // 2048, '%s: wrong sector count %d' % (label, img.total_sectors))
        rnd = random.Random(1)
        ok = True
        for _ in range(40):
            s = rnd.randrange(img.total_sectors)
            c = rnd.randint(1, 9)
            ok = ok and img.read_sectors(s, c) == got[s * 2048:(s + c) * 2048]
        check(ok, '%s: random sector reads differ from the unpacked image' % label)
    finally:
        img.close()


def native_run(tool, src, dst, jobs=None):
    cmd = [tool] + (['--jobs', str(jobs)] if jobs else []) + [src, dst]
    return subprocess.run(cmd, capture_output=True, text=True)


def check_native(label, tool, chd_path, expect, sha1=None):
    """The native unpacker gives the same bytes as the Python code (expect), with one thread and with several."""
    for jobs in (1, 3):
        out = chd_path + '.native%d.iso' % jobs
        done = native_run(tool, chd_path, out, jobs)
        check(done.returncode == 0, '%s: chdunpack --jobs %d failed: %s' % (label, jobs, done.stderr[-200:]))
        if done.returncode:
            continue
        got = open(out, 'rb').read()
        check(got == expect, '%s: chdunpack --jobs %d gives other bytes than chd.py (%d against %d)' % (label, jobs, len(got), len(expect)))
        if sha1:
            check(hashlib.sha1(got).hexdigest() == sha1, '%s: chdunpack --jobs %d: sha1 differs' % (label, jobs))
        check('%d %%' % 100 in done.stdout, '%s: chdunpack prints no progress line' % label)
        check(not os.path.exists(out + '.part'), '%s: chdunpack left a .part file behind' % label)
        os.remove(out)


def damaged_cases(tmp, name, raw):
    """Files that must be refused with a clear message and leave nothing behind."""
    cases = {
        'not a chd': b'hello world' * 50,
        'old version': raw[:12] + struct.pack('>I', 4) + raw[16:],
        'unknown codec': raw[:16] + b'avhu' + raw[20:],
        'has a parent': raw[:104] + b'\x01' * 20 + raw[124:],
        'cut off': raw[:len(raw) // 2],
    }
    for label, data in cases.items():
        path = os.path.join(tmp, 'bad.chd')
        with open(path, 'wb') as f:
            f.write(data)
        out = os.path.join(tmp, 'bad.iso')
        runs = [('chd.py', subprocess.run([sys.executable, '-I', os.path.join(HERE, '..', 'scripts', 'chd.py'), 'iso', path, out], capture_output=True, text=True,
                                          env=dict(os.environ, PS2_CHD_NATIVE='0')))]
        if NATIVE:
            runs.append(('chdunpack', native_run(NATIVE, path, out)))
        for who, done in runs:
            check(done.returncode != 0, '%s (%s): %s accepted it' % (name, label, who))
            check('Traceback' not in done.stderr, '%s (%s): %s crashed instead of saying what is wrong: %s' % (name, label, who, done.stderr[-200:]))
            check(done.returncode == 0 or done.stderr.startswith('chd.py: '), '%s (%s): %s gave no sentence of its own: %s' % (name, label, who, done.stderr[-200:]))
            check(not os.path.exists(out) and not os.path.exists(out + '.part'), '%s (%s): %s left an image behind' % (name, label, who))


def main():
    args = sys.argv[1:]
    if args[:1] == ['--make-fixtures']:
        make_fixtures(args[1], os.path.join(HERE, 'chd_fixtures.py'))
        return 0
    global NATIVE
    NATIVE = args[args.index('--chdunpack') + 1] if '--chdunpack' in args else chd.native_unpacker()
    if not NATIVE:
        print('chd: the native unpacker is not built (build/tools/chdunpack): its checks are skipped')
    with tempfile.TemporaryDirectory() as tmp:
        from chd_fixtures import FIXTURES
        for name in sorted(FIXTURES):
            path = os.path.join(tmp, name + '.chd')
            sha1, *text = FIXTURES[name]
            with open(path, 'wb') as f:
                f.write(base64.b64decode(''.join(text)))
            check_image(name, path, sha1=sha1)
            if NATIVE:
                check_native(name, NATIVE, path, open(path + '.iso', 'rb').read(), sha1)
        check(len(FIXTURES) >= 11, 'fixtures are missing')
        # the codecs really are in the files (a fixture that fell back to "none" would test nothing)
        want = {'dvd-zlib': b'zlib', 'dvd-lzma': b'lzma', 'dvd-huff': b'huff', 'dvd-flac': b'flac', 'dvd-zstd': b'zstd', 'cd1-cdzl': b'cdzl', 'cd1-cdlz': b'cdlz', 'cd2-cdzs': b'cdzs', 'cd2-cdfl': b'cdfl'}
        for name, tag in want.items():
            c = chd.Chd(os.path.join(tmp, name + '.chd'))
            used = {c.codecs[t] for t in c.types if t <= 3}
            c.close()
            check(tag in used, '%s: no hunk is compressed with %s' % (name, tag.decode()))
        # shared hunks (self references) and CD sync flags are exercised too
        c = chd.Chd(os.path.join(tmp, 'dvd-zlib.chd'))
        check(any(t == 5 for t in c.types), 'dvd-zlib: no hunk refers to an earlier one')
        c.close()
        # the default of chdman uses more than one codec in one file
        c = chd.Chd(os.path.join(tmp, 'dvd-default.chd'))
        check(len({t for t in c.types if t <= 3}) >= 2, 'dvd-default: only one codec used')
        c.close()
        c = chd.Chd(os.path.join(tmp, 'cd1-cdzl.chd'))
        flagged = 0
        for h in range(c.hunks):
            if c.types[h] <= 3:
                c.fp.seek(c.offsets[h])
                flagged += any(c.fp.read((c.frames_per_hunk + 7) // 8))
        c.close()
        check(flagged > 0, 'cd1-cdzl: no hunk has the sync flags set')

        # the disc toolkit reads a .chd through the same code: inspect and extract name the boot program
        for name in ('dvd-default', 'cd1-cdlz', 'cd2-cdfl', 'dvd-zstd'):
            done = subprocess.run([sys.executable, '-I', EXTRACT, 'inspect', os.path.join(tmp, name + '.chd')], capture_output=True, text=True)
            check(done.returncode == 0 and 'GAME.ELF' in done.stdout, '%s: extract_disc.py inspect: %s' % (name, (done.stdout + done.stderr)[-200:]))
            check('CHD-v5' in done.stdout, '%s: extract_disc.py does not name the container' % name)
        folder = os.path.join(tmp, 'extracted')
        done = subprocess.run([sys.executable, '-I', EXTRACT, 'extract', os.path.join(tmp, 'cd2-cdzs.chd'), folder], capture_output=True, text=True)
        found = []
        for root, _dirs, files in os.walk(folder):
            found += [f for f in files if f.upper().startswith('GAME.ELF')]
        check(done.returncode == 0 and found, 'cd2-cdzs: extract_disc.py extract did not write GAME.ELF: %s' % (done.stdout + done.stderr)[-200:])
        done = subprocess.run([sys.executable, '-I', EXTRACT, 'chd-iso', os.path.join(tmp, 'cd1-cdzl.chd'), os.path.join(tmp, 'x.iso')], capture_output=True, text=True)
        check(done.returncode == 0 and open(os.path.join(tmp, 'x.iso'), 'rb').read() == open(os.path.join(tmp, 'cd1-cdzl.chd.iso'), 'rb').read(), 'extract_disc.py chd-iso gives the original')

        damaged_cases(tmp, 'dvd-lzma', open(os.path.join(tmp, 'dvd-lzma.chd'), 'rb').read())
        damaged_cases(tmp, 'cd1-cdzl', open(os.path.join(tmp, 'cd1-cdzl.chd'), 'rb').read())

        # a flipped byte inside a stored (not compressed) hunk of a DVD is caught by the checksum the file carries
        raw = bytearray(open(os.path.join(tmp, 'dvd-zlib.chd'), 'rb').read())
        c = chd.Chd(os.path.join(tmp, 'dvd-zlib.chd'))
        at = next(int(c.offsets[h]) for h in range(c.hunks) if c.types[h] == 4 and c.offsets[h]) + 100
        c.close()
        raw[at] ^= 0x55
        flipped = os.path.join(tmp, 'flipped.chd')
        with open(flipped, 'wb') as f:
            f.write(raw)
        try:
            chd.convert(flipped, os.path.join(tmp, 'flipped.iso'), jobs=1)
            check(False, 'a damaged DVD was accepted')
        except chd.ChdError as e:
            check('checksum' in str(e), 'a damaged DVD: wrong message: %s' % e)
        check(not os.path.exists(os.path.join(tmp, 'flipped.iso')), 'a damaged DVD left an image behind')
        if NATIVE:
            done = native_run(NATIVE, flipped, os.path.join(tmp, 'flipped.iso'))
            check(done.returncode == 1 and 'checksum' in done.stderr, 'a damaged DVD: chdunpack: wrong answer: %s' % done.stderr[-200:])
            check(not os.path.exists(os.path.join(tmp, 'flipped.iso')) and not os.path.exists(os.path.join(tmp, 'flipped.iso.part')), 'a damaged DVD left an image behind (chdunpack)')

        # several worker processes give the same bytes as one
        out = os.path.join(tmp, 'many.iso')
        chd.convert(os.path.join(tmp, 'dvd-default.chd'), out, jobs=3)
        check(open(out, 'rb').read() == open(os.path.join(tmp, 'dvd-default.chd.iso'), 'rb').read(), 'dvd-default with 3 worker processes differs')

        if '--chdman' in args:
            iso = make_disc(tmp)
            live(args[args.index('--chdman') + 1], tmp, iso, dvd_image(iso))
    print('chd: %d failed (%d checks)' % (failed, checks))
    return 1 if failed else 0


def live(chdman, tmp, iso, dvd):
    """Fresh files from the chdman at hand, with a bigger made-up image so there are thousands of hunks."""
    rnd = random.Random(23)
    big = bytearray(dvd)
    for _ in range(12):
        big += noise(rnd, 8192) + dvd[len(iso):len(iso) + 40960] + bytes(8192)
    big = bytes(big)
    made = {}
    src = os.path.join(tmp, 'big.iso')
    with open(src, 'wb') as f:
        f.write(big)
    for kind, codec in DVD_KINDS.items():
        out = os.path.join(tmp, 'big-%s.chd' % kind)
        run_chdman(chdman, ['createdvd', '-i', src, '-o', out, '-f'] + (['-c', codec] if codec else []))
        made['big-' + kind] = (out, big)
    run_chdman(chdman, ['createdvd', '-i', src, '-o', os.path.join(tmp, 'big-hs.chd'), '-f', '-hs', '65536', '-c', 'zstd,lzma'])
    made['big-hs'] = (os.path.join(tmp, 'big-hs.chd'), big)
    for name, (path, expect) in made.items():
        check_image(name, path, expect)
        check(hashlib.sha1(open(path + '.iso', 'rb').read()).digest() == hashlib.sha1(expect).digest(), '%s: sha1 differs' % name)
        if NATIVE:
            check_native(name, NATIVE, path, expect)


if __name__ == '__main__':
    sys.exit(main())
