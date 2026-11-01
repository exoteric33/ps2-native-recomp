import argparse
import fnmatch
import os
import struct
import sys

SECTOR = 2048
MAX_DIRECTORY = 16 << 20  # no real directory is near this (a record is at least 34 bytes); the runtime's disc.cpp uses the same


def warn(message):
    print('disc.py: ' + message, file=sys.stderr)


def safe_name(name):
    # An ISO 9660 name is raw bytes from the image: one that is "." or "..", or holds a path separator, a drive colon or a
    # control byte, would make a path outside the destination folder. Real discs use only letters, digits, "_" and ".".
    return name not in ('', '.', '..') and not any(c in '/\\:' or ord(c) < 32 for c in name)


def inside(destination, path):
    base = os.path.realpath(destination)
    target = os.path.realpath(os.path.join(base, path))
    return os.path.commonpath([base, target]) == base


class Layer:
    def __init__(self, index, base, pvd):
        self.index = index
        self.base = base
        self.volume = pvd[40:72].decode('ascii', 'replace').strip()
        self.sectors = struct.unpack_from('<I', pvd, 80)[0]
        self.root = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]


class Image:
    def __init__(self, path):
        self.path = path
        self.file = open(path, 'rb')
        self.size = os.path.getsize(path)
        self.layers = [Layer(0, 0, self.pvd(16))]
        first = self.layers[0].sectors
        for sector in range(first, min(first + 64, self.size // SECTOR)):
            if self.read(sector, 1)[:6] == b'\x01CD001':
                self.layers.append(Layer(1, sector - 16, self.read(sector, 1)))
                break

    def read(self, sector, count):
        self.file.seek(sector * SECTOR)
        return self.file.read(count * SECTOR)

    def pvd(self, sector):
        data = self.read(sector, 1)
        if data[:6] != b'\x01CD001':
            sys.exit('not an ISO 9660 image: no primary volume descriptor at sector %d' % sector)
        return data

    def files(self):
        for layer in self.layers:
            yield from self.walk(layer, layer.root[0], layer.root[1], '', 0, set())

    def walk(self, layer, lba, size, prefix, depth, seen):
        # A damaged or crafted image: a directory that points back at a parent is walked once, one that is too deep, too large or
        # whose records run past its end is left out with a warning, and one past the end of the file stops with an error.
        if depth > 16 or layer.base + lba in seen:
            return
        seen.add(layer.base + lba)
        if size > MAX_DIRECTORY:
            warn('damaged directory %r at sector %d: too large (left out)' % (prefix, layer.base + lba))
            return
        data = self.read(layer.base + lba, (size + SECTOR - 1) // SECTOR)
        if len(data) < size:
            sys.exit('%s: the disc image ends early (truncated rip or copy?)' % self.path)
        offset = 0
        while offset < size:
            length = data[offset]
            if length == 0:
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            # ISO 9660 (ECMA-119 9.1): a directory record is 33 bytes plus the name, all inside the directory
            if length < 34 or offset + length > len(data) or 33 + data[offset + 32] > length:
                warn('damaged directory %r at sector %d: a record runs past its end (left out)' % (prefix, layer.base + lba))
                return
            record = data[offset:offset + length]
            offset += length
            extent, extent_size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
            name = record[33:33 + record[32]]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii', 'replace').split(';')[0]
            if not safe_name(name):
                warn('unsafe name %r in %r left out' % (name, prefix))
                continue
            path = prefix + name
            if record[25] & 2:
                yield from self.walk(layer, extent, extent_size, path + '/', depth + 1, seen)
            else:
                yield path, layer, layer.base + extent, extent_size

    def find(self, path):
        wanted = path.replace('\\', '/').strip('/').upper()
        for entry in self.files():
            if entry[0].upper() == wanted:
                return entry
        return None

    def copy(self, entry, destination):
        path, _, sector, size = entry
        os.makedirs(os.path.dirname(destination) or '.', exist_ok=True)
        self.file.seek(sector * SECTOR)
        with open(destination, 'wb') as out:
            left = size
            while left:
                chunk = self.file.read(min(left, 1 << 22))
                if not chunk:
                    out.close()
                    os.remove(destination)
                    sys.exit('%s: the disc image ends early (truncated rip or copy?) in %s' % (self.path, path))
                out.write(chunk)
                left -= len(chunk)


def info(image):
    for layer in image.layers:
        print('layer %d: volume %s, base sector %d, %d sectors' % (layer.index, layer.volume, layer.base, layer.sectors))
    entry = image.find('SYSTEM.CNF')
    if entry:
        image.file.seek(entry[2] * SECTOR)
        print(image.file.read(entry[3]).decode('ascii', 'replace').strip())


def listing(image):
    for path, layer, sector, size in image.files():
        print('%d %10d %12d  %s' % (layer.index, sector, size, path))


def extract(image, destination, patterns, limit):
    count = 0
    for entry in image.files():
        path, _, _, size = entry
        if patterns and not any(fnmatch.fnmatch(path.upper(), p.upper()) for p in patterns):
            continue
        if not patterns and limit and size > limit:
            print('left in the image: %s (%d MiB)' % (path, size >> 20))
            continue
        if not inside(destination, path):
            sys.exit('refusing to write outside %s: %r' % (destination, path))
        image.copy(entry, os.path.join(destination, path))
        count += 1
    print('%d files extracted to %s' % (count, destination))


def read_at(image, sector, offset, size):
    image.file.seek(sector * SECTOR + offset)
    return image.file.read(size)


def modules(image, destination):
    found = []
    for path, _, sector, size in image.files():
        head = read_at(image, sector, 0, 8)
        if head[:4] in (b'SNR2', b'xff2'):
            found.append((path.replace('/', '_'), sector, 0, size))
        elif head[:4] == b'AFS\0':
            count = struct.unpack_from('<I', head, 4)[0]
            table = read_at(image, sector, 8, min(count * 8, max(size - 8, 0)))
            for index in range(len(table) // 8):
                offset, length = struct.unpack_from('<II', table, index * 8)
                if offset + 4 <= size and read_at(image, sector, offset, 4) == b'SNR2':
                    found.append(('%s_%04d' % (path.replace('/', '_'), index), sector, offset, length))
    os.makedirs(destination, exist_ok=True)
    for name, sector, offset, length in found:
        if not inside(destination, name + '.snr'):
            sys.exit('refusing to write outside %s: %r' % (destination, name))
        with open(os.path.join(destination, name + '.snr'), 'wb') as out:
            out.write(read_at(image, sector, offset, length))
    print('%d relocatable modules (SNR2, xff2) extracted to %s' % (len(found), destination))


def main():
    sys.stdout.reconfigure(newline='\n')
    parser = argparse.ArgumentParser(description='Read files from PS2 CD and DVD images, including both layers of a DVD9.')
    parser.add_argument('command', choices=['info', 'list', 'extract', 'modules'])
    parser.add_argument('image')
    parser.add_argument('destination', nargs='?')
    parser.add_argument('patterns', nargs='*', help='paths or globs to extract, everything below --limit if omitted')
    parser.add_argument('--limit', type=int, default=64, help='MiB; larger files stay in the image unless named')
    args = parser.parse_args()
    image = Image(args.image)
    if args.command == 'info':
        info(image)
    elif args.command == 'list':
        listing(image)
    elif args.command == 'modules':
        if not args.destination:
            parser.error('modules needs a destination folder')
        modules(image, args.destination)
    else:
        if not args.destination:
            parser.error('extract needs a destination folder')
        extract(image, args.destination, args.patterns, args.limit << 20)


if __name__ == '__main__':
    main()
