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

## Upstream review, 2026-10-22 (kits)

Reviewed main up to b775a03. The rebuilt kits (src/kits/) are Windows MFC programs that
talk to the game over DDE, so none of their code runs here; their page designs are
adapted, as Dear ImGui pages, for the classic layout's kit windows (Kits.cpp, the `lc_`
functions; Preferences offers the 1996 pages and covers instead):

- Health Kit (src/kits/health/src/health_pages.cpp): the Fitness page's headed, labelled
  readings and cold-to-hot scale; every drive's gauge with the strongest marked; a row per
  brain lobe with its colour, purpose and activity.
- Funeral Kit (src/kits/funeral/src/funeral_pages.cpp): Deaths, the dead newest first beside
  each one's memorial (epitaph, Make/Visit Headstone, photographs), and the Graveyard of
  raised stones. The graveyard file records a raised stone (OPENC1-GRAVES 3; in older files
  an inscription counts as one).
- Injector Kit (src/kits/injector/src/injector_pages.cpp): Cobs, with a find box, what's left
  of each add-on, warnings, Inject and Remove; Analysis, what an add-on's scripts do.
  C1CobAnalysis.h adapts the script analysis in src/kits/c1kitlib/include/c1kit/cob.hpp:
  the objects made (`setv clas`), the chemicals changed (`chem` and stimulus pairs), the
  things to warn about, and a removal for an add-on without an .rcb. The warnings and the
  event names are our own wording. CobInjector.cpp's remove_cob runs that removal.
- Hatchery (src/kits/hatchery/src/hatchery.cpp): the Nest (the 1996 picture) and the
  Designer, which makes an egg from any of mum1-mum9 and dad1-dad9, chosen by their faces,
  with a sex, colour and genetic changes. C1GenomeEdits.h adapts
  src/kits/c1kitlib/include/c1kit/genome_edits.hpp (eat better, no death from old age,
  locked learning genes, hardier babies) and hatchery.hpp's colour genes, working on gene
  records as the engine stores them.
- Science Kit (src/kits/science/src/page_*.cpp): Biochemistry as one graph with up to 16
  of the named chemicals ticked beside it; the Brain scanner as a map of the whole brain on
  its grid, each lobe outlined and named, neurons shaded with the square root of their
  value, a list of each lobe's activity; Decisions with their values and the strongest
  marked; Genetics as a list of properties; Injections whose dose is the amount added.
- Breeder's Kit (src/kits/breeder/src/breeder_pages.cpp): the Fertility page's hormones (for a
  female progesterone, oestrogen and gonadotrophin, for a male testosterone and
  gonadotrophin, then sex drive and glycogen) as labelled bars, the pregnancy, and a graph of
  them over the last minutes.
- Ecology Kit (src/kits/ecology/src/ecology_map.cpp, c1kitlib ecology.hpp), as the
  Observation Kit's Map page: back.spr's tiles at a sixth of the world's size, the rooms
  coloured by crowding, food and drink (genera 6 and 7), toys and instruments (9, 13, 14) or
  infected creatures, creatures as dots in their genus colours, eggs (2 5 2), and a click to
  choose a creature and look at it. This engine has no weather, so the temperature layer is
  a layer of room kinds instead.
- Biochemistry Kit (src/kits/biochem/src/biochem_pages.cpp), folded into the Science Kit's
  pages: saved sets of followed chemicals, any chemical by name for an injection, and an
  injection repeated every n seconds, n times (0 until stopped).
- Owner's, Observation and Score Kits: reviewed. Their fixes are to the Windows programs'
  own workings (stacked tabs, query holders, registry keys, scroll ranges, clock digits),
  which these kits don't have; their pages match ours.
- Health and Breeder's Kits' Options > Refill Dispensary (src/kits/c1kitlib/include/c1kit/
  health_files.hpp): a button on the shop pages that brings every item back up to the 12 the
  game ships with, leaving any item with more alone.

