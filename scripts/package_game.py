#!/usr/bin/env python3
"""
Package a recompiled PS2 game into a standalone, ISO-free native distribution folder.
Auto-detects the boot ELF (e.g. SLES_xxx.xx) from SYSTEM.CNF if --elf is omitted.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from ps2_disc_toolkit.cli import main as toolkit_main  # noqa: E402


if __name__ == '__main__':
    toolkit_main(['package'] + sys.argv[1:])
