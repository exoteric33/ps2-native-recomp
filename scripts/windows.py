#!/usr/bin/env python3
"""Code window support: the segments a game rewrites at runtime.

An executable LOAD segment with no file contents is a code window: the game
unpacks a different image into it as it goes, so no single translation of the
area can be right. The recompiler instead takes one module per image of the
window, and the runtime picks the image in place by its check words.

  windows.py list <elf>              print "base size" per window, one per line
  windows.py capture <elf> <dump> <dir> [<misses file>]
                                     save the image of every window found in
                                     the RAM dump to <dir>/<base>_<crc>.bin,
                                     and rewrite plain entry misses inside a
                                     window as <base>_<crc>:<offset>, naming
                                     the image the dump holds (or the saved one
                                     that is the same code with other data)
  windows.py trim <dir> [<entries file>]
                                     bring a folder of images down to the limits
                                     below and tidy the entries file to match

The RAM dump is the one the runner writes with PS2_DUMP, so at a halt it holds
exactly the image the game was running when it called the missing entry.

The folder is bounded, because the build translates every image in it (a window
that shares its memory with changing data gets a new name, the CRC, every time:
Tekken 5 wrote 374 MB in a few runs). The same rules as runtime/src/ee_live.cpp:
an image that differs from a saved one at the same address in at most 1/16 of its
words is the same code with other data and is not saved again; at most 8 images
per address, 32 in all, 16 MiB together, the least recently used go first. Only
names of the form <8 hex>_<8 hex>.bin are managed.
"""
import binascii
import os
import re
import struct
import sys

UNIT = 0x4000
PER_BASE, TOTAL, TOTAL_BYTES = 8, 32, 16 << 20
NAME = re.compile(r'^([0-9A-Fa-f]{8})_[0-9A-Fa-f]{8}\.bin$')


