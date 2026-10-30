# ps2-native-recomp

Turns the disc image of a PS2 game you own into a native program for Windows or Linux. **This project is at an early
stage** and still changes a lot. God of War II has been played from start to end on Windows; a few more games run as far
as they have been played.

**Don't expect every game to work on the first try.** Only a few games have been tried so far, and each game uses the
console a little differently, so many still stop at a black screen, crash, or play with missing sound or wrong pictures.
When a game doesn't work, please send me a report (see [*Playtesters wanted*](#playtesters-wanted)): every report shows
me what to fix next, and one fix often helps several games at once.

## What it does

- **Turns your disc into a PC game:** in the app, choose the disc image, press *Build*, then *Play*. You get a program of
  its own with a desktop shortcut. The app downloads its own build tools, so nothing has to be installed.
- **Windows and Linux**, drawing through Direct3D 12 or Vulkan.
- **Keyboard and mouse or a game controller**, with a key card (F1) and keys you can change.
- **Saving:** quick saves (F5 and F8) and the game's own memory card saves.
- **A clearer picture:** sharper text and HUD, smoother edges, picture styles to switch between (F2), and twice the
  resolution on a strong graphics card.
- **Widescreen and other patches** from PCSX2's community, picked in the app.
- **Smooth sound** when a game runs below full speed: it is stretched instead of chopped.
- **Nothing left out:** game code the first build did not see is translated while you play and kept for the next start.

## How far it is

