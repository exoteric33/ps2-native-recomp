#!/usr/bin/env python3
"""No PS2_* switch may be read before main() runs.

A tester sets switches through switches.txt in the game's folder: host/main.cpp reads that file at the start
of main() and puts its lines into the environment. A global variable initialised from the environment (std::getenv, os::env) is
filled when the program loads, before main(), so a switch from the file never reaches it. Read a switch on
first use (a function with a local static), or, for a plain flag a hot path tests, in ps2::read_switches(), which main() calls once
the file is in the environment.

This test scans runtime/ and host/ for the mistake, no build and no disc needed. It fails on the initialiser of a namespace-scope
variable that reads the environment (also through a lambda called there, or a function of the same file that reads it), and on a
constructor or default member initialiser that reads it when the same file has a global of that class. Run from anywhere:
python tests/switch_early.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDERS = ['runtime', 'host']
ENV = re.compile(r'\b(?:std::)?(?:getenv|secure_getenv|_wgetenv)\s*\(|\bos::env\s*\(|\bGetEnvironmentVariable[AW]?\s*\(')

# Cases that are fine for now, as (path, the variable's or class's name), each with its reason.
ALLOWED = {
    # PS2_BATCH_LOG: a fix that reads it on first use is on its way; take these out with it
    ('runtime/src/gs/gpu/gpu.cpp', 'g_batch_log'),
    ('runtime/src/gs/gpu/gpu.cpp', 'g_batch_log_frame'),
    # PS2_GPU_WGPU_TIME (a developer's timing switch): the same mistake, found by this test. It is fixed together with PS2_BATCH_LOG
    # above; take it out of here with that fix.
    ('runtime/src/gs/gpu/gpu.cpp', 'g_every'),
}

QUALIFIERS = r'(?:(?:static|const|inline|thread_local|extern|constinit|volatile)\s+)*'
NOT_TYPES = {'return', 'using', 'typedef', 'namespace', 'template', 'struct', 'class', 'enum', 'union', 'friend', 'delete', 'goto'}


def strip(text):
    """The source without comments and with string and character literals emptied, so braces and names in them do not count.
    Line breaks stay, so line numbers stay right."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif text.startswith('R"', i) and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            k = text.find('(', i + 2)
            delim = text[i + 2:k]
            j = text.find(')' + delim + '"', k)
            j = n if j < 0 else j + len(delim) + 2
            out.append('""' + '\n' * text.count('\n', i, j))
            i = j
        elif c in '"\'':
            if c == "'" and i > 0 and text[i - 1].isalnum():  # a digit separator (1'000)
                i += 1
                continue
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def preprocessor_blanked(text):
    """Preprocessor lines (with their continuations) as empty lines: a macro body's braces must not count."""
    out, cont = [], False
    for line in text.split('\n'):
        if cont or line.lstrip().startswith('#'):
            cont = line.rstrip().endswith('\\')
            out.append('')
        else:
            out.append(line)
    return '\n'.join(out)


def top_level_equals(s):
    """Is there an '=' at bracket depth 0 that assigns (not ==, !=, <=, >=, operator=)?"""
    depth = 0
    for i, c in enumerate(s):
        if c in '([':
            depth += 1
        elif c in ')]':
            depth -= 1
        elif c == '=' and depth == 0:
            before, after = s[i - 1] if i else '', s[i + 1] if i + 1 < len(s) else ''
            if before in '=!<>' or after == '=' or re.search(r'operator\s*$', s[:i]):
                continue
            return True
    return False


def last_name(s):
    m = re.search(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$', s)
    return m.group(1) if m else ''


def declared_name(statement):
    """The name a declaration declares: the last name before its top-level '=', '{' or '('."""
    depth = 0
    for i, c in enumerate(statement):
        if depth == 0 and c in '({' and not re.search(r'\boperator\s*$', statement[:i]):
            name = last_name(statement[:i])
            if name:
                return name
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == '=' and depth == 0 and top_level_equals(statement[:i + 1]):
            return last_name(statement[:i])
    return last_name(statement)


def instance_type(head):
    """The type of a namespace-scope variable declared by `head` (`Foo g_x`, `static ns::Foo g_x = ...`), or ''."""
    m = re.match(QUALIFIERS + r'(?:[A-Za-z_]\w*::)*([A-Za-z_]\w*)\s*(?:<[^;{}]*>)?\s*[&*]?\s*[A-Za-z_]\w*\s*(?:$|[={(\[])', head)
    return m.group(1) if m and m.group(1) not in NOT_TYPES else ''


def scan(path):
    """The findings of one file, as (line, what, name)."""
    text = preprocessor_blanked(strip(path.read_text(encoding='utf-8', errors='replace')))
    readers = set()       # the functions of this file whose body reads the environment (filled by the first walk)
    instances = set()     # the types of this file's namespace-scope variables
    classes = []          # (line, what, class): constructors and member initialisers that read the environment
    findings, seen = [], set()

    def report(line, what, name):
        if (line, name) not in seen:
            seen.add((line, name))
            findings.append((line, what, name))

    def walk(report):
        # the scope stack: ('ns', None), ('class', name), ('func', name) or ('init', the name of the variable it initialises)
        stack, statement, start_line, line, closed_class = [], '', 1, 1, None

        def first_line():
            return start_line + statement[:len(statement) - len(statement.lstrip())].count('\n')

        for i, c in enumerate(text):
            if c == '\n':
                line += 1
            if c == '{':
                head = statement.strip()
                at_ns = all(e[0] == 'ns' for e in stack)
                in_class = bool(stack) and stack[-1][0] == 'class'
                if re.search(r'\bnamespace\b[^;()]*$', head) or re.fullmatch(r'extern\s*""', head):
                    kind, name = 'ns', None
                elif re.search(r'\b(?:struct|class|union|enum)\b', head) and '(' not in head and not top_level_equals(head):
                    kind, name = 'class', last_name(re.sub(r'\s*(?:final\s*)?(?::[^:].*)?$', '', head)) or last_name(head)
                elif (at_ns or in_class) and (top_level_equals(head) or (
                        head and not re.search(r'\)\s*(?:const|noexcept|override|final|mutable|->[^{]*|\s)*$', head) and
                        re.search(r'[A-Za-z_]\w*\s*$', head) and not re.search(r'\b(?:else|do|try)\s*$', head))):
                    kind, name = 'init', declared_name(head)
                    if at_ns and instance_type(head):
                        instances.add(instance_type(head))
                else:
                    m = re.search(r'([~A-Za-z_][\w:~]*)\s*\(', head)
                    kind, name = 'func', (m.group(1).split('::')[-1] if m else '')
                stack.append((kind, name))
                statement, start_line = '', line
                continue
            if c == '}':
                closed_class = stack[-1][1] if stack and stack[-1][0] == 'class' and all(e[0] == 'ns' for e in stack[:-1]) else None
                if stack:
                    stack.pop()
                if not stack or stack[-1][0] != 'init':  # an initialiser's statement goes on to its ';', a function ends here
                    statement = ''
                start_line = line
                continue
            if c == ';':
                head = statement.strip()
                kinds = [e[0] for e in stack]
                if all(k == 'ns' for k in kinds):
                    if instance_type(head):
                        instances.add(instance_type(head))
                    if closed_class and re.fullmatch(r'[A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*', head):  # struct X { ... } g_x;
                        instances.add(closed_class)
                    if top_level_equals(head):
                        if ENV.search(head):
                            report(first_line(), 'namespace-scope variable', declared_name(head))
                        else:
                            hit = set(re.findall(r'([A-Za-z_]\w*)\s*\(', head.split('=', 1)[1])) & readers
                            if hit:
                                report(first_line(), 'namespace-scope variable (through %s)' % ', '.join(sorted(hit)), declared_name(head))
                elif kinds and kinds[-1] == 'class' and ENV.search(head) and (top_level_equals(head) or '{' in head):
                    classes.append((first_line(), 'default member initialiser', stack[-1][1]))
                closed_class = None
                statement, start_line = '', line
                continue
            statement += c
            if stack and ENV.match(text, i):
                kinds = [e[0] for e in stack]
                if 'func' in kinds:
                    fi = kinds.index('func')
                    readers.add(stack[fi][1])
                    above = stack[fi - 1] if fi else None
                    if above and above[0] == 'class' and stack[fi][1] == above[1]:  # a destructor runs at exit, after main(): fine
                        classes.append((line, 'constructor', above[1]))
                elif 'init' in kinds and all(k in ('ns', 'init') for k in kinds):
                    report(line, 'namespace-scope variable', stack[kinds.index('init')][1])
                elif 'init' in kinds and 'class' in kinds:
                    classes.append((line, 'default member initialiser', next(e[1] for e in stack if e[0] == 'class')))

    walk(lambda *a: None)  # the first walk only learns which functions read the environment
    instances.clear()
    classes.clear()
    walk(report)
    for line, what, name in classes:
        if name in instances:  # a function-local static of it is made on first use, after main(): fine
            report(line, what + ' of a global', name)
    return sorted(findings)


def main():
    bad, allowed_seen = [], set()
    for folder in FOLDERS:
        for path in sorted((ROOT / folder).rglob('*')):
            if path.suffix not in ('.cpp', '.hpp', '.h', '.cc', '.inl'):
                continue
            rel = path.relative_to(ROOT).as_posix()
            for line, what, name in scan(path):
                if (rel, name) in ALLOWED:
                    allowed_seen.add((rel, name))
                    continue
                bad.append('%s:%d: %s %s reads the environment before main()' % (rel, line, what, name))
    for rel, name in sorted(ALLOWED - allowed_seen):
        print('note: %s %s is allowed but no longer found; take it out of ALLOWED' % (rel, name))
    if bad:
        print('\n'.join(bad))
        print('FAIL: %d switch(es) read before main(); a switch from switches.txt never reaches them' % len(bad))
        return 1
    print('PASS: no switch is read before main() (allowed for now: %s)' % (', '.join(sorted(n for _, n in allowed_seen)) or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
