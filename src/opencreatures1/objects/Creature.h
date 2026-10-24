#pragma once

#include "CreatureBiochemistry.h"
#include "CreatureBrain.h"
#include "WorldState.h"
#include "CreatureSkeleton.h"
#include "DullPart.h"
#include "fileformats/sfc/Body.h"
#include "fileformats/sfc/Creature.h"
#include "fileformats/sfc/Limb.h"
#include "objects/Object.h"

#include <array>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

class SFCContext;
class genomeFile;
namespace sfc {
struct CBiochemistryV1;
struct CBrainV1;
struct CInstinctV1;
} // namespace sfc

struct BodyPart : DullPart {
	// not implemented
	int32_t angle;
	int32_t view;
};

struct Body : BodyPart {
	// store body data in column-major order, like SFC files but unlike
	// ATT files which are in row-major order
	std::array<std::array<sfc::Vector2i8, 10>, 6> body_data;
};

struct Limb : BodyPart {
	std::array<sfc::LimbData, 10> limb_data;
	std::unique_ptr<Limb> next_limb;
};

// whether creatures' speech is voiced aloud (the toolbar's Speech button, batch 3 Q1)
extern bool g_creature_speech_audible;
// LibreCreatures row 88 (not in the original): a Norn whose Anger is its strongest drive, past
// the upper threshold, shows the head's angry face (frame 12), art the original never uses.
// On with the modern layout (GameUI keeps it in step), off in the classic one.
extern bool g_creature_angry_face;

struct Creature;
// moves a creature's pose one step towards its target, and its animation along (Creature.cpp)
void creature_tick_pose(Creature& c);
// world-wide disease: every 128 ticks a random creature may catch one of the world's
// bacteria (batch 2, Q15; CreatureMind.cpp)
void disease_world_tick();

struct Creature : Object {
	// TODO: lots of things!

	// not implemented
	std::string moniker;
	std::string mother;
	std::string father;

	// sorta implemented
	std::unique_ptr<Body> body;
	std::unique_ptr<Limb> head;
	std::unique_ptr<Limb> left_thigh;
	std::unique_ptr<Limb> right_thigh;
	std::unique_ptr<Limb> left_arm;
	std::unique_ptr<Limb> right_arm;
	std::unique_ptr<Limb> tail;

	void handle_left_click(float, float) override;
	const DullPart* get_part(int32_t) const override;
	void render(RenderSystem&) const override;

	// all body parts, body first
	std::vector<DullPart*> all_parts();
	std::vector<const DullPart*> all_parts() const;
	Rect2f get_body_bbox() const;

	// Poses: a pose string sets each part's target frame, e.g. "212122301110000". The first
	// character is the direction, then head, body, the legs (thigh, shin, foot) and arms
	// (humerus, radius), left before right, then two for the tail. Facing changes at once;
	// each part's angle then moves one step per tick towards its target (spec Q13, Q21).
	void set_pose_string(const std::string& pose);
	void set_pose_number(int32_t pose);
	bool pose_reached() const;
	// ANIM for creatures: two-digit pose numbers, ending in R to repeat
	void set_pose_animation(const std::string& animation);
	bool has_pose_animation() const { return !pose_animation.empty(); }
	void walk();
	// recalculates every part's position and z-order from the frames and foot position
	void reposition();
	// steps onto the floor if the swinging foot is below it, and keeps the body inside the
	// limits (batch 5, Q1 and 1b); after a pose change or MVTO
	void settle_on_limits();
	void move_by(float xdiff, float ydiff);
	CreatureFacing facing() const;
	void drop_carried_objects();
	// the outer end of the arm that carries objects (the second arm in pose strings, spec Q12)
	SkeletonPoint carry_point() const;

	// drives, senses and other locus values used by biochemistry
	CreatureLoci loci;

	BrainMigrationState brain_migration;
	// counts limb movement for the muscle energy locus, reset every 16 ticks (spec Q21)
	int32_t muscle_counter = 0;
	// set when a room limit pushed the creature back (spec Q1)
	bool pushed_by_limit = false;

	// --- the mind (CreatureMind.cpp), following Clean Specs/BATCH1-ANSWERS.md ---
	void creature_tick();
	// creates, places or removes the "zzz" object to match the creature being asleep
	void update_zzz();
	// debugging: warns once if a freely walking creature isn't on any room's floor
	void check_standing_on_a_floor();
	bool standing_in_the_air = false;
	// the attention category of an object: simple objects by genus (0-24), compound objects
	// 25 + genus, creatures 35 + genus; anything else is 0 (spec Q4)
	static int32_t attention_category(const Object& obj);
	// the ground point: the planted foot's x and the floor's y (spec terms)
	float ground_x() const { return numeric_cast<float>(footx); }
	float ground_y() const { return numeric_cast<float>(footy); }