Still early: the project went public on 28 October 2026. God of War II has been played from start to end. More than 15
other games play well as far as testers got, among them God of War, Gran Turismo 4, Resident Evil 4, Silent Hill 2,
Shadow of the Colossus and Tekken 4 (the full list is in [*What runs*](#what-runs)). Most PS2 games have not been tried
yet, and some still stop at a black screen or crash. 54 of the 55 programs of a free PS2 test suite print exactly what a
real console prints.

## Why recompile instead of emulate

An emulator works like a whole PS2 inside your PC: while you play, it reads the game's code piece by piece and works out
what each piece means. This project does the biggest part of that work once, before you play: it translates the code of
the PS2's main processor into C++ and compiles it into a normal program. That gives you:

- **A real PC program for each game**, with its own shortcut, instead of an emulator with a game loaded into it.
- **Way faster:** the code is already translated when you play, so the PC spends its time running the game instead of
  working out each piece of code first.
- **A game whose code can be changed:** fixes and improvements can go right into the game itself, not only around it.
  That is what opens the door to the ideas below.

To be fair: an emulator like PCSX2 runs far more games today and has years of work behind it. It also translates code
while you play; a side-by-side speed test against it is next. If your game doesn't run here yet, PCSX2 is the better
choice for now.

Under the hood, the rest of the console (graphics chip, vector units, sound, disc, controller, memory card) is rebuilt
in a runtime that draws with WebGPU (wgpu-native, on Vulkan or Direct3D 12).

## Ideas for later

Not promises, just where this can go:

- **An in-game menu** for resolution, picture look and 16:9.
- **A folder you can move:** the game program and your disc image together, to take to another PC.
- **60 fps** for games that run at 30, where the game allows it, and **mods** that change the game's code directly.
- **Faster on weak laptops**, so more games reach full speed on built-in graphics.
- **The Steam Deck and other handhelds:** Linux already works; it needs testing there.
- **More games:** every report shows me what to fix next.

## What runs

Tried with real discs (mine are US releases), as far as each was played (no known bugs):

| Game | How far | Tested by |
|---|---|---|
| God of War II | **played from start to end**, with a game controller; keyboard and mouse work too, and so do quick saves and the game's own save | petarsti |
| God of War | plays well, not played through yet: from power-on through the menus and the opening film into the first fight on the ship, also in 16:9 | me |
| Gran Turismo 4 | plays well, not played through yet: from power-on through the film and the menus, and a whole race played | me |
| Resident Evil 4 | plays well, not played through yet: the menus, the opening, Leon in the forest | me |
| Silent Hill 2 | plays well, not played through yet: the opening at the rest stop and about 20 minutes of play from there but works fine| me |
| Shadow of the Colossus | plays well, not played through yet: played like 30 minutes that went very well | me |
| The Super Dimension Fortress Macross | plays well, not played through yet: menus and gameplay at about 60 fps (Windows 11, a laptop with Intel graphics, a game controller; [#3](https://github.com/exoteric33/ps2-native-recomp/issues/3)) | omegakatana92 |
| The King of Fighters XI | plays well, not played through yet: works perfectly | TsimpleD (Reddit) |
| God Hand | plays well, not played through yet: as far as the casino hub, no problems so far | theraineydaze (Reddit) |
| Tekken 4 | plays well, not played through yet: fights in Story Battle at 60 fps, only a short stutter when a fight starts (Windows 11, a Radeon RX 6800 XT, keyboard and mouse; [#8](https://github.com/exoteric33/ps2-native-recomp/issues/8)) | DevCrashDay |
| Armored Trooper VOTOMS (Japan) | plays well, not played through yet: the tutorial and the first mission at about 60 fps, and saves work; it slowed down once, the first time a new part loaded (Windows 11, a laptop with Intel graphics, a game controller; [#5](https://github.com/exoteric33/ps2-native-recomp/issues/5)) | omegakatana92 |
| Ben 10: Alien Force | plays well, not played through yet: works very well, no problems so far | Travani |
| Samurai Western (Europe) | plays well, not played through yet: from the opening film into the game at 49 to 60 fps, with short slowdowns while new parts appear; the US disc gets there too but slows down more (Windows 11, a laptop with Intel graphics, a game controller; [#19](https://github.com/exoteric33/ps2-native-recomp/issues/19)) | omegakatana92 |
| Thunder Force VI (Japan) | plays well, not played through yet: to the second boss, smooth (Windows 11, a laptop with AMD graphics, a game controller; [#21](https://github.com/exoteric33/ps2-native-recomp/issues/21)) | Pacioso |
| Raiden III | plays well, not played through yet: to the second stage, smooth (Windows 11, a laptop with AMD graphics, a game controller; [#25](https://github.com/exoteric33/ps2-native-recomp/issues/25)) | Pacioso |
| Heavenly Guardian (USA) | plays well, not played through yet: to the first boss, smooth (Windows 11, a laptop with AMD graphics, a game controller; [#28](https://github.com/exoteric33/ps2-native-recomp/issues/28)) | Pacioso |
| Ecco the Dolphin: Defender of the Future (USA) | plays well, not played through yet: the first level, smooth (Windows 11, a laptop with AMD graphics, a game controller; [#30](https://github.com/exoteric33/ps2-native-recomp/issues/30)) | Pacioso |

Known not to work yet (reported by playtesters, on my list to fix):

| Game | What happens | Reported by |
|---|---|---|
| Ben 10: Alien Force - Vilgax Attacks | stops at "Checking memory card" right after the start and goes no further | Travani |

54 of the 55 programs of the free [ps2autotests](https://github.com/unknownbrackets/ps2autotests) suite print exactly
what the console prints (`python scripts/autotests.py`).

Not checked yet: any other game from start to end, macOS since my changes, Windows 10, and every other game.

## Playtesters wanted

I'm looking for people who try this with their own PS2 discs and tell me how it went: which games start, how far they
get, how fast they run, and what looks or sounds wrong. Every report helps, "it just worked" too, and so does every PC I
don't have, from weak laptops to big graphics cards.

1. Build a game you own (see *Get started* below) and play it.
2. When something looks, sounds or behaves wrong, press **F9** right there. It notes the spot, and the game goes on.
3. Open a [playtest report](https://github.com/exoteric33/ps2-native-recomp/issues/new?template=playtest.yml): the game,
   your PC, how far you got, what went wrong, and the text of `log.txt` and `info.txt` from the newest folder in
   `reports\`. If you'd rather not use GitHub, write to me at exoteric54@gmail.com.

Please don't attach disc images, save states or other files from the game; if I need more, I'll ask. Questions, ideas
and fixes are welcome too, as issues, pull requests or by e-mail.

## Get started on Windows

**With the app** (nothing to install): on this page press *Code*, then *Download ZIP*. Unpack all of it into a short
folder such as `C:\PS2Native` and double-click `Start.cmd` (Windows asks once whether to run it: press *Run*). Press
*Add a game*, choose the disc image (an `.iso`, or a `.zip` or `.7z` that holds one), press *Build*, then *Play*. The
first build downloads the build tools it needs (about 312 MB, from their official release pages) into that folder;
building a game takes several minutes. *Make desktop shortcut* puts the game on the desktop.

**From the command line** (for developers):

1. Install llvm-mingw (UCRT), CMake, Ninja, Python 3 and Git for Windows. `scripts\bootstrap.ps1` installs them with
   winget. No Visual Studio is needed.
2. In Git Bash: `bash scripts/setup.sh --native-only`
3. `bash scripts/port.sh mygame "/c/Games/My Game.iso" --native-only`
4. Run the line `port.sh` prints at the end, for example
   `build/native-mygame/ps2run.exe games/mygame/disc/BOOT.ELF --disc games/mygame/disc.iso --window --memcard mygame.ps2`

## Get started on Linux

You need git, clang, CMake, Ninja, Python 3, the SDL2 development files and a Vulkan driver. Then
`bash scripts/setup.sh --native-only` and `bash scripts/port.sh mygame "My Game.iso" --native-only`, and run the line it
prints. Checked on CachyOS with an NVIDIA graphics card.

On macOS the original project builds and runs (on Metal); my changes have not been tried on a Mac yet.

## Playing

- **F1** shows all keys. They can be changed in `keys.ini` (the F1 card has a button for it).
- Keyboard and mouse: **W A S D** is the left stick, the mouse buttons, **Space**, **E**, **Q**, **F** and **Shift** are the
  game's buttons (the F1 card shows which is which), **Esc** is Start.
- A game controller works too (any controller SDL knows).
- **F5** quick save, **F8** quick load, **F11** full screen, **F2** switches between picture styles.
- Twice the resolution: set `PS2_SCALE=2` before starting the game (for a strong graphics card).
- Widescreen and other patches: in the app, *Choose extras...* looks up PCSX2's community patches for the game when you
  ask it to. On the command line: `port.sh --pnach FILE --pnach-section "Widescreen 16:9"`.
- Parts of a game that the first build did not see are translated while you play and kept for the next start. The first
  time, they can run slowly for a few seconds.

## Your own disc

Everyone uses a disc image of their own disc. This repo contains no game code or data and never will: no disc images,
extracted files, generated C++, built game programs, screenshots, recordings or saves. The generated C++ and the built
programs are translations of the game's own code and belong to the game's owners: keep them on your own machine and do
not share them. Please do not ask for disc images here. There is no decryption or copy-protection code in this repo
(PS2 discs need none).

## Credits and license

The recompiler and the runtime started as [ps2-web-recomp](https://github.com/snuri00/ps2-web-recomp) by Samir Nuri,
which also runs on macOS and in the browser; its README explains how the recompiler and the runtime work.

MIT, see [LICENSE](LICENSE): the original copyright is Samir Nuri's, and my changes are under the same license. The VLC
tables of the MPEG decoder are adapted from [Play!](https://github.com/jpd002/Play-) (BSD 2-clause, notice in
`runtime/src/ipu/NOTICE`). wgpu-native, SDL2 and the build tools are downloaded from their own release pages, not shipped.
PCSX2's patch files are downloaded to your PC when you ask for them, not shipped.

I wrote much of the code with the help of an AI coding assistant (Claude Code) and checked it with the test suite and my
own discs.

PlayStation and PS2 are trademarks of Sony Interactive Entertainment. All game names are trademarks of their owners. This
project is not affiliated with or endorsed by Sony or any game publisher.
