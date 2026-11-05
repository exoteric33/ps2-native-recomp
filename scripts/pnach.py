#!/usr/bin/env python3
import collections
import json
import os
import re
import struct
import sys
import textwrap

USAGE = '''usage: pnach.py crc <elf>
       pnach.py apply <elf> <patched elf> <runtime list> [--section NAME]... [--crc HEX] <pnach>...
       pnach.py fetch <disc image or elf> [--out DIR] [--json]   download the game's patch file from PCSX2's repository
       pnach.py sections <pnach> [<disc image or elf>] [--json]  list the sections of a patch file
(--json prints one line of JSON instead of the text, for the Windows app.)'''
# PS2_PNACH_REPO points the download at another folder of patch files (a mirror, or a local web server in a test); it
# is the web address of the folder, with the slash at the end.
REPO_FILES = os.environ.get('PS2_PNACH_REPO') or 'https://raw.githubusercontent.com/PCSX2/pcsx2_patches/main/patches/'
REPO_LIST = 'https://api.github.com/repos/PCSX2/pcsx2_patches/git/trees/main:patches'
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = {'byte': 1, 'short': 2, 'word': 4, 'double': 8, 'beshort': 2, 'beword': 4, 'bedouble': 8}
EXTENDED = {0: 1, 1: 2, 2: 4}
PAD = 16
RAM = 0x2000000


def crc(data):
    value = 0
    for (word,) in struct.iter_unpack('<I', data[:len(data) & ~3]):
        value ^= word
    return value


def physical(address):
    # The runtime's mapping: kseg0 and kseg1, and the uncached mirrors at 2xxxxxxx and 3xxxxxxx, all land in RAM.
    if 0x80000000 <= address < 0xC0000000:
        return address & 0x1FFFFFFF
    if address >> 28 in (2, 3):
        return address & 0x0FFFFFFF
    return address


def segments(elf):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    for i in range(phnum):
        kind, offset, vaddr, _, filesz, memsz = struct.unpack_from('<6I', elf, phoff + i * phentsize)
        if kind == 1:
            yield vaddr & 0x1FFFFFFF, offset, filesz, memsz


def grow(elf, patches):
    # Code written into the zero-filled end (bss) of an executable segment must be in the file too, or the translator
    # sees zeros there. The segment moves to the end of the file with its contents reaching to the last patch.
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    grown = 0
    for i in range(phnum):
        at = phoff + i * phentsize
        kind, offset, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from('<8I', elf, at)
        base = vaddr & 0x1FFFFFFF
        end = max([address + size for place, address, size, value in patches if base + filesz <= address and address + size <= base + memsz] or [0])
        if kind != 1 or not flags & 1 or not end:
            continue
        data = bytes(elf[offset:offset + filesz]) + bytes(end - base - filesz)
        elf.extend(bytes(-len(elf) % 16))
        struct.pack_into('<I', elf, at + 4, len(elf))
        struct.pack_into('<I', elf, at + 16, len(data))
        elf.extend(data)
        grown += 1
    return grown


def caves(patches, loads):
    image = {}
    for place, address, size, value in patches:
        if any(vaddr <= address < vaddr + memsz or vaddr < address + size <= vaddr + memsz for vaddr, _, _, memsz in loads):
            continue
        for k, byte in enumerate(value.to_bytes(size, 'little')):
            image[address + k] = byte
    blocks = []
    for address in sorted(image):
        if blocks and address <= blocks[-1][0] + len(blocks[-1][1]) + PAD:
            start, data = blocks[-1]
            data.extend(bytes(address - start - len(data)))
            data.append(image[address])
        else:
            blocks.append((address, bytearray([image[address]])))
    return [(start, bytes(data) + bytes(PAD)) for start, data in blocks]


