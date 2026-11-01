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
compile the captured image or prove that a cached native library is correct;
the separate recompiler/loader validation must establish that.
