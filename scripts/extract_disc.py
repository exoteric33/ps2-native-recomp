#!/usr/bin/env python3
"""
Wrapper CLI around the independent PS2 Disc Extraction Toolkit (tools/ps2_disc_toolkit).
Supports inspect, extract, verify, and package across ISO-2048 (DVD-5/DVD-9),
BIN/CUE-2352 (CD-ROM Mode 1 / Mode 2 XA), and CSO compressed images.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))

from ps2_disc_toolkit.cli import main, extract_disc, verify_disc, inspect_disc  # noqa: E402,F401

if __name__ == '__main__':
    main()
