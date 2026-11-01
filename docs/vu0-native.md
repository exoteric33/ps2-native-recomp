# Recorded VU0 microprogram compilation (draft)

The shared vector code generator can compile recorded VU0 entries into the generated C++ of any game project. Selection uses reachable microcode bytes and entry addresses, with separate per-unit registries and code-generation caches. No game identity, executable address list or title-specific program key is used.

VU0 uses the reference interpreter's truncating arithmetic, 4 KiB code/data masks and final MAC/status flags visible to the EE. VU1 retains its existing vector arithmetic. VU0 checks host-stop requests every 256 instructions and has the same one-million-instruction budget as the reference interpreter. Unknown entries and entries with unsupported lower instructions, register jumps, XGKICK or unsupported pipeline plans execute entirely in the exact interpreter.

VU0 `XTOP` returns zero and `XITOP` reads the current VIF0 ITOP, matching the reference interpreter. VU1 continues to read its own latched TOP/ITOP; its no-argument helper functions and live-library ABI are retained.

## Record and build

`port.sh <name> <disc> ...` creates `games/<name>/vu0` and `vu1`, records unknown entries during native exploration, and compiles both sets. When playing a native window build, the host discovers either recording directory beside the extracted executable or disc image. Explicit `PS2_VU0_RECORD=<directory>` and `PS2_VU_RECORD=<directory>` settings take precedence independently. Use separate directories for the two units.

The recording format is `%016llX_%04X.bin` (key and byte entry address), with exactly 4096 bytes for VU0 or 16384 bytes for VU1. The CLI validates size/alignment and the generator verifies the reachable-code key:

```
vurecomp <vu1-directory> <generated-directory> --vu0-dir <vu0-directory>
vurecomp - <generated-directory> --vu0-dir <vu0-directory>
```

The original `vurecomp <vu1-directory> <generated-directory>` invocation remains supported. The existing `vu1_NNN.cpp` / `vu1_table.cpp` output filenames are retained for build-tool compatibility; functions and table entries identify their unit. AOT registration adds a unit field defaulting to VU1. The generator fingerprint is bumped, and changed embedded headers invalidate incompatible live-library caches.

## Differential validation

- `PS2_VU0_INTERPRET=1`: force reference interpretation for comparison runs.
- `PS2_VU0_VERIFY=1`: execute each compiled call and reference interpretation from identical initial state. Compare every architectural register field, including TPC, and all 4096 data bytes, excluding ABI padding. Keep the reference result and blacklist a mismatching key for the rest of the process.
- `PS2_VU0_STATS=1`: report compiled/interpreted counts every million calls. The final atexit report is available to hosts that exit normally; the window host uses `_Exit`.

Run `python3 scripts/test_vu0_codegen.py` with a C++20 compiler supporting `std::format` (`CXX` selects it). No disc, recorded game code, graphics device or game configuration is required. It reuses the synthetic instruction fixtures from `scripts/test_vu_codegen.py`: 150 random programs, pipeline cases, code/data wrapping, externally visible flags, zero destination masks, per-unit key isolation, code replacement, unknown-entry fallback, host stop and nonterminating code budgets. A negative control injects a wrong function and checks blacklisting, while padding-only changes must not reject a correct result. It also exercises recording/deduplication and the CLI with synthetic VU0-only, VU1-only, mixed-unit and malformed inputs.

`scripts/test_vu_codegen.py` retains the existing native VU1 and Wasm JIT regressions.

The VU0 suite also compiles the same synthetic `XTOP`/`XITOP` programs for both units. It compares full register/data state with the interpreter while varying VIF0 ITOP and VU1's latched TOP/ITOP independently, without changing the cached program. Cases cover VI0, conditional branches and addressed loads/stores; no game program or diagnostic memory dump is used.

## Draft boundaries

VU0 compilation is ahead of time. Native VU0 background compilation and the browser's VU0 recording/export UI are follow-up work; the generator rejects VU0 live-library requests. Broader game coverage and full Linux, Windows and WebAssembly build/playtest coverage are still needed. Generated game code, real microcode recordings, memory cards and game states are not part of this change.
