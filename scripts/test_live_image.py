#!/usr/bin/env python3
"""Smoke-test the offline live-image translation command with synthetic EE code."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def run(command, expected_status):
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != expected_status:
        raise SystemExit(
            f"expected exit {expected_status}, got {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_live_image.py <path-to-eerecomp>")

    eerecomp = Path(sys.argv[1]).resolve()
    if not eerecomp.is_file():
        raise SystemExit(f"eerecomp executable not found: {eerecomp}")

    with tempfile.TemporaryDirectory(prefix="eerecomp-live-image-") as temporary:
        root = Path(temporary)
        image_path = root / "synthetic-ee-window.bin"
        image = bytearray(0x400)
        # addiu v0, zero, 1; jr ra; nop
        struct.pack_into("<III", image, 0, 0x24020001, 0x03E00008, 0)
        image_path.write_bytes(image)

        output = root / "generated"
        command = [
            str(eerecomp), "--live-image", str(image_path),
            "--base", "00100000", "--stamp", "42",
            "--entry", "00100000", "--out", str(output), "--dense-checks",
        ]
        result = run(command, 0)
        if "ok=1" not in result.stdout or "accepted=1" not in result.stdout:
            raise SystemExit(f"translation result was unexpected:\n{result.stdout}")
        if not any(path.suffix == ".cpp" for path in output.iterdir()):
            raise SystemExit("translation succeeded but wrote no C++ source")
        if not (output / "table.cpp").is_file():
            raise SystemExit("translation succeeded but wrote no table.cpp")

        wrong_name = run(command + ["--expect-name", "00100000_00000000"], 1)
        if "does not match expected window name" not in wrong_name.stderr:
            raise SystemExit(f"wrong-name rejection was unexpected:\n{wrong_name.stderr}")

        zero_stamp = command.copy()
        zero_stamp[zero_stamp.index("--stamp") + 1] = "0"
        invalid_stamp = run(zero_stamp, 2)
        if "usage:" not in invalid_stamp.stderr:
            raise SystemExit(f"zero stamp rejection was unexpected:\n{invalid_stamp.stderr}")

    print("offline live-image translation and input rejection checks passed")


if __name__ == "__main__":
    main()