	// What the creature remembers: at most one object per attention category (spec Q1).
	struct AttentionEntry {
		ObjectHandle object;
		bool has_position = false;
		float last_x = 0;
		float last_y = 0;
		uint8_t input = 255; // the last stimulus's input byte, until the next attention update
		uint8_t intensity = 0;
		bool stimulated_since_update = false;
		bool in_view = false;
		bool ever_seen = false;
	};
	std::array<AttentionEntry, 40> attention_memory;
	// Lets go of a category's object, which has gone out of reach or (killed) left the world.
	void forget_object(size_t category, bool killed);
	// per category, a running average of chemicals 33-48 in stimuli from that category
	std::array<std::array<uint8_t, 16>, 40> learned_chemicals{};
	// the object of attention (_IT_) and the point on it the creature aims at (spec Q10)
	ObjectHandle attention;
	float target_x = 0;
	float target_y = 0;
	bool has_target = false;
	void update_target_point();
	bool can_see(const Object& obj) const;
	bool can_hear(const Object& source) const;
	Rect2i sight_limits() const;

	// Stimuli: script stimuli are queued and delivered every 4 ticks; the engine's own
	// stimuli take effect at once (spec Q2, Q22).
	void queue_stimulus(const sfc::StimulusV1& stimulus, ObjectHandle from);
	void receive_stimulus(const sfc::StimulusV1& stimulus, ObjectHandle from, int32_t engine_value = 0);
	void receive_numbered_stimulus(int32_t number, ObjectHandle from, int32_t engine_value = 0);
	struct QueuedStimulus {
		sfc::StimulusV1 stimulus;
		ObjectHandle from;
	};
	std::vector<QueuedStimulus> queued_stimuli;

	// Decisions (spec Q6-Q9)
	int32_t current_decision = 0;
	int32_t aim_action = 0;
	int32_t importance = 0; // IMPT
	void start_decision(int32_t decision);
	void done();
	// a "suggested" verb waiting for attention to switch to its noun (spec Q1)
	int32_t pending_verb = -1;
	int32_t pending_verb_category = -1;
	int32_t pending_verb_updates_left = 0;
	int32_t pending_verb_boost = 0;

	// Involuntary actions (spec Q24)
	std::array<int32_t, 8> involuntary_latency{};
	int32_t running_involuntary = -1;

	// Limits (batch 2, Q10): the room under the creature, its vehicle's cabin, its carrier's
	// limits or the whole world. Worked out only when it's picked up, dropped, moved by MCRT
	// or TELE, or loaded; the creature then stands on the limit's floor.
	void update_limits();
	// the base draw plane, which the creature returns to after being carried (batch 2, Q9)
	uint32_t saved_z_order = 0;
	void land();
	void die();
	// MVTO on a creature moves its ground point
	void move_ground_to(float x, float y);
	void update_expression();
	void blink();

	// The bacterium the creature carries, if any (batch 2, Q15). Uses the world's bacterium
	// record: state 0 none, 1 incubating, 2 active; fatal_level is the cure level.
	WorldState::Bacterium bacterium;
	void disease_update();
	void sneeze();

	// Genetics (CreatureGenetics.cpp, batch 2, Q17-Q21)
	std::shared_ptr<genomeFile> genome; // Genetics/<moniker>.gen, if there is one
	std::array<int32_t, 4> breeds{-1, -1, -1, -1}; // head, body, legs, arms
	// red, green and blue pigment, and the two pigment-bleed values (batch 3, Q34)
	std::array<uint8_t, 3> pigments{128, 128, 128};
	uint8_t pigment_rotation = 128;
	uint8_t pigment_swap = 128;
	void age_update();
	// Picks the creature's own draw plane (saved_z_order), as LibreCreatures does at birth and
	// at each life stage.
	void choose_plane();
	void dream_tick();
	void update_fertility();
	// MATE: try to conceive with _IT_ (Q21)
	void mate();

	// Language (CreatureLanguage.cpp, batch 2, Q22-Q23)
	// the hand's speech (or a script's): heard, obeyed or not
	void hear_sentence(const std::string& sentence, Object* speaker);
	// a word for a vocabulary entry, from a creature, the hand or a blackboard: learnt, and said
	// back (a creature's one time in four), as LibreCreatures has it
	void hear_word(size_t entry, const std::string& word, const Object* from);
	// a blackboard teaches a word for a vocabulary entry (batch 4, Q19)
	void teach_word(size_t entry, const std::string& word, const Object* from = nullptr);
	// Words spoken nearby, still to be heard: the entry, the speaker's own word for it, who said
	// it, and in how many ticks (a phrase's verb and noun come once its voice has finished).
	struct WordToHear {
		size_t entry;
		std::string word;
		ObjectHandle from;
		int32_t ticks;
	};
	std::vector<WordToHear> words_to_hear;
	void hear_words_due();
	// "look": the attention category to turn to
	int32_t look_target();
	std::string said_word(size_t entry) const;
	// shows a speech bubble and voices the text; returns the voice's length in ticks
	int32_t say(const std::string& text, int32_t ticks);
	// Voices (batch 3, Q26): the syllables still to play, and when the next one starts.
	struct Syllable {
		std::string sound;
		uint32_t start_tick;
	};
	std::vector<Syllable> speaking;
	// plays text in the creature's voice; returns its length in ticks
	int32_t voice(const std::string& text);
	void update_voice();
	// loads the voice file for the creature's genus and sex into voices and voices_lookup
	void load_voice();
	void say_need();
	void say_what_doing();

