#!/usr/bin/env python
"""Guard: a Windows ps2run.exe may import only SDL2.dll and DLLs that are part of Windows.

Run from the repository root:  python tests/win_imports.py <ps2run.exe> [more .exe/.dll files]

Why: a player's PC has no compiler folder on its PATH. A DLL that lives only in llvm-mingw's folder
(libwinpthread-1.dll, libc++.dll, libunwind.dll, ...) makes Windows stop the program before main()
with 0xC0000135 and no log. The build PC always has that folder on its PATH, so the problem never
shows there. This test reads the PE import tables (normal and delay-load) with no tool needed.

Allowed: SDL2.dll, the api-ms-win-* / ext-ms-win-* API sets, and a named list of DLLs that ship in
System32 of every Windows 10 and 11 (below). Anything else fails. Prints "win imports: N failed".
"""
import struct
import sys

# DLLs that exist in C:\Windows\System32 on every Windows 10/11 install (none of them comes from a compiler).
SYSTEM_DLLS = {
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll",
    "ole32.dll", "oleaut32.dll", "ws2_32.dll", "msvcrt.dll", "ucrtbase.dll", "bcrypt.dll", "userenv.dll",
    "imm32.dll", "winmm.dll", "setupapi.dll", "version.dll", "shlwapi.dll", "comdlg32.dll", "comctl32.dll",
    "crypt32.dll", "secur32.dll", "ncrypt.dll", "iphlpapi.dll", "cfgmgr32.dll", "bcryptprimitives.dll", "opengl32.dll", "synchronization.dll",
    "dwmapi.dll", "winspool.drv", "mswsock.dll", "rpcrt4.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "d3dcompiler_47.dll",
    "hid.dll", "xinput1_4.dll", "dbghelp.dll", "psapi.dll", "wininet.dll", "dinput8.dll", "dsound.dll",
    "ole32.dll", "combase.dll", "uxtheme.dll", "shcore.dll", "powrprof.dll", "wldap32.dll", "normaliz.dll",
}
EXTRA_OK = {"sdl2.dll"}
SYSTEM_PREFIXES = ("api-ms-win-", "ext-ms-win-")


def allowed(name):
    n = name.lower()
    return n in EXTRA_OK or n in SYSTEM_DLLS or n.startswith(SYSTEM_PREFIXES)


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def imported_dlls(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("no PE header")
    nsec, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    dd = opt + (112 if magic == 0x20B else 96)  # data directories
    sec = opt + optsize
    sections = []
    for i in range(nsec):
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, sec + 40 * i + 8)
        sections.append((va, max(vsize, rawsize), raw))

    def rva(r):
        for va, size, raw in sections:
            if va <= r < va + size:
                return r - va + raw
        raise ValueError("bad RVA %#x" % r)

    names = []
    # 1 = import table (20-byte entries, name RVA at +12); 13 = delay-load table (32-byte entries, name RVA at +4)
    for index, esize, name_at in ((1, 20, 12), (13, 32, 4)):
        table, size = struct.unpack_from("<II", data, dd + 8 * index)
        if not table:
            continue
        off = rva(table)
        while True:
            entry = data[off:off + esize]
            if len(entry) < esize or not any(entry):
                break
            name_rva, = struct.unpack_from("<I", entry, name_at)
            if name_rva:
                names.append(cstr(data, rva(name_rva)))
            off += esize
    return names


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    failed = 0
    for path in argv[1:]:
        try:
            dlls = imported_dlls(path)
        except (OSError, ValueError) as e:
            print("FAIL %s: %s" % (path, e))
            failed += 1
            continue
        bad = sorted({d for d in dlls if not allowed(d)}, key=str.lower)
        for d in bad:
            print("FAIL %s imports %s (not SDL2.dll and not part of Windows: a player's PC may not have it)" % (path, d))
        failed += len(bad)
        print("%s: %d DLLs imported, %d not allowed" % (path, len(set(d.lower() for d in dlls)), len(bad)))
    print("win imports: %d failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
