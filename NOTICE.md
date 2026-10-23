# Licensing notice

LibreC2e combines code under different licences. The program as a whole, in particular the
Creatures 1 engine (`opencreatures1`), is distributed under the **GNU General Public
License, version 3** ([`LICENSE`](LICENSE)).

| Part | Licence | Where |
|---|---|---|
| openc2e (the engine this is built on, and its Creatures 2/3 runtime) | GNU LGPL 2.1 or later | [`COPYING`](COPYING), [`AUTHORS`](AUTHORS) |
| Code adapted from LibreCreatures | GNU GPL 3 | [`third_party/librecreatures/`](third_party/librecreatures/) (`LICENSE`, `NOTICE.md`) |
| LibreC2e's own additions (`src/opencreatures1/` and elsewhere) | GNU GPL 3 | [`LICENSE`](LICENSE) |
| Bundled libraries | their own licences | `externals/` |

openc2e's licence allows its code to be used in a GPLv3 program. Its files keep their
original notices. Anyone distributing a build must provide its corresponding source under
the GPL.

## The game itself

No part of *Creatures* is included: no executables, pictures, sounds, worlds, genomes,
scripts or text. LibreC2e needs a player's own installation and reads the game's files from
it at runtime.

- Lines of the original interface shown in the classic layout are found in the player's copy
  by a fingerprint (a length and a rolling hash). The source holds only those fingerprints
  and our own replacement wording. See `src/opencreatures1/GameText.h`.
- Corrections to the shipped world's scripts identify each original script by its length and
  an FNV-1a fingerprint, and rebuild the corrected script from the player's copy. See
  `third_party/librecreatures/C1WorldScriptFixes.h`.
- Higher-resolution sprites (LibreCreatures' `NAME@2x.s32` files) are only ever read from
  the player's own folders. None are included, and git ignores `.s32` files.

*Creatures* and its content belong to their copyright holders. LibreC2e is a non-commercial
project, not affiliated with or endorsed by them.