def add_segments(elf, blocks):
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    table = bytearray(elf[phoff:phoff + phentsize * phnum])
    for address, data in blocks:
        elf.extend(bytes(-len(elf) % 16))
        offset = len(elf)
        elf.extend(data)
        table += struct.pack('<8I', 1, offset, address, address, len(data), len(data), 7, 16) + bytes(phentsize - 32)
    elf.extend(bytes(-len(elf) % 4))
    struct.pack_into('<I', elf, 28, len(elf))
    struct.pack_into('<H', elf, 44, phnum + len(blocks))
    elf.extend(table)


def parse(path, wanted, sections, skipped, guarded=None):
    """The patches of one file as (place, address, size, value). With `guarded` (a list) the lines that a 16-bit
    comparison (extended codes E, test "==" or "!=" against a variable) guards go there instead, as (position in the
    patches, place, address, size, value, conditions) with conditions as (address, equal, value); resolve() decides what
    they do. Without it they apply always, as before."""
    patches = []
    stack = []  # [lines still guarded, the conditions that guard them]
    section = None
    skip = 0
    pointer = False
    for number, raw in enumerate(open(path, encoding='utf-8-sig', errors='replace'), 1):
        line = raw.split('//', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip()
            sections.setdefault(section, {'used': section in wanted, 'aspect': None})
            skip = 0
            pointer = False
            continue
        if section is not None and section not in wanted:
            continue
        if line.lower().startswith('gsaspectratio=') and section is not None:
            sections[section]['aspect'] = line.split('=', 1)[1].strip()
        if not line.lower().startswith('patch='):
            continue
        fields = [f.strip() for f in line[6:].split(',')]
        if len(fields) != 5:
            skipped.append('%s:%d malformed' % (path, number))
            continue
        place, cpu, address, kind, value = fields
        kind = kind.lower()
        if cpu.upper() != 'EE':
            skipped.append('%s:%d %s patch' % (path, number, cpu))
            continue
        try:
            place, address = int(place), int(address, 16)
            data = bytes.fromhex(value) if kind == 'bytes' else None
            value = 0 if data else int(value, 16)
        except ValueError:
            skipped.append('%s:%d malformed' % (path, number))
            continue
        # The lines that belong to an extended code left out below are not patches of their own.
        if pointer:
            pointer = False
            skip = (address & 0xFFFF or 1) // 2
            continue
        if skip:
            skip -= 1
            continue
        conds = ()
        for entry in stack:
            if entry[0] > 0:
                entry[0] -= 1
                conds = entry[1]
        stack = [entry for entry in stack if entry[0] > 0]
        if kind == 'extended':
            code = address >> 28
            size = EXTENDED.get(code)
            if not size:
                if guarded is not None and code == 0xE and (address >> 24) & 0xF == 0 and value >> 28 in (0, 1):
                    stack.append([(address >> 16) & 0xFF, conds + ((physical(value & 0x0FFFFFFF), value >> 28 == 0, address & 0xFFFF),)])
                elif code in (0xD, 0xE):
                    lines = (address >> 16 if code == 0xE else value >> 24) & 0xFF
                    skipped.append('%s:%d conditional code %X, the %d lines it guards apply without the condition' % (path, number, code, lines))
                else:
                    skipped.append('%s:%d extended code type %X' % (path, number, code))
                    pointer = code == 6
                    skip = int(code in (4, 5) or (code == 3 and address >> 16 in (0x3040, 0x3050)))
                continue
            address &= 0x0FFFFFFF
        elif kind == 'bytes':
            size = 1
        elif kind in SIZES:
            size = SIZES[kind]
        else:
            skipped.append('%s:%d %s patch' % (path, number, kind))
            continue
        value &= (1 << 8 * size) - 1
        if kind.startswith('be'):
            value = int.from_bytes(value.to_bytes(size, 'big'), 'little')
        pieces = [(address + k, byte) for k, byte in enumerate(data)] if data else [(address, value)]
        for at, piece in pieces:
            at = physical(at)
            if at + size > RAM:
                skipped.append('%s:%d address %08X is outside the 32 MB of RAM' % (path, number, at))
                break
            if conds and guarded is not None:
                guarded.append((len(patches), place, at, size, piece, conds))
            else:
                patches.append((place, at, size, piece))
    return patches


class Problem(Exception):
    """Something went wrong that is not the game's fault: no internet, an unreadable image."""


class Missing(Exception):
    """The repository has no patch file for this game."""


def read_program(path):
    """(serial or None, program name, program bytes) of a disc image, or of a bare ELF. Only the file system's
    directories and the program are read, so an 8.5 GB dual layer image costs a fraction of a second."""
    if not os.path.isfile(path):
        raise Problem('%s does not exist (or is not a file)' % path)
    with open(path, 'rb') as f:
        head = f.read(4)
    if head == b'\x7fELF':
        with open(path, 'rb') as f:
            return None, os.path.basename(path), f.read()
    import importlib.util
    spec = importlib.util.spec_from_file_location('ps2disc', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disc.py'))
    disc = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(disc)
    not_iso = '%s is not a PS2 disc image that can be read (an .iso is needed; unpack a .zip or .7z first). A program file (ELF) works too.' % path
    try:
        image = disc.Image(path)
        entry = image.find('SYSTEM.CNF')
        if not entry:
            raise Problem('%s has no SYSTEM.CNF: it is not a PS2 game disc image' % path)
        image.file.seek(entry[2] * disc.SECTOR)
        cnf = image.file.read(min(entry[3], 1 << 16)).decode('ascii', 'replace').replace('\r', '\n').lstrip('\ufffd')  # CR alone ends a line; a BOM is 3 bytes
        boot = re.search(r'^\s*BOOT2\s*=\s*(\S+)', cnf, re.M | re.I)
        if not boot:
            raise Problem('SYSTEM.CNF of %s has no BOOT2 line: it is not a PS2 game disc image' % path)
        name = re.sub(r';\d+$', '', re.sub(r'^\w+:', '', boot.group(1))).replace('\\', '/').strip('/')
        program = image.find(name)
        if not program:
            raise Problem('SYSTEM.CNF names %s as the program, but it is not on the disc' % boot.group(1))
        if program[3] > 64 << 20:
            raise Problem('the program %s is %d MiB: that is not a PS2 program' % (name, program[3] >> 20))
        image.file.seek(program[2] * disc.SECTOR)
        data = image.file.read(program[3])
    except SystemExit:
        raise Problem(not_iso)
    except (struct.error, IndexError, OSError):
        raise Problem(not_iso)
    base = name.rsplit('/', 1)[-1]
    found = re.fullmatch(r'([A-Za-z]{4})_(\d{3})\.(\d{2})', base)
    return ('%s-%s%s' % tuple(found.groups())).upper() if found else None, base, data


def get(url):
    """The bytes at url, or None when the server says 404."""
    import http.client
    import urllib.error
    import urllib.request
    request = urllib.request.Request(url, headers={'User-Agent': 'ps2-native-recomp pnach.py', 'Accept': 'application/vnd.github+json'})
    host = url.split('/')[2]
    try:
        with urllib.request.urlopen(request, timeout=30) as answer:
            data = answer.read(8 << 20)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        if error.code in (403, 429):
            raise Problem('%s says "too many requests" (HTTP %d): GitHub limits how often a PC may ask. Try again in an hour.' % (host, error.code))
        raise Problem('%s answered with an error: HTTP %d %s' % (host, error.code, error.reason))
    except (urllib.error.URLError, http.client.HTTPException, OSError) as error:
        raise Problem('cannot reach %s: %s. Is this PC online? The patch files are downloaded from github.com/PCSX2/pcsx2_patches. '
                      'Connect to the internet and run the same command again.' % (host, getattr(error, 'reason', error)))
    return data


def repository_files():
    """The names of all patch files in PCSX2's repository."""
    import json
    data = get(REPO_LIST)
    if data is None:
        raise Problem('the file list of the patch repository is not there any more')
    try:
        return [e['path'] for e in json.loads(data)['tree'] if e.get('type') == 'blob' and e['path'].endswith('.pnach')]
    except (ValueError, KeyError, TypeError):
        raise Problem('the file list of the patch repository could not be read')


def stem_crc(name):
    found = re.fullmatch(r'(?:[A-Z]{4}-\d{5}_)?([0-9A-Fa-f]{1,8})\.pnach', name)
    return int(found.group(1), 16) if found else None


def find_patch(serial, crc):
    """(file name, bytes, note) of the program's patch file. Names tried first: <serial>_<CRC>.pnach, then <CRC>.pnach
    (a few files in the repository leave out the leading zero of the CRC). Missing says why there is none."""
    import urllib.parse
    names = []
    for prefix in ([serial + '_'] if serial else []) + ['']:
        for digits in ('%08X' % crc, '%X' % crc):
            if prefix + digits + '.pnach' not in names:
                names.append(prefix + digits + '.pnach')
    for name in names:
        data = get(REPO_FILES + urllib.parse.quote(name))
        if data is not None:
            return name, data, ''
    who = 'the program with CRC %08X%s' % (crc, ' (serial %s)' % serial if serial else '')
    try:
        files = repository_files()
    except Problem as error:
        raise Missing('PCSX2\'s repository has no file named %s for %s. (Whether it has one under another name could not be checked: %s)'
                      % (' or '.join(names), who, error))
    same_crc = [n for n in files if stem_crc(n) == crc]
    if same_crc:
        data = get(REPO_FILES + urllib.parse.quote(same_crc[0]))
        if data is not None:
            listed = same_crc[0].split('_')[0]
            return same_crc[0], data, ('the file is listed under another serial than the disc says (%s)' % listed if serial else
                                       'the program alone has no serial; the repository lists the file under %s' % listed) + ', but the program is the same, so it fits'
    other = [n for n in files if serial and n.startswith(serial + '_')]
    if other:
        raise Missing('PCSX2 has patches for %s, but only for another version of it (CRC %s). Yours has CRC %08X. Addresses differ between versions, so those '
                      'patches would not fit. Is this image another release (other region, re-release) or a changed copy?'
                      % (serial, ', '.join(n[len(serial) + 1:-6] for n in other), crc))
    raise Missing('PCSX2 has no patch file for %s. Not every game has one. (Looked for %s, and in the list of all %d files.)'
                  % (who, ' or '.join(names), len(files)))


def blank():
    return dict(description='', author='', aspect=None, lines=0, title='')


def scan(path):
    """The sections of a patch file in order: name -> title, description, author, screen shape and number of patch lines.
    None stands for the lines outside any section, which always apply."""
    found = {None: blank()}
    section = None
    for raw in open(path, encoding='utf-8-sig', errors='replace'):
        line = raw.split('//', 1)[0].strip()
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1].strip()
            found.setdefault(section, blank())
            continue
        key, _, value = raw.strip().partition('=')
        key, info = key.strip().lower(), found[section]
        if key in ('description', 'comment') and not info['description']:
            info['description'] = value.strip()
        elif key == 'author' and not info['author']:
            info['author'] = value.strip()
        elif key == 'gametitle' and not info['title']:
            info['title'] = value.strip()
        elif key == 'gsaspectratio':
            info['aspect'] = value.split('//')[0].strip()
        elif line.lower().startswith('patch='):
            info['lines'] += 1
    return found


def reasons_of(skipped):
    return collections.Counter('conditional code (not checked here; port.sh follows a simple test of a variable the file sets itself or of one instruction, the rest applies always)' if s.split(' ', 1)[1].startswith('conditional code')
                               else re.sub(r'^address [0-9A-F]{8}', 'address', s.split(' ', 1)[1]) for s in skipped)


def analyse(path, loads=None):
    """The sections of a patch file with what `describe` and `--json` say about each: (found, rows). A row is (name, info,
    the reasons patch lines cannot be used and how many, where the patches land (None without the program's segments))."""
    found = scan(path)
    top_skipped = []
    top = collections.Counter(parse(path, set(), {}, top_skipped))
    rows = []
    for name, info in found.items():
        if name is None:
            patches, skipped = top, top_skipped
        else:
            skipped = []
            patches = collections.Counter(parse(path, {name}, {}, skipped))
            patches.subtract(top)
            patches, skipped = +patches, [s for s in skipped if s not in top_skipped]
        if name is None and not info['lines']:
            continue
        where = None
        if loads is not None and patches:
            where = collections.Counter('file' if any(v <= a and a + size <= v + filesz for v, _, filesz, _ in loads) else
                                        'bss' if any(v <= a < v + memsz for v, _, _, memsz in loads) else 'outside'
                                        for (place, a, size, value), n in patches.items() for _ in range(n))
        rows.append((name, info, reasons_of(skipped), where))
    return found, rows


def describe(path, loads=None):
    """Print the sections of a patch file; with the program's segments also where the patches land. Returns the sections."""
    found, rows = analyse(path, loads)
    if found[None]['title']:
        print('the file says: %s' % found[None]['title'])
    for name, info, reasons, where in rows:
        notes = []
        if info['aspect']:
            notes.append('meant for a %s screen' % info['aspect'])
        if name is None:
            notes.append('always applied')
        notes.append('%d patch line%s' % (info['lines'], '' if info['lines'] == 1 else 's'))
        if not reasons:
            notes.append('all can be used here')
        else:
            notes.append('%d can be used here; cannot be used: %s' % (info['lines'] - sum(reasons.values()), '; '.join('%s (%d)' % (r, n) for r, n in reasons.items())))
        if where is not None:
            notes.append('writes: ' + ', '.join(
                ['%d into the program file' % where['file']] * bool(where['file']) +
                ['%d into its zero-filled end (bss), set while the game runs' % where['bss']] * bool(where['bss']) +
                ['%d outside the program (new code, or code the game loads later: such a patch may show no effect)' % where['outside']] * bool(where['outside'])))
        print('  [%s]' % name if name is not None else '  (outside any section)')
        print(textwrap.fill(' | '.join(notes) + (' | by ' + info['author'] if info['author'] else ''), 118, initial_indent='      ', subsequent_indent='      '))
        if info['description']:
            print(textwrap.fill(info['description'], 118, initial_indent='      ', subsequent_indent='      '))
    return found


def is_wide(name, info):
    return bool(name) and (info['aspect'] == '16:9' or bool(re.search(r'16\s*[:/x]\s*9|widescreen', name, re.I)))


def problem_kind(reason):
    if reason.startswith('conditional code'):
        return 'conditional'
    if re.match(r'address .*outside', reason):
        return 'memory'
    if reason.startswith('extended code type'):
        return 'extended'
    if reason == 'malformed':
        return 'malformed'
    return 'unsupported'


def sections_data(found, rows):
    """The rows of `analyse` as plain data for --json: {'title', 'always': the lines outside any section or None, 'sections'}.
    A section has name, author, description, aspect ('16:9' or ''), widescreen (looks like a 16:9 set), lines, usable
    (lines this project can use), problems ([{kind, text, count}]; kind is conditional, memory, extended, malformed or
    unsupported) and, when the program was given, writes ({file, bss, outside}: where the patches land)."""
    always, sections = None, []
    for name, info, reasons, where in rows:
        record = {'name': name or '', 'author': info['author'], 'description': info['description'], 'aspect': info['aspect'] or '',
                  'widescreen': is_wide(name, info), 'lines': info['lines'], 'usable': info['lines'] - sum(reasons.values()),
                  'problems': [{'kind': problem_kind(r), 'text': r, 'count': n} for r, n in reasons.items()]}
        if where is not None:
            record['writes'] = {'file': where['file'], 'bss': where['bss'], 'outside': where['outside']}
        if name is None:
            always = record
        else:
            sections.append(record)
    return {'title': found[None]['title'], 'always': always, 'sections': sections}


def shown(path):
    full = os.path.abspath(path)
    try:
        inside = os.path.commonpath([full, ROOT]) == ROOT
    except ValueError:
        inside = False
    text = (os.path.relpath(full, ROOT) if inside else full).replace('\\', '/')
    return '"%s"' % text if re.search(r'[\s&()]', text) else text


def suggest(found, options, image=None):
    wide = [n for n, i in found.items() if is_wide(n, i)]
    choose = ' --pnach-section "%s"' % wide[0] if wide else ''
    print()
    if wide:
        print('To try the wide screen set%s:' % (' (the first of %d that look like it)' % len(wide) if len(wide) > 1 else ''))
    else:
        print('No section of this file says 16:9. Pick sections with --pnach-section "NAME" (repeat it for several); the file as a whole is:')
    if image:
        name = re.sub(r'[^A-Za-z0-9._-]', '_', os.path.splitext(os.path.basename(image))[0])[:61].lstrip('._-') or 'game'
        print('  python scripts/report.py %s %s %s%s' % (name, shown(image), options, choose))
        print('  scripts/port.sh %s %s --native-only %s%s' % (name, shown(image), options, choose))
    else:
        print('  %s%s' % (options, choose))
    if found[None]['lines']:
        print('(the lines outside any section apply in every case)')


def usage(text):
    print(text, file=sys.stderr)
    sys.exit(2)


def fetch_command(args, as_json=False):
    out, rest = os.path.join(ROOT, 'isos', 'patches'), []
    while args:
        if args[0] == '--out' and len(args) > 1:
            out = args[1]
            args = args[2:]
        else:
            rest.append(args.pop(0))
    if len(rest) != 1:
        usage('usage: pnach.py fetch <disc image or elf> [--out DIR] [--json]\n'
              'Downloads the game\'s patch file from PCSX2\'s repository (default folder isos/patches, which git ignores).')
    serial, program, data = read_program(rest[0])
    value = crc(data)
    if not as_json:
        print('image:     %s' % shown(rest[0]))
        print('program:   %s%s' % (program, ', serial ' + serial if serial else ' (the name is no serial, only the CRC is used)'))
        print('CRC:       %08X (the number PCSX2 puts at the end of a patch file\'s name)' % value)
    name, patch, note = find_patch(serial, value)
    os.makedirs(out, exist_ok=True)
    target = os.path.join(out, name)
    same = os.path.isfile(target) and open(target, 'rb').read() == patch
    with open(target, 'wb') as f:
        f.write(patch)
    if as_json:
        found, rows = analyse(target, list(segments(data)))
        report = {'status': 'ok', 'serial': serial or '', 'program': program, 'crc': '%08X' % value, 'file': os.path.abspath(target),
                  'name': name, 'bytes': len(patch), 'same': same, 'note': note}
        report.update(sections_data(found, rows))
        print(json.dumps(report))
        return
    print('patch:     %s (%d bytes), %s %s' % (name, len(patch), 'already there, the same:' if same else 'saved as', shown(target)))
    if note:
        print('note:      ' + note)
    print()
    print('Sections (PCSX2 shows them as tick boxes):')
    found = describe(target, list(segments(data)))
    suggest(found, '--pnach ' + shown(target), rest[0])


def sections_command(args, as_json=False):
    if len(args) not in (1, 2):
        usage('usage: pnach.py sections <pnach> [<disc image or elf>] [--json]\n'
              'Lists the sections of a patch file. With the game\'s image it also says where the patches land in the game\'s program.')
    if not os.path.isfile(args[0]):
        raise Problem('%s does not exist (or is not a file)' % args[0])
    loads = None
    warning = ''
    if len(args) == 2:
        serial, program, data = read_program(args[1])
        loads = list(segments(data))
        made = re.search(r'(?:^|_)([0-9A-Fa-f]{8})\.pnach$', os.path.basename(args[0]))
        if made and made.group(1).upper() != '%08X' % crc(data):
            warning = 'warning: %s is made for the game with CRC %s, this one has %08X' % (os.path.basename(args[0]), made.group(1).upper(), crc(data))
            if not as_json:
                print(warning)
    if as_json:
        found, rows = analyse(args[0], loads)
        report = {'status': 'ok', 'file': os.path.abspath(args[0]), 'warning': warning}
        report.update(sections_data(found, rows))
        print(json.dumps(report))
        return
    found = describe(args[0], loads)
    suggest(found, '--pnach ' + shown(args[0]), args[1] if len(args) == 2 else None)


def plain(command, args):
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(newline='\n', errors='replace')
        except (AttributeError, ValueError):
            pass
    as_json = '--json' in args
    if as_json:
        args = [a for a in args if a != '--json']
    try:
        command(args, as_json)
    except Missing as error:
        print('pnach: ' + str(error), file=sys.stderr)
        if as_json:
            print(json.dumps({'status': 'none', 'message': str(error)}))
        sys.exit(1)
    except Problem as error:
        print('pnach: ' + str(error), file=sys.stderr)
        if as_json:
            print(json.dumps({'status': 'error', 'message': str(error)}))
        sys.exit(2)


def resolve(patches, guarded, notes):
    """Decides the lines parse() held back because a variable guards them. PCSX2 checks the variable every frame; here
    the program is translated once, so: a variable that the same file sets to a fixed value (an on/off switch the patch
    writes itself) is known, and the lines it guards apply or not. A condition on a game variable stays open: when it
    chooses between two instruction words at one address (a constant in the code that differs between the menu and the
    game), the address becomes a toggle: the ELF gets the last word, and the translation gets the other one for as long
    as the variable has its value (eerecomp --toggles, written to toggles.txt). Anything else applies without the
    condition, as before. Returns the patches and the toggles [(address, word, variable address, value)]."""
    known = {}
    for place, address, size, value in patches:
        for k, byte in enumerate(value.to_bytes(size, 'little')):
            known[address + k] = byte

    def state(condition):
        address, equal, value = condition
        if address in known or address + 1 in known:  # a byte the file does not write is taken as 0, as the patch's author did
            return ((known.get(address, 0) | known.get(address + 1, 0) << 8) == value) == equal
        return None

    inserted, open_ones = {}, {}
    applied = dropped = 0
    for entry in guarded:
        position, place, address, size, value, conds = entry
        states = [state(c) for c in conds]
        if False in states:
            dropped += 1
            continue
        unknown = [c for c, st in zip(conds, states) if st is None]
        if unknown:
            open_ones.setdefault(address, []).append((entry, unknown))
        else:
            applied += 1
            inserted.setdefault(position, []).append((place, address, size, value))
    toggles = []
    for address, group in sorted(open_ones.items()):
        simple = all(len(u) == 1 and u[0][1] and e[3] == 4 for e, u in group) and len({u[0][0] for e, u in group}) == 1
        words = [e[4] for e, u in group]
        if simple and len(set(words)) > 1:
            last, first = group[-1][0], next(e for e, u in group if e[4] != group[-1][0][4])
            toggles.append((address, first[4], group[0][1][0][0], next(u[0][2] for e, u in group if e is first)))
            group = [(last, None)]
            applied += 1
            notes.append('toggled %08X: %08X while the variable at %08X is %d, otherwise %08X' % (address, toggles[-1][1], toggles[-1][2], toggles[-1][3], last[4]))
        else:
            notes.append('%d lines at %08X depend on a game variable that cannot be followed: they apply without the condition' % (len(group), address))
        for entry, unknown in group:
            applied += 1
            inserted.setdefault(entry[0], []).append(entry[1:5])
    result = []
    for position in range(len(patches) + 1):
        result += inserted.get(position, [])
        if position < len(patches):
            result.append(patches[position])
    if guarded:
        notes.append('conditional lines: %d of %d apply (%d of them switched at run time), %d are dropped because the patch file itself sets their condition the other way' % (applied, len(guarded), len(toggles), dropped))
    return result, toggles


def main():
    if len(sys.argv) > 1 and sys.argv[1] in ('fetch', 'sections'):
        return plain(fetch_command if sys.argv[1] == 'fetch' else sections_command, sys.argv[2:])
    if len(sys.argv) == 3 and sys.argv[1] == 'crc':
        print('%08X' % crc(open(sys.argv[2], 'rb').read()))
        return
    if len(sys.argv) < 6 or sys.argv[1] != 'apply':
        sys.exit(USAGE)
    elf = bytearray(open(sys.argv[2], 'rb').read())
    args, wanted, files, game = sys.argv[5:], set(), [], '%08X' % crc(elf)
    while args:
        if args[0] == '--section' and len(args) > 1:
            wanted.add(args[1])
            args = args[2:]
        elif args[0] == '--crc' and len(args) > 1:
            game = args[1].upper()
            args = args[2:]
        else:
            files.append(args.pop(0))
    skipped, patches, sections, guarded, notes = [], [], {}, [], []
    for path in files:
        held = []
        part = parse(path, wanted, sections, skipped, held)
        guarded += [(len(patches) + h[0],) + h[1:] for h in held]
        patches += part
    patches, toggles = resolve(patches, guarded, notes)
    in_bss = grow(elf, patches)
    loads = list(segments(elf))
    in_elf = zeroed = 0
    for place, address, size, value in patches:
        for vaddr, offset, filesz, memsz in loads:
            if vaddr <= address and address + size <= vaddr + filesz:
                at = offset + address - vaddr
                elf[at:at + size] = value.to_bytes(size, 'little')
                in_elf += 1
                break
            if vaddr <= address < vaddr + memsz:
                zeroed += 1
                break
    blocks = caves(patches, loads)
    if blocks:
        add_segments(elf, blocks)
    open(sys.argv[3], 'wb').write(elf)
    with open(sys.argv[4], 'w', newline='\n') as out:
        for place, address, size, value in patches:
            out.write('%d %08X %d %X\n' % (place, address, size, value))
    if toggles:
        with open(os.path.join(os.path.dirname(os.path.abspath(sys.argv[4])), 'toggles.txt'), 'w', newline='\n') as out:
            for address, word, cond, value in toggles:
                out.write('%08X %08X %08X %X\n' % (address, word, cond, value))
    print('%d patches, %d of them in the executable' % (len(patches), in_elf))
    for note in notes:
        print(note)
    if in_bss:
        print('the executable grew to hold patches in its zero-filled end (bss), so they are translated with it')
    if zeroed:
        print('%d patches are in zero-filled memory of the executable (bss): they apply at run time only, code there is not translated' % zeroed)
    for address, data in blocks:
        print('new code at %08X..%08X outside the executable joins it as a segment' % (address, address + len(data) - PAD))
    for name, info in sections.items():
        print('%s [%s]%s' % ('applied' if info['used'] else 'left out', name, ', meant for a %s screen' % info['aspect'] if info['aspect'] else ''))
    for name in sorted(wanted - set(sections)):
        print('no section named [%s]' % name)
    for path in files:
        made = re.search(r'(?:^|_)([0-9A-Fa-f]{8})\.pnach$', os.path.basename(path))
        if made and made.group(1).upper() != game:
            print('warning: %s is made for the game with CRC %s, this one has %s' % (os.path.basename(path), made.group(1).upper(), game))
    for reason in skipped:
        print('skipped', reason)


if __name__ == '__main__':
    main()