	// the parts of the creature tick
	void brain_step();
	void sight_scan();
	void choose_attention();
	void forget_out_of_reach();
	void update_four_tick_inputs();
	void update_eight_tick_inputs();
	void update_sixteen_tick_loci();
	void update_environment_loci();
	void need_driven_recall();
	bool try_involuntary_action();
	void choose_voluntary_action();

	Limb* left_shin() { return left_thigh ? left_thigh->next_limb.get() : nullptr; }
	Limb* left_foot() { return left_shin() ? left_shin()->next_limb.get() : nullptr; }
	Limb* right_shin() { return right_thigh ? right_thigh->next_limb.get() : nullptr; }
	Limb* right_foot() { return right_shin() ? right_shin()->next_limb.get() : nullptr; }
	Limb* left_hand() { return left_arm ? left_arm->next_limb.get() : nullptr; }
	Limb* right_hand() { return right_arm ? right_arm->next_limb.get() : nullptr; }

	uint8_t direction;
	// set when the right foot is on the ground and (footx, footy) is its tip
	bool downfoot_right;
	int32_t footx;
	int32_t footy;

	// not saved: the current ANIM or WALK, the last pose set with POSE, and each part's
	// target frame
	std::string pose_animation;
	size_t pose_animation_index = 0;
	int32_t pose_number = -1;
	void step_pose();
	// Pose values (batch 2, Q2-Q4): value 0 is the facing (0 away, 1 towards the camera,
	// 2 right, 3 left); values 1-14 are the parts in pose string order (0-3 angles, 4 towards
	// the camera, 5 away). Frames are worked out from them.
	std::array<int8_t, 15> pose_values{};
	std::array<int8_t, 15> pose_targets{}; // -1: no target ('X')
	int8_t target_facing = 2;
	bool facing_from_question = false; // the facing came from '?' (towards _IT_)
	bool head_looks_at_it = false; // '?' in the head position
	DullPart* pose_part(size_t character);
	void update_frames();
	void init_pose_values_from_frames();
	int8_t head_look_value() const;
	void turn_step();

	// not implemented
	uint32_t z_order;
	std::string current_pose;
	uint8_t expression;
	uint8_t eyes_open;
	uint8_t asleep;

	// sorta implemented
	std::array<std::string, 100> poses;
	std::array<std::string, 8> gait_animations;

	// not implemented
	std::array<sfc::VocabWordV1, 80> vocabulary;
	// Fill missing spoken vocabulary with the babble found in original newborn exports.
	// Also repairs creatures created before vocabulary initialization was implemented.
	void initialize_vocabulary();
	// Player Teach action: action words plus look/what, preserving identity and age.
	size_t learn_action_words();
	std::array<sfc::Vector2i, 40> object_positions;
	std::array<sfc::StimulusV1, 36> stimuli;
	// std::unique_ptr<Brain> brain;
	// std::unique_ptr<Biochemistry> biochemistry;
	std::shared_ptr<sfc::CBrainV1> brain;
	std::shared_ptr<sfc::CBiochemistryV1> biochemistry;
	uint8_t sex;
	uint8_t age;

	// sorta implemented
	uint32_t biotick;

	// not implemented
	std::string gamete;
	std::string zygote;
	uint8_t dead;

	// sorta implemented
	uint32_t age_ticks;

	// not implemented
	uint32_t dreaming;
	std::vector<std::shared_ptr<sfc::CInstinctV1>> instincts;
	std::array<std::array<uint32_t, 16>, 40> goals;
	ObjectHandle zzzz; // the "zzz" object shown while asleep (batch 3, Q35)
	std::array<std::array<uint32_t, 3>, 27> voices_lookup;
	std::array<sfc::VoiceV1, 32> voices;
	std::string history_moniker;
	std::string history_name;
	std::string history_moms_moniker;
	std::string history_dads_moniker;
	std::string history_birthday;
	std::string history_birthplace;
	std::string history_owner_name;
	std::string history_owner_phone;
	std::string history_owner_address;
	std::string history_owner_email;

	void serialize(SFCContext&, sfc::CreatureV1*);
};
