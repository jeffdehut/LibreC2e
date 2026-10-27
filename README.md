# LibreC2e

An open-source engine for playing **Creatures** (1996) on today's computers: macOS,
Windows and Linux, with an iPad and iPhone build on the way. It's built on
[openc2e](https://github.com/openc2e/openc2e) and folds in fixes from
[LibreCreatures](https://github.com/h4rm0n1c/LibreCreatures).

**You need your own copy of Creatures 1** (for example the GOG release). This project
contains none of the game: no pictures, sounds, worlds, genomes, scripts or text. When it
starts, it finds your installation and plays the game from your files.

## What it does

- **Plays Creatures 1** from the original files: the world of Albia, Norns with their
  genetics, biochemistry, brains and language, eggs, breeding, the kits, and import and
  export of creatures, saving worlds in the original's format. The brains, biochemistry
  and genetics follow LibreCreatures' close reading of the original.
- **Eggs as files**, in LibreCreatures' `.egg` format: export the egg in the hand, import
  eggs from anyone. Every egg laid is kept, sorted by generation, and the family tree can
  save a copy of any Norn's egg, living or dead.
- **A folder for each world**, holding the world with its autosaves, photo albums,
  exported Norns, eggs and wolfling records.
- **Two layouts.**
  - **Classic** looks and works like the original, with its toolbar, status bar and kits.
  - **Modern** puts everything on a carved-wood bar along the bottom of the screen:
    - the chosen Norn's portrait, with its life force;
    - Creature's View in a brass porthole;
    - every Norn as a medallion;
    - three quick trays: **Give** (objects, set down beside your Norn), **Heal** (herbs
      and medicine) and **Teach** (speech, reward and punish, words).
  - In the modern layout, the kits are gathered into four panes (Norn, Lab, Objects and
    Records) that fit the window without scrolling.
- **Smooth camera:** real-time scrolling, middle-button drag, the wheel and trackpad, and
  pinch or Shift+wheel to zoom. On touch screens, tap is a click, touch-and-hold picks
  things up, and double-tap speaks.
- **Sharper art, if you have it.** The modern layout can draw higher-resolution sprites in
  LibreCreatures' `NAME@2x.s32` format in place of the originals, frame for frame. Put the
  files in an `Updated Sprites` folder inside or beside your Creatures folder. None are
  included.
- **Wolfling runs** (leave your Norns to fend for themselves), with run records and
  unique generational names.
- **A family tree**: every Norn, with its face, in one view you can drag and zoom.
- **Fixes for the original's bugs**, many of them from LibreCreatures. The classic layout
  keeps the original's behaviour wherever it isn't a bug.

## Latest changes — 27 October 2026

- The modern dock shows each creature's health and a pregnancy ring, with an egg badge
  and an estimated time to laying.
- Hungry Norns are helped to notice usable food nearby. In wolfling runs, the optional
  food assistance supplies food to hungry Norns who cannot see any, hungriest first.
- Population gates on breeding, egg hatching and incubator use are lifted in both layouts.
  Reproductive genes are protected from mutation, duplication and deletion; other genes
  continue to vary. These are deliberate playability changes from the original game.
- Wolfling runs name Grendels too, and end and pause when every creature has died and
  there are no eggs or population-floor replacements remaining.

These changes improve supported play; they do not establish complete original-game
compatibility or guarantee unattended survival for every population or world.

## Getting started

1. Install Creatures 1 (GOG, or your original copy).
2. Build LibreC2e (below) and run it.
3. It looks for your game in the usual places:
   - the original's own settings on Windows;
   - GOG and GOG Galaxy folders, and GOG's Mac app;
   - CrossOver and Wine bottles, and Lutris.

   If it can't find the game, it asks you for the folder that has `Creatures.exe` in it,
   and remembers your choice.

On an iPad or iPhone, copy your Creatures folder into the app's folder: in Finder, select
the device, choose Files, and drag the folder in. You can also use the Files app.

## Building

You need CMake 3.16 or later and a C++20 compiler. SDL3 and the other libraries are in
`externals/`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -Dhave_fsanitize_address=OFF
cmake --build build --target opencreatures1 -j
./build/opencreatures1
```

`packaging/` has scripts for each platform:

| Script | Makes |
|---|---|
| `packaging/build-mac-app.sh` | `LibreC2e.app` for Apple Silicon Macs |
| `packaging/build-windows.sh` | a 64-bit Windows build, cross-compiled with MinGW-w64 |
| `packaging/build-linux.sh` | a Linux build in an Ubuntu 22.04 container (Docker) |
| `packaging/build-ios.sh` | an iPad/iPhone build, installed on a connected device with `--device` or sent to TestFlight (needs Xcode and an Apple developer account) |

### Tests

```sh
cmake --build build --target test_opencreatures1 && ctest --test-dir build
```

Some tests check behaviour against the real game data. Point `OPENC1_GAME_DATA` at a
Creatures 1 folder to run them; without it, they're skipped.

## Legal

LibreC2e is a non-commercial fan project. It isn't affiliated with or endorsed by the
owners of Creatures. *Creatures* and its artwork, sounds, worlds and text belong to their
copyright holders, and nothing of the game is distributed here.

- Where the classic layout shows the original's own wording, that wording is read from
  your installed copy at runtime. The source only holds fingerprints of those lines, plus
  our own wording to use when they aren't found.
- The corrections to the shipped world's scripts work the same way. Each script is
  recognised by a fingerprint, and the corrected version is rebuilt from your own copy.

## Licence

The program as a whole is distributed under the **GNU General Public License, version 3**
([`LICENSE`](LICENSE)), because it includes code adapted from LibreCreatures (GPLv3).
openc2e's own code keeps its licence, the GNU LGPL version 2.1 or later
([`COPYING`](COPYING)). See [`NOTICE.md`](NOTICE.md) for the details, and
[`third_party/librecreatures/NOTICE.md`](third_party/librecreatures/NOTICE.md) for what
came from LibreCreatures.

## Credits

- **openc2e**: the engine this is built on, by Alyssa Milburn and the openc2e
  contributors ([`AUTHORS`](AUTHORS)).
- **LibreCreatures**, by h4rm0n1c and its contributors: fixes for the original's bugs,
  adapted under the GPL.
- **Libraries:** SDL, Dear ImGui, fmt, zlib and others in `externals/`, each under its own
  licence.
