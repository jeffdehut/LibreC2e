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
  export of creatures, saving worlds in the original's format.
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
  pinch to zoom. On touch screens, tap is a click, touch-and-hold picks things up, and
  double-tap speaks.
- **Wolfling runs** (leave your Norns to fend for themselves), with run records and
  unique generational names.
- **A family tree**: every Norn, with its face, in one view you can drag and zoom.
- **Fixes for the original's bugs**, many of them from LibreCreatures. The classic layout
  keeps the original's behaviour wherever it isn't a bug.

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
| `packaging/build-ios.sh` | an iPad/iPhone build for TestFlight (needs Xcode and an Apple Developer account) |

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
