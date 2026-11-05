import argparse
import os
import re
import struct
import sys
import time

SECTOR = 2048
ROOT = 20
NAME = re.compile(r'[A-Z0-9_]{1,8}(\.[A-Z0-9_]{0,3})?')


def both(fmt, value):
    return struct.pack('<' + fmt, value) + struct.pack('>' + fmt, value)


def record(extent, size, name, directory, when):
    length = 33 + len(name)
    length += length % 2
    date = bytes([when.tm_year - 1900, when.tm_mon, when.tm_mday, when.tm_hour, when.tm_min, when.tm_sec, 0])
    flags = bytes([2 if directory else 0, 0, 0])
    return (bytes([length, 0]) + both('I', extent) + both('I', size) + date + flags + both('H', 1)
            + bytes([len(name)]) + name.ljust(length - 33, bytes(1)))


def build(boot, extras, cnf=None):
    when = time.gmtime()
    if cnf is None:  # tests give a SYSTEM.CNF of their own (tabs, lower case, a BOM): bytes, with %s where the name goes
        cnf = ('BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n' % os.path.basename(boot).upper()).encode('ascii')
    else:
        cnf = cnf.replace(b'%s', os.path.basename(boot).upper().encode('ascii'))
    files = {'SYSTEM.CNF': cnf}
    for path in [boot] + extras:
        name = os.path.basename(path).upper()
        if not NAME.fullmatch(name):
            sys.exit('%s: not an ISO 9660 level 1 name (8.3, A-Z 0-9 _)' % name)
        if name in files:
            sys.exit('%s: given twice' % name)
        with open(path, 'rb') as f:
            files[name] = f.read()

    directory = [record(ROOT, SECTOR, bytes([0]), True, when), record(ROOT, SECTOR, bytes([1]), True, when)]
    sector = ROOT + 1
    placed = []
    for name in sorted(files):
        directory.append(record(sector, len(files[name]), (name + ';1').encode('ascii'), False, when))
        placed.append((sector, files[name]))
        sector += max(1, (len(files[name]) + SECTOR - 1) // SECTOR)
    directory = b''.join(directory)
    if len(directory) > SECTOR:
        sys.exit('too many files for a one-sector root directory')

    # the path table holds the root only, once little-endian and once big-endian
    table = [bytes([1, 0]) + struct.pack(order + 'IH', ROOT, 1) + bytes(2) for order in '<>']
    stamp = time.strftime('%Y%m%d%H%M%S', when).encode('ascii') + b'00' + bytes(1)
    unset = b'0' * 16 + bytes(1)
    pvd = bytearray(SECTOR)
    pvd[0:8] = bytes([1]) + b'CD001' + bytes([1, 0])
    pvd[8:72] = b'PLAYSTATION'.ljust(32) + b'SMOKETEST'.ljust(32)
    pvd[80:88] = both('I', sector)
    pvd[120:132] = both('H', 1) + both('H', 1) + both('H', SECTOR)
    pvd[132:156] = both('I', len(table[0])) + struct.pack('<II', 18, 0) + struct.pack('>II', 19, 0)
    pvd[156:190] = record(ROOT, SECTOR, bytes([0]), True, when)
    pvd[190:813] = b' ' * 623
    pvd[813:881] = stamp + stamp + unset + unset
    pvd[881] = 1

    image = bytearray(sector * SECTOR)
    image[16 * SECTOR:17 * SECTOR] = pvd
    image[17 * SECTOR:17 * SECTOR + 7] = bytes([255]) + b'CD001' + bytes([1])
    image[18 * SECTOR:18 * SECTOR + len(table[0])] = table[0]
    image[19 * SECTOR:19 * SECTOR + len(table[1])] = table[1]
    image[ROOT * SECTOR:ROOT * SECTOR + len(directory)] = directory
    for start, data in placed:
        image[start * SECTOR:start * SECTOR + len(data)] = data
    return image


def main():
    parser = argparse.ArgumentParser(description='Write a small PS2 disc image: SYSTEM.CNF booting one program, plus any other files.')
    parser.add_argument('image', help='the .iso to write')
    parser.add_argument('boot', help='the program SYSTEM.CNF boots, named in 8.3')
    parser.add_argument('files', nargs='*', help='more files for the root folder, also named in 8.3')
    args = parser.parse_args()
    image = build(args.boot, args.files)
    with open(args.image, 'wb') as out:
        out.write(image)
    print('%s: %d sectors' % (args.image, len(image) // SECTOR))


if __name__ == '__main__':
    main()
