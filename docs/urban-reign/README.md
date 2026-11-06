# Urban Reign (Europe): playable

| | |
|---|---|
| Disc | Urban Reign (Europe) (En,Fr,De,Es,It), **SCES-53688**, boot program `SCES_536.88`, CRC `AE4BEBD3` |
| Status | plays well, not played through yet: menus, profiles, versus and team fights with knockouts, the pause and end-of-fight screens, the game's own saves, all five languages of the disc |
| Tested on | Windows 11, Ryzen 9 9900X, RTX 4080 SUPER (Direct3D 12), Xbox controller, 4x resolution, 480p, full speed |

Most of the work went into the tool, so other games get it too. What is specific to this game is one patch file,
[`AE4BEBD3.pnach`](AE4BEBD3.pnach), next to this page.

## Build and play

From the top of the repository (Git Bash on Windows, or Linux), with your own disc image:

```sh
bash scripts/port.sh urbanreign "/c/Games/Urban Reign (Europe) (En,Fr,De,Es,It).iso" --native-only \
    --pnach docs/urban-reign/AE4BEBD3.pnach --pnach-section "Progressive Scan"
```

Then start it at 4x resolution (the line `port.sh` prints, plus the options below):

```sh
PS2_SCALE=4 build/native-urbanreign/ps2run games/urbanreign/patched/SCES_536.88 --disc games/urbanreign/disc.iso \
    --window --game-dir games/urbanreign --patches games/urbanreign/patches.txt --pal --language en
```

In PowerShell: `$env:PS2_SCALE = 4; .\build\native-urbanreign\ps2run.exe ...` with the same options.

- `--pal` is needed with the *Progressive Scan* section: the game counts time in 50 Hz frames, and 480p would otherwise
  run everything, the round clock too, 20 % fast.
- `--language en|fr|de|es|it` picks the console language, which the game follows.
- `--memcard <file>` keeps the memory card where you want it.
- Without `PS2_SCALE`, the resolution is chosen in the quick menu (**F1**, up to 4x, used at the next start).
- Leave out `--pnach-section "Progressive Scan"` (and `--pal`) for the original PAL 576i picture.
- `--pnach-section "Widescreen 16:9"` with `--widescreen` gives the game's own 16:9 mode with a corrected 3D aspect. It is a
  draft: the fight HUD and the menus stay stretched.

## What the patch file does

Lines outside any section always apply. Both are bugs of the original game that a TV's overscan hid, so they are safe on
a real PS2 and in PCSX2 too.

- **Shadow scissor off by one.** Three copies of the shadow projection clamp their scissor to 640 x 448, but SCISSOR
  bounds are inclusive. Row 448 is the first line of the display buffer, so every shadow left short coloured stripes at
  the top of the picture. Clamped to 639 x 447 instead.
- **Bloom reads past its source.** The full-screen glow stretches an 80 x 56 copy of the scene up to texel row 112,
  which the blur passes never write. The last two lines of every fight blended in stale memory as a band of coloured
  noise. Its UVs are pulled in by half a texel (the glow is 1 % smaller).

The optional sections:

- **Progressive Scan.** The PAL game still contains the complete 480p mode of the NTSC release. This section starts it,
  shows the 24-bit back buffer directly (no 16-bit copy, no flicker filter, no 512-line downsample) and points the
  effects that read the shown picture (pause and end-of-fight backgrounds, the special-move haze) at the 640 x 448 copy.
- **Widescreen 16:9** (draft). Forces the game's own anamorphic mode on and corrects its 3D scale from 0.8 to 0.75.

Every patch line has a comment with the original instruction.

## What changed in the tool

Each of these is a separate commit, and each one works for any game.

| Area | Change | To compare with the old way |
|---|---|---|
| Sound | A key on takes effect when it is written. NUSOUND reads ENDX right after each key on, saw the voice still ended, reused it and cut the fighters' cries when they were knocked out. | `PS2_SPU2_KON_LATE=1` |
| Pacing | The game is paced by the clock, steered by the sound queue (about 90 ms). The camera no longer jerks at the start of a fight, and the sound queue neither runs dry nor gets stretched. | `PS2_PACE=audio` |
| Graphics | Up to 4x internal resolution (`PS2_SCALE=4`, the quick menu, `keys.ini`). | |
| Graphics | Upscaled bilinear sprites stay on their own texels at the edges. Button icons cut from a sheet no longer pick up a line of the next icon. | |
| Graphics | Upscaled sprites sample at pixel centres. The GS samples a pixel at its corner and 2D art is laid out for that; upscaled, half of the samples fell between two texels, so text was soft and half a pixel off. | |
| Graphics | Text, icons and menus drawn 1:1 keep each texel's colour, with smooth edges (*Text and menus* in F1: soft, sharp, sharpest). | `PS2_SHARP_2D=0` |
| Graphics | A picture the GS drew is filtered between its own upscaled samples. The knockout trail, a slightly zoomed copy of the last frame, showed a mesh of dots. | |
| Graphics | A cached GS download is served only for reads repeated every frame. Pause backgrounds were stale. | |
| Window | A picture bigger than the window is reduced by averaging, not by an enlarging filter, which left jagged text. | |
| Window | The title shows the disc's serial when no launcher names the game. | `PS2_WINDOW_TITLE` |
| Saves | The memory card is checked when it is loaded, written atomically, locked to one run, and the last good copies are kept in `<card>.backups`. | |
| Console | `--language` sets the language the game reads from the console settings. | |

## Not done yet

- The fight HUD and menus in 16:9.
- FMVs in 480p show interlace combing: the films are interlaced, and a deinterlace for the video decoder would fix it.
- Played for some hours, not from start to end.