def load_phdrs(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        yield struct.unpack_from('<8I', elf, phoff + i * phentsize)


def windows(elf):
    out = []
    for v in load_phdrs(elf):
        if v[0] != 1 or v[4] or not v[5] or not (v[6] & 1) or v[2] >= 0x02000000:  # no PT_LOAD, has contents, empty, not executable, or outside RAM
            continue
        base = v[2] & ~(UNIT - 1)
        limit = min(0x02000000, (v[2] + v[5] + UNIT - 1) & ~(UNIT - 1))
        while base < limit and base in [b for b, _ in out]:
            base += UNIT
        if base < limit:
            out.append((base, UNIT))
    return out


def extent(dump, base, cap=0x40000):
    # The ELF only says the window begins here; what the game unpacks decides
    # how far it reaches, so the image covers the units it wrote, stopping at
    # the first entirely empty one.
    size = 0
    while size < cap and size + UNIT <= len(dump) - base:
        if dump[base + size:base + size + UNIT] == b'\0' * UNIT:
            break
        size += UNIT
    return size or UNIT


def saved_images(folder):
    """The managed images of a folder, newest first: (name, base, size, mtime, path)."""
    found = []
    if os.path.isdir(folder):
        for f in os.listdir(folder):
            m = NAME.match(f)
            if m:
                path = os.path.join(folder, f)
                st = os.stat(path)
                found.append((f[:-4], int(m.group(1), 16), st.st_size, st.st_mtime, path))
    found.sort(key=lambda o: (-o[3], o[0]))
    return found


def near_duplicate(a, b):
    """The same code with other data: at most 1/16 of the words differ (a different length counts as different words)."""
    common, longest = min(len(a), len(b)) // 4, max(len(a), len(b)) // 4
    wa, wb = memoryview(a[:common * 4]).cast('I'), memoryview(b[:common * 4]).cast('I')
    different = longest - common + sum(1 for x, y in zip(wa, wb) if x != y)
    return different * 16 <= longest


def similar_saved(images, base, blob):
    """The newest saved image at this address that stands for blob, or None."""
    for name, b, size, _, path in images:
        if b == base and len(blob) // 2 <= size <= len(blob) * 2 and near_duplicate(open(path, 'rb').read(), blob):
            return name, path
    return None


def trim(folder, entries=None, keep=None):
    """Remove the images past the limits (least recently used first, `keep` never); returns the number removed.
    Entry lines of a removed image move to the saved image that is the same code, or are dropped."""
    images = saved_images(folder)
    images.sort(key=lambda o: (o[0] != keep, -o[3], o[0]))
    kept, per_base, total, size = [], {}, 0, 0
    removed = []
    for o in images:
        if o[0] == keep or (per_base.get(o[1], 0) < PER_BASE and total < TOTAL and size + o[2] <= TOTAL_BYTES):
            per_base[o[1]] = per_base.get(o[1], 0) + 1
            total += 1
            size += o[2]
            kept.append(o)
        else:
            removed.append(o)
    stand_in = {}
    for o in removed:
        blob = open(o[4], 'rb').read()
        hit = similar_saved(kept, o[1], blob)
        if hit:
            stand_in[o[0]] = hit[0]
    for o in removed:
        os.remove(o[4])
    if entries and os.path.isfile(entries):
        existing = {o[0] for o in kept}
        seen, lines, changed = set(), [], False
        for line in open(entries).read().split('\n'):
            if line.lstrip().startswith('#'):
                lines.append(line)
                continue
            out = []
            for tok in line.split():
                name, colon, at = tok.partition(':')
                if colon and NAME.match(name + '.bin'):
                    name = name if name in existing else stand_in.get(name)
                    tok = '%s:%s' % (name, at) if name else None
                    if tok is None or tok in seen:
                        changed = True
                        continue
                    seen.add(tok)
                out.append(tok)
            if out or not line.strip():
                lines.append(' '.join(out) if out else line)
            else:
                changed = True
        if changed:
            open(entries, 'w', newline='\n').write('\n'.join(lines))
    return len(removed)


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in ('list', 'capture', 'trim') or \
            len(sys.argv) not in ({'list': (3,), 'capture': (5, 6), 'trim': (3, 4)}[sys.argv[1]]):
        sys.exit(__doc__)
    mode = sys.argv[1]
    sys.stdout.reconfigure(newline='\n')
    if mode == 'trim':
        n = trim(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else None)
        if n:
            print('trimmed %d images from %s (limits: %d per address, %d in all, %d MiB)' % (n, sys.argv[2], PER_BASE, TOTAL, TOTAL_BYTES >> 20))
        return
    elf = open(sys.argv[2], 'rb').read()
    wins = windows(elf)
    if mode == 'list':
        for base, size in wins:
            print('%08x %x' % (base, size))
        return

    dump = open(sys.argv[3], 'rb').read()
    outdir = sys.argv[4]
    misses = sys.argv[5] if len(sys.argv) > 5 else None
    for base, _ in wins:
        size = extent(dump, base)
        blob = dump[base:base + size]
        if not any(blob):
            continue
        crc = binascii.crc32(blob) & 0xFFFFFFFF
        stem = '%08X_%08X' % (base, crc)
        path = os.path.join(outdir, stem + '.bin')
        if os.path.exists(path):
            os.utime(path)                # used again: the newest
        else:
            hit = similar_saved(saved_images(outdir), base, blob)
            if hit:
                stem, path = hit          # the same code with other data is saved already
                os.utime(path)
            else:
                os.makedirs(outdir, exist_ok=True)
                open(path, 'wb').write(blob)
                print('captured %s, %d bytes' % (stem, size))
                removed = trim(outdir, None, stem)
                if removed:
                    print('dropped %d images past the limits' % removed)
        if misses and os.path.exists(misses) and os.path.exists(path):
            lines = open(misses).read().split()
            names = {}
            for k, token in enumerate(lines):
                if ':' in token:
                    continue
                pc = int(token, 16)
                if base <= pc < base + size:
                    names[token] = '%s:%x' % (stem, pc - base)
            if names:
                open(misses, 'w', newline='\n').write('\n'.join(names.get(t, t) for t in lines) + '\n')


if __name__ == '__main__':
    main()