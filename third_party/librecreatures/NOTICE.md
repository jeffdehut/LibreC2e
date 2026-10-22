# LibreCreatures source integration

Upstream: https://github.com/h4rm0n1c/LibreCreatures
Author/project attribution: h4rm0n1c and LibreCreatures contributors.
Pinned commit: 907b8642f79c2688bd8a1c3f43e401a5f9431c8c (main).
License: GNU GPL version 3; full text in LICENSE.

Jeff states that the author has granted permission for reuse. This integration
also retains the upstream GPL license and attribution; no separate unpublished
license terms or commercial-use exceptions are assumed.

C1Arithmetic.h adapts the arithmetic cases of
src/c1/scripting/macro.cpp, Macro::execute_variable_arithmetic_command.
It preserves unsigned 32-bit arithmetic, widened signed division/remainder,
wrapping the signed minimum divided by -1, and skipping assignment for a zero
divisor. Changes: independent C++ header, std::optional for skipped assignment,
std::bit_cast for explicit signed representation, and openc2e command adapters.
No Windows/MFC transport or original game assets are included.

C1WorldScriptFixes.h adapts the stock/fixed script corrections from
src/c1/platform/windows_document_host.cpp at the pinned commit, without keeping any of
the original game's script text: each original script is recognised by its length and an
FNV-1a fingerprint, and the corrected script is rebuilt from the player's own copy by a
recipe of copied ranges and the few new commands the fixes add. WorldCorrections.cpp
applies them only to exact matching stock bodies, preserving custom scripts and
requiring the island-boat group to be compatible as a whole.

C1CobHeader.h adapts the historical script-classifier parser from
src/kits/c1kitlib/include/c1kit/cob.hpp at the same commit. CobInjector.cpp uses it
only to identify an old whitespace-misinstalled definition for removal; a complete
body match is required before removing that legacy definition. New installations
use the engine's validated whitespace-aware parser.

src/opencreatures1/objects/CreatureMind.cpp also adapts the missing-involuntary-script
correction in upstream src/c1/creatures/creature.cpp (action selection, original bug
11). Our adapter validates and queues the script before publishing the active reflex,
retaining the previous voluntary decision and importance on failure. This is a
source-informed adaptation, not a new clean-room implementation.

The subsequent learning/polish batch adapts these policies from the same pin:

- src/c1/creatures/creature.cpp: the default action-word table used by the explicit
  Teach command. Our command preserves the player's name, age and life stage and
  teaches look/what without the upstream testing command's forced ageing/renaming.
- src/c1/brain/lobe.cpp: ignore loose connections in migration duplicate checks;
  expose the per-type loose-connection counters; map chemical receptor loci to rest
  state and both dendrite types' relaxation/learning rates. Adapters live in
  CreatureBrain.cpp, CreatureBiochemistry.cpp and the runtime-only C1 lobe fields.
- src/c1/creatures/genome.cpp: preserve the recognized gext marker during mutation
  while retaining mutation of its following payload. Our CreatureGenetics.cpp
  adapter limits recognition to a C1 pigment gene's extension position.

The shared genome parser's optional C1 extension reader/writer is independently
written from the file-format behavior. C2 does not interpret that extension.

The C1 executable now combines this GPLv3 component with the existing openc2e
sources. Preserve both licenses/notices and provide the corresponding source
under applicable GPL terms when distributing that combined executable. Original
openc2e source notices remain intact; this does not relicense the independent C2
runtime or grant rights to original Creatures assets. The combined source is published
with LibreC2e under the GPL.

C1LearnedWords.h adapts the pronunciation substitutions and newborn word banks in
src/c1/creatures/learned_words.cpp, plus bounded prefix regeneration in
src/c1/creatures/creature.cpp at the same pin. Our std::string adapter limits both
heard and spoken records to 24 bytes and never scans beyond the heard word.
CreatureLanguage.cpp adapts intermediate pronunciation, reinforcement and reply
probability. Blackboard replacement retains the independently specified strength
zero; ordinary heard-word replacement uses 60. CreatureGenetics.cpp initializes
vocabulary after genus expression so the correct word banks are selected.

CreatureBiochemistry.cpp adapts src/c1/creatures/biochemistry.cpp's positive-excess
receptor arithmetic (divide by 256), absent-chemical exclusion and emitter clearing
only after an eligible emission. Existing death latching and safe period-zero
handling remain in the openc2e adapter.

C1Palette.h adapts src/c1/display/palette.cpp's channel ramps and creature palette
remapping at the same pin. It uses the existing expanded openc2e palette, restricts
matching to indices 10..245, preserves reserved/transparent indices, validates palette
capacity and returns a plain index table. ImageManager applies the table to joined
C1 body-part pixels before rendering and writing generated sprites. Creature loading
retains existing art and remaps reconstructed missing art exactly once. No original
palette or sprite assets are included in the source port.

CreatureGenetics.cpp also adapts receptor target nominal initialization during
new life-stage expression from src/c1/biochemistry/biochemistry.cpp. Our newborn
brain is constructed before expression so those targets exist. The openc2e immune
target is a latched death flag: its nominal is deliberately not written into that
flag during expression; the chemistry step evaluates its actual chemical signal.
Existing dead creatures remain dead. Loaded brains are not re-expressed or reset.

Batch 9 behavior reconciliation: CreatureLanguage.cpp now uses retained replacement
strength 60 for both blackboard and ordinary hearing, superseding the earlier adapter
exception above. The corrected clean behavior answer is Batch 9 section 8a.

## Upstream review, 2026-10-21

Reviewed main up to 3bba7ef (7 commits after the pinned 907b864): the LibreFixes
catalog and its Windows options dialog, the 64-bit object-handle registry, tick and
dialog timing traces, kit crash reports and a BiochemKit toolbar bitmap. None needs
porting: the dialog, 64-bit registry, traces and kit transport are Windows/MFC or
LibreCreatures-internal; openc2e already uses generation-checked object handles.
The catalog was used as a checklist. Rows already corrected here: 1, 7, 11, 13, 22,
27, 28, 33, 48, 49, 51, 60, 66, 83 and the world-data script rows 69-72. Row 23 (the
Flying Lemon) is covered by releasing passengers that no longer touch the cabin;
row 18 (the egg in the sky) cannot occur, since this engine's NEW: does not end a
script's turn; row 36 (sound cache size) does not apply, since sounds aren't cached.

## Upstream review, 2026-10-22

Reviewed main up to 8aed7f9 (3 commits after 3bba7ef). Row 88, the angry face, is adapted
from src/c1/creatures/creature.cpp and skeleton.cpp: when Anger is above its upper
threshold and stronger than every other drive the face reacts to (a tie goes to the other
drive), the head shows its angry expression, frame 12, which the original never shows.
Upstream ships it off by default; here it follows the modern layout (classic keeps the
original faces), and the expression is saved as sad so the original engine reads the world
unchanged. Adapters: Creature::update_expression (CreatureMind.cpp), Creature::update_frames
(Creature.cpp), g_creature_angry_face; tests/ExpressionTest.cpp. The C1_NATIVE_ROWS
override and the verification-script changes are LibreCreatures' own fix-catalogue lab
tooling and have no counterpart here.