## Upstream review, 2026-10-23: egg files

Reviewed main up to 62049d3 and the neorender branch up to 88f9c46. C1EggFile.h adapts the
egg file of src/c1/creatures/egg.hpp and egg.cpp and Genome::serialize (genome.cpp), so eggs
can be traded with LibreCreatures: version 2, a "CEgg" record (classifier, sex, the genome's
size and CRC-32) and a "CGenome" record (size, the four-character name, sex, life stage, the
genome file), every byte checked before use. EggFiles.cpp adapts Export Held Egg and Import
Egg (src/c1/platform/windows_egg_transfer.cpp): an exported egg leaves the world; an imported
one is a fresh egg by the incubator, renamed when its name is taken, and its file is used up.
Not in LibreCreatures: each world's archive of its eggs by generation, copies of any Norn's
egg from the family tree, and a world's own archive copies not being used up on import.

## Higher-resolution sprites, 2026-10-23

HdArt.cpp reads LibreCreatures' S32 files (adapted from neorender's src/c1/display/s32.cpp,
88f9c46): a flags word with bit 0x4, a count, then each frame's PNG offset, every offset and
PNG header checked against the file. As neorender's image_tiers.cpp requires, a NAME@2x.s32
file is used for a gallery only when it has each of the gallery's frames at exactly twice the
size (a frame with no size in the .spr isn't held against it). The art itself is the
player's own and is never included.

The modern layout's panes now show the kits' pages adapted earlier (Health, Breeder's,
Science, Hatchery Designer, Injector Analysis, Ecology map, Funeral Deaths and Graveyard),
drawn in the modern layout's wood and brass.

## Brain, 2026-10-23

CreatureBrain.cpp's update is adapted from src/c1/brain/lobe.cpp, neuron.cpp, brain.cpp and
rules.cpp (main 62049d3), keeping the world file's brain structures: rules read a register
file filled as the lobe updates (a dendrite's rules see the neuron's new state before it
relaxes, its earlier output and its sums from before its rule); dendrite sums are worked out
once per neuron; the migrations draw from each lobe's list of neurons that fired at its last
update (before winner-takes-all), whole-set rewiring shuffles that list, searches down from
the last neuron it rewired and checks the lobe's other active neurons for the same set;
lobes 0-5 only take input; activity is out of 256. CreatureGenetics.cpp's lobe growth folds
rule bytes into 30 tokens, wraps a range whose maximum is below its minimum, sizes lobes as
the original does and holds only lobes 1-7 to 32 x 32. LibreCreatures' fixes 2, 4, 5 and 6
are the behaviour kept. Random-number order is the engine's own.

## Biochemistry, 2026-10-23

Compared with src/c1/biochemistry/biochemistry.cpp (main 62049d3): the update (emitters,
receptors, reactions, decay, and LibreCreatures' fixes 50 and 51) already matched. Adapted
from its gene loading, in CreatureGenetics.cpp and CreatureBiochemistry.cpp: a receptor
replaces any earlier one on the same locus whatever its chemical (an emitter, one on the same
locus for the same chemical); the organ byte folds onto 0 and 1; reaction amounts fold into
1 to 16 (also when worlds saved before run); at most 128 reactions.

## Genetics, 2026-10-23

CreatureGenetics.cpp's crossover and mutation are adapted from src/c1/creatures/genome.cpp
(main 62049d3, Genome::recombine_genome_streams, copy_gene_with_mutation,
random_mutation_mask): runs of 10-100 genes from a random parent, crossing over where the
other parent has the next gene (its first like it) unless that gene is a duplicate of the
one just copied; one crossover in 81 duplicates the next gene (its generation one higher in
the first copy) or leaves it out, as its flags allow; a gene's first eight bytes and its
flags byte are never mutated, its switch-on stage and data bytes 1 in 1601 each, a bit chosen
by coin flips; a clone is an exact copy. Our own addition: a cap on how far duplications can
grow a genome.

## Creature: attention and stimuli, 2026-10-23

From src/c1/creatures/creature.cpp (main 62049d3), Creature::update_attention and
apply_stimulus, in CreatureMind.cpp: the decision lobe's winner-takes-all leaves out the
actions the creature can't take with its current attention (or lack of it); asleep, attention
doesn't turn to nothing; a sense neuron must be under 32; a stimulus's neuron offset applies
only when it isn't modulated, wrapping as a byte; and an object parked below the world by
`kill` doesn't become the thing to attend to (LibreCreatures' fix 9). Action selection already
matched.
Also from creature.cpp, update_perception and update_goal_direction: a remembered object's
position is kept up to date whether or not it can be seen (never below the world); the wall
sense also fires past the wall; and goal direction skips category 0, isn't run asleep,
dreaming or riding, forgets a remembered place only facing it sideways, stops when the
remembered object is already attended to, takes "right here" as to the left, and groups
undersea rooms with surface ones. Its numbers already matched our specs'.

## Creature: life cycle and disease, 2026-10-23

From src/c1/creatures/creature.cpp (main 62049d3), update_environment_and_life_stage and
update_bacterium_and_environment, and src/c1/creatures/bacterium.cpp, in CreatureMind.cpp and
CreatureBiochemistry.cpp: the death receptor raises a signal of its own, apart from the dead
flag that the emitter on the same locus reads, and the signal isn't a latch; fertility, the
fertile and pregnant outputs and the death signal are read at the 128-tick check, in its
order (environment, fertility, life stage, death, then the recall), as our batch 7 answers
also have it; an antigen's antibody is 8 below it in a byte; a sneeze doesn't infect a creature
already ill, and one nobody sees moves the world's bacteria down a place, a mutated copy
taking the last; random infection picks from every creature, and is 1 in 251 with more than
24. Temperature, light, crowdedness, life stages and bacterium mutation already matched.
Our own choice: a newly expressed death receptor doesn't seed its nominal value, so a
newborn can't die before its first chemistry step.
Also from creature.cpp: a body no longer ages (Creature::update advances the age only while
alive), and select_walk_gait's rule that the last gait is only for walking to the object
attended to. Mating (process_insemination) and the head's look table already matched.

## Creature planes, 2026-10-23

From src/c1/creatures/skeleton.cpp (main 62049d3), select_normal_render_plane and its calls
from load_genome, in CreatureMind.cpp Creature::choose_plane: a creature's draw plane is
chosen at random from 1000 to 3000 at birth and at each life stage, again while another
creature's plane lies 0-8 above it. As LibreCreatures saves it, the creature's own plane goes
into the world file, not the hand's while it's held. Our own additions: a limit on the tries,
and new planes on loading for creatures outside that range (our plane-100 saves).

## Scripts and memory, 2026-10-23

From src/c1/scripting/macro.cpp and tables.hpp (main 62049d3), in MacroCommands.cpp,
MacroContext.cpp and Scriptorium.h: `bbd:` does nothing unless TARG is a blackboard and the
slot exists, and any non-zero value counts as on; an object animation of 32 characters or
more is ignored (the world file has 32 bytes for it); any value can name an object (objects in
variables); a TARG of nothing leaves TARG as it was; reversed `rndv` and `ltcy` bounds give
the first; `new: part` selects the new part; `stim from` goes to FROM when that's a creature;
room for 2000 scripts. From src/c1/creatures/creature.cpp forget_killed_object and
clear_references_to_object, in CreatureMind.cpp Creature::forget_object: forgetting an object
drops only the link (and, for one that's gone, its stimulus), keeping where that kind was;
from deserialize, a remembered place at y 1200 or below is unknown.
Also: what the hand held when a world was saved is held again after loading (LC
simple_object.cpp tick and document.cpp serialize; ours PointerManager::restore_hold), and an
object's timer is a countdown, saved as one, that keeps running without a timer script (LC
simple_object.cpp tick; our batch 6 answers agree).
Also from macro.cpp: `appr` chooses the walking gait the first time, over any looping animation,
and turns itself into APPR in the script while it waits, back into appr on arriving; and a
saved script's buffer size is its length + 6, with object scripts marked "destroy when
finished".
Also from src/c1/creatures/registry.cpp and creature.cpp: the register's monikers are hex of
their 32-bit values, "0" for none, father before mother, and a newborn's birthday is its
creation time ("%H:%M %b %d %Y"). Our addition: plain monikers in the old order are read as our
own older saves.
Also from src/c1/creatures/skeleton.cpp and body.cpp serialize: a creature's pose is saved as
the values reached, in digits, and each part's value and frame (without the face or closed
eyes) as its angle and view.
Also from application/document.cpp serialize (our batch 6 answers agree): the list after the
scripts holds objects `kill` parked out of the world, which we delete on loading, and death row
is the file's last list, up to 16 monikers for the Funeral Kit.
Also from macro.cpp execute_object_enumeration_command (our batch 11 answers agree): ENUM keeps
a frame of four on the flow stack (the loop's start, the classifier, its mask, and the current
object's place in the list), so loops nest and one saved part-way carries on from its place.
Our own: the rest of each loop is kept as a list, so objects killed in the loop are skipped.

## Waking, 2026-10-23

From src/c1/creatures/creature.cpp dispatch_script_event and its callers (main 62049d3), in
MacroManager::queue_script: an object event sent to a sleeping creature (activate, deactivate,
pickup, drop, initialise, deinitialise) wakes it and removes its zzz, script or no script; its
own decisions and reflexes don't.

## Speech, 2026-10-23

From src/c1/creatures/creature.cpp (main 62049d3), speak_action_phrase,
speak_dominant_drive_phrase, handle_word_learning_event, update_learned_word_record,
process_heard_words and resolve_pointer_attention_target, events.cpp
queue_events_in_speech_range, and the platform's word-learning and blackboard hosts, in
CreatureLanguage.cpp and Blackboard.cpp:
- A creature's phrase is its name (one time in three), the verb for its action and, one time in
  three for an action on something, the noun for what it attends to.
- Creatures in earshot learn each word from the speaker's own heard word, the verb and noun once
  the voice has finished, with no stimuli or brain suggestions, and say it back one time in four.
- Blackboards teach those that see them (emit 0) or hear them (emit 1), and are always answered.
- "Come" and "run" name the creature's own kind, and the sex drive (and drives 13-15) have phrases.
- Heard speech:
  - a name alone turns attention to the speaker, "why" half the time, "rest" to nothing;
  - praise, scold, look and what act on the last word that isn't a noun;
  - "look" picks the object on the hand's side nearest the hand.

## Lifts and transports, 2026-10-23

From src/c1/objects/lift.cpp, call_button.cpp and vehicle.cpp (main 62049d3), with the
platform's event mapping and passenger plane rule, in Object.cpp, Lift.h and Lift.cpp:
- **Calls.** They take the first free of eight saved slots; a creature a button won't answer is
  disappointed but its call counts, and a full lift turns the button off. Calls are looked at
  every 16 ticks (skipping one look after each arrival): the nearest by floor, the earliest slot
  on a tie, answered with activate 1 or 2 by direction.
- **Arrows.** A lift's arrows ignore its creature knobs and set actv 1 either way.
- **Arrival.** A lift arrives when the cabin is within its speed's whole pixels of the floor or
  past it, stops where it is, and clears only the call it answered.
- **Passengers.** They are drawn 5 planes off the vehicle's main part, and get their own plane
  back when they leave. A rider that starts an action about anything but its vehicle asks it to
  stop.
- **tele.** It moves only creatures.
- **Collisions.** A vehicle checks for them only at a new whole pixel, resting on the ground
  counts as a hit, and a hit drops its fraction of a pixel.
