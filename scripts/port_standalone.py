#!/usr/bin/env python3
"""
Standalone wrapper for the end-to-end ISO-free PS2 porting pipeline:
  Disc Image (.iso, .bin/.cue, .cso, .chd) -> Virtual Disc Extraction -> Recompilation -> dist/<name>/
Delegates directly to tools/ps2_disc_toolkit/cli.py (subcommand: port).
"""

import os
import sys

TOOLS_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools')
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from ps2_disc_toolkit.cli import main as toolkit_main  # noqa: E402


if __name__ == '__main__':
    toolkit_main(['port'] + sys.argv[1:])
