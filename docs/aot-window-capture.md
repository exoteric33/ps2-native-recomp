# Offline AOT window capture

The native runtime can save selected EE RAM windows at selected guest frames
for offline inspection and recompilation. This is disabled unless all three
environment variables below are set:

- `PS2_CAPTURE_AOT_DIR`: destination directory.
- `PS2_CAPTURE_AOT_FRAMES`: comma-separated decimal guest frame numbers.
- `PS2_CAPTURE_AOT_BASES`: comma-separated EE RAM base addresses, in decimal or
  `0x`-prefixed hexadecimal. Each base must be 16 KiB aligned and inside EE RAM.

At each requested frame, the runtime scans each selected window in 16 KiB
increments, skips empty windows, and writes the non-empty prefix up to 4 MiB as
`frame_<10-digit-frame>_<8-digit-base>.bin`. It writes a diagnostic line for
each saved window. The capture request does not change guest memory or execution.
Choose base addresses from the game and state under investigation; the runtime
does not assume addresses for a particular title.

For rendered frames, `ps2run` also accepts repeated `--frame-at <frame>`
arguments alongside `--frames-dir`. These add exact capture frames to the
existing `--frame-every <n>` schedule; pass `--frame-every 0` to capture only
the requested frames.

Example in PowerShell:

```powershell
$env:PS2_CAPTURE_AOT_DIR = "capture/windows"
$env:PS2_CAPTURE_AOT_FRAMES = "1200,2400"
$env:PS2_CAPTURE_AOT_BASES = "0x00100000,0x00200000"
build/native-mygame/ps2run.exe games/mygame/disc/BOOT.ELF --disc game.iso --frames 2500 --frames-dir capture/screens --frame-every 0 --frame-at 1200 --frame-at 2400
```

The environment variables select passive capture points only. They do not
compile the captured image or prove that a cached native library is correct.

## Translate a captured image offline

`eerecomp --live-image` sends a raw captured window through the same analysis
and C++ generation path used for live EE recompilation. Pass the image base and
a nonzero library stamp. Give one or more absolute guest entry addresses when
the capture contains known call targets. `--expect-name` can pin the input to
the `<base>_<CRC-32>` name used by the live cache, and `--dense-checks` asks the
generated library to compare the complete window before loading.

```powershell
build/tools/eerecomp.exe --live-image capture/windows/frame_0000001200_00100000.bin `
  --base 00100000 --stamp 0000000000000042 --entry 00100000 `
  --expect-name 00100000_12345678 --out capture/generated --dense-checks
```

The command reports accepted and rejected entries and fails if none of the
requested entries is code in the image. The resulting C++ still has to be
compiled, registered, and loaded by the matching runner. A successful
translation alone is not proof that the cache covers all runtime code or that
its entry checks match the game state.

Run `python scripts/test_live_image.py <path-to-eerecomp>` for a synthetic
smoke test of successful translation and rejection of a wrong image name. It
uses no disc, BIOS, saved game, or captured title data.
