# Start the native runner from the ISO

A runner already built for a game can read its boot ELF directly from that game's ISO:

```sh
build/native-mygame/ps2run --disc '/path/to/game.iso' --game-dir games/mygame --window --memcard '/path/to/play.ps2'
```

No extracted boot executable is needed at launch. The runner reads `SYSTEM.CNF`, finds its `BOOT2` entry through the existing ISO9660 index and reads that ELF into memory. Nothing is extracted to the host filesystem or embedded in the runner. The ISO remains open for the game's CD/DVD reads.

This does not turn a game-specific recompiled runner into a general emulator: the native game code still comes from the existing `port.sh` build. The build continues to extract its input ELF for translation. Its printed launch command uses ISO boot when the translated ELF is the original boot program.

An explicit ELF argument keeps the previous behavior:

```sh
build/native-mygame/ps2run '/path/to/program.elf' --disc '/path/to/game.iso' --window
```

Use this form when translation used an executable patched before translation, a special extracted core or a disc-free test program. `port.sh` continues to print that form for patched/special executables. Disc boot uses the original ELF on the ISO; it does not apply build-time executable patches to it.

`--game-dir DIR` selects an existing game folder independently of the image and runner locations. `port.sh` always includes it in the printed command, including when a Windows image on another drive cannot be hard-linked to `games/<name>/disc.iso`. The runner uses this folder for `vu1` recording, `vu1-live` and `ee-live` caches, and existing `entries.txt`/`overlays` build inputs. It also supplies the default location of states, reports, logs and `keys.ini`. `--quick-dir` and an explicit memory-card location keep their precedence for those user files; `PS2_VU_RECORD`, `PS2_VU_LIVE` and `PS2_EE_LIVE` can still select individual recording/cache folders. A missing game directory is an error, rather than a silent fallback to the ISO folder.

Without `--game-dir`, the runner looks for `vu1` or `entries.txt` beside the ELF (or its virtual ISO-boot path), one folder above it, then beside the ISO. When none is found, caches and user files default beside the program or ISO. This preserves standalone ISO-only launches; pass `--game-dir` when the image lives separately from the game's existing work folder.

Live EE translation uses the same in-memory ELF bytes as boot, including deferred analysis of the original program. Exact file lengths are preserved so the cache stamp matches external-file boot. Input recordings include `disc-program 1` and the selected `game-dir`; `scripts/replay.py` then boots from the ISO and reuses that folder if it still exists. Replay `--game-dir` overrides it after a move, while an explicit `--program` selects external-file boot.

Missing `SYSTEM.CNF`/`BOOT2`, missing executables, invalid ELF headers, oversized entries and short reads fail with a readable error. Host allocation limits are 64 KiB for the configuration and 128 MiB for the complete ELF, including sections that do not load into EE RAM. `BOOT2` lookup uses disc paths, never host paths, and supports ISO subdirectories and `;1` version suffixes. A relative BOOT2 path is normalized to `cdrom0:` before it is passed to the game.

## Disc-free checks

```sh
ninja -C build/ci ps2discinputtest
build/ci/ps2discinputtest
python3 tests/disc_boot.py --recompiler build/tools/eerecomp --runtime build/ci/libps2runtime.a --cxx clang++
```

The first test covers valid and malformed synthetic discs. The second builds a tiny recompiled program and ISO, removes the original ELF, forces live translation of a code window, checks warm-cache reuse across file/ISO boot, and replays a fresh recording. It also checks a separate game folder with spaces and Unicode, both live-cache locations, persistent entries/overlays, recording/replay and invalid directories. It uses no game data or GPU and is included in the Linux/macOS CI job.
