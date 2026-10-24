// Creature language: hearing sentences, learning words and speaking in speech bubbles.
// Behaviour follows Clean Specs/BATCH2-ANSWERS.md, questions 22 and 23.

#include "Bubble.h"
#include "Creature.h"

#include "CreatureBrain.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "ObjectManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "C1SoundManager.h"
#include "common/Ascii.h"
#include "common/io/FileReader.h"
#include "common/endianlove.h"
#include "common/Random.h"
#include "../../../third_party/librecreatures/C1LearnedWords.h"

#include <array>
#include <algorithm>
#include <sstream>

namespace {

// vocabulary entries
constexpr size_t VOCAB_NAME = 16;
constexpr size_t VOCAB_FIRST_NOUN = 17; // nouns for attention categories 1-39
constexpr size_t VOCAB_PRAISE = 72;
constexpr size_t VOCAB_SCOLD = 73;
constexpr size_t VOCAB_LOOK = 74;
constexpr size_t VOCAB_WHAT_DOING = 75;

// genetic stimuli for hearing (Q22)
constexpr int32_t STIMULUS_PRAISED_BY_HAND = 1;
constexpr int32_t STIMULUS_PRAISED_BY_CREATURE = 2;
constexpr int32_t STIMULUS_SCOLDED_BY_HAND = 3;
constexpr int32_t STIMULUS_SCOLDED_BY_CREATURE = 4;
constexpr int32_t STIMULUS_HEARD_NOTHING_KNOWN = 9;
constexpr int32_t STIMULUS_HEARD_HAND = 10;
constexpr int32_t STIMULUS_HEARD_CREATURE = 11;

// speech bubbles: syst.spr frames 9 (tail bottom right) and 10 (tail bottom left)
constexpr int32_t BUBBLE_FRAME_TAIL_RIGHT = 9;
constexpr int32_t BUBBLE_FRAME_TAIL_LEFT = 10;
constexpr int32_t BUBBLE_PLANE = 9000;
constexpr int32_t NEED_BUBBLE_TICKS = 20;
constexpr int32_t BUBBLE_TICKS_VOICE_LENGTH = -1;

bool same_word(const std::string& a, const std::string& b) {
	return !a.empty() && to_ascii_lowercase(a) == to_ascii_lowercase(b);
}

std::string newborn_word(size_t entry, uint8_t genus) {
	using namespace openc1::librecreatures;
	const size_t partition = genus <= 1 ? 0 : 4;
	const auto& bank = newborn_word_banks[partition + rand_size_t(0, 3)];
	const size_t group = entry < 16 ? 0 : entry == VOCAB_NAME ? 4 : entry < 56 ? 1 : entry < 72 ? 2 : 3;
	return bank[group];
}

// Source-informed progression, reconciled by Batch 9 Q8a: ordinary heard words
// and direct blackboard teaching both retain strength 60 on replacement.
void learn_word(sfc::VocabWordV1& record, const std::string& input) {
	using namespace openc1::librecreatures;
	const auto word = input.substr(0, word_capacity);
	if (word.empty()) return;
	uint32_t strength = record.strength & 255u;
	if (same_word(record.in, word)) {
		strength = std::min<uint32_t>(255, strength + 60);
		if (strength >= 160) record.out = record.in;
		else if (strength >= 80) record.out = normalize_word_response(record.in);
	} else strength = strength > 45 ? strength - 45 : 0;
	if (strength == 0) {
		record.in = word;
		record.out = regenerate_word_response(word, record.out);
		strength = 60;
	}
	record.strength = strength;
}

std::vector<std::string> split_words(const std::string& sentence) {
	std::vector<std::string> words;
	std::istringstream in(sentence);
	std::string w;
	while (in >> w) {
		words.push_back(w);
	}
	return words;
}

} // namespace

void Creature::initialize_vocabulary() {
	for (size_t i = 0; i < vocabulary.size(); ++i) {
		auto& word = vocabulary[i];
		if (!word.out.empty()) continue;
		// A word already reinforced past the learning threshold should be spoken as taught.
		word.out = !word.in.empty() && word.strength >= 160 ? word.in : newborn_word(i, genus);
		if (word.in.empty()) {
			word.in = word.out;
			word.strength = rand_uint32(30, 50);
		}
	}
}

void Creature::teach_word(size_t entry, const std::string& word, const Object* from) {
	// Batch 4, Q19: only while awake. The same word as the entry's heard word strengthens it
	// by 60 (up to 255), and at 160 it becomes the word the creature says; any other word
	// weakens it by 45, and at 0 the heard word is replaced. As LibreCreatures has it, the
	// creature then says the word back.
	hear_word(entry, word, from);
}

void Creature::hear_word(size_t entry, const std::string& word, const Object* from) {
	if (asleep || dead || entry >= vocabulary.size() || word.empty()) {
		return;
	}
	learn_word(vocabulary[entry], word);
	// said back: always to the hand or a blackboard, one time in four to another creature
	if (from && from->as_creature() && rand_int32(0, 3) != 0) {
		return;
	}
	say(vocabulary[entry].out, NEED_BUBBLE_TICKS);
}

void Creature::hear_words_due() {
	for (size_t i = 0; i < words_to_hear.size();) {
		if (words_to_hear[i].ticks-- > 0) {
			++i;
			continue;
		}
		const WordToHear w = words_to_hear[i];
		words_to_hear.erase(words_to_hear.begin() + static_cast<std::ptrdiff_t>(i));
		// nothing if the speaker has gone (LibreCreatures)
		if (auto* from = g_engine_context.objects->try_get(w.from)) {
			hear_word(w.entry, w.word, from);
		}
	}
}

// Explicit player assistance: install meaningful action words, not the creature's
// existing babble. Vocabulary table adapted from the authorized LibreCreatures
// snapshot; see third_party/librecreatures/NOTICE.md. Preserve name and life stage.
size_t Creature::learn_action_words() {
	if (dead) return 0;
	static constexpr const char* verbs[] = {
		"stay", "push", "pull", "stop", "come", "run", "get", "drop",
		"why", "rest", "west", "east", "aaa", "bbb", "ccc", "ddd"};
	for (size_t i = 0; i < std::size(verbs); ++i) {
		vocabulary[i].in = vocabulary[i].out = verbs[i];
		vocabulary[i].strength = 255;
	}
	for (auto entry : {std::pair<size_t, const char*>{74, "look"}, {75, "what"}}) {
		vocabulary[entry.first].in = vocabulary[entry.first].out = entry.second;
		vocabulary[entry.first].strength = 255;
	}
	return 18;
}

void Creature::hear_sentence(const std::string& sentence, Object* speaker) {
	// As LibreCreatures hears it (checked against the original). Creatures don't hear each
	// other's sentences: they learn each other's words (say_what_doing), and aren't told what
	// to do by them.
	if (dead || asleep || !speaker) {
		return;
	}
	const bool from_hand = speaker->as_pointer_tool() != nullptr;
	auto* it = g_engine_context.objects->try_get(attention);
	const int32_t it_category = it ? attention_category(*it) : -1;
	const size_t it_noun = it_category > 0 ? VOCAB_NAME + static_cast<size_t>(it_category) : VOCAB_NAME;

	// the nouns, the name, and the last other word known (the verb)
	int32_t verb = -1, noun = -1;
	bool name_used = false;
	for (const auto& word : split_words(sentence)) {
		auto known = std::find_if(vocabulary.begin(), vocabulary.end(), [&](auto& e) { return same_word(e.in, word) || same_word(e.out, word); });
		if (known == vocabulary.end()) {
			// an unknown word is offered to the noun for _IT_, or the name with no _IT_
			hear_word(it_noun, word, speaker);
			continue;
		}
		const auto index = static_cast<int32_t>(known - vocabulary.begin());
		if (index == static_cast<int32_t>(VOCAB_NAME)) {
			name_used = true;
		} else if (index >= static_cast<int32_t>(VOCAB_FIRST_NOUN) && index < static_cast<int32_t>(VOCAB_FIRST_NOUN) + 39) {
			noun = index;
		} else {
			verb = index;
		}
	}

	receive_numbered_stimulus(!name_used && noun < 0 && verb < 0 ? STIMULUS_HEARD_NOTHING_KNOWN : from_hand ? STIMULUS_HEARD_HAND : STIMULUS_HEARD_CREATURE, speaker->uid);
	// the noun for _IT_ is strengthened when it's heard
	if (it_category > 0 && noun == static_cast<int32_t>(it_noun)) {
		hear_word(it_noun, vocabulary[it_noun].in, speaker);
	}

	// what to attend to, and what to do
	const int32_t speaker_category = attention_category(*speaker);
	int32_t category = -1, action = -1;
	int32_t magnitude = 180;
	if (name_used && noun < 0 && verb < 0) {
		category = speaker_category; // its name on its own: it looks at who said it
	} else {
		category = noun < 0 ? -1 : noun - static_cast<int32_t>(VOCAB_NAME);
		switch (verb) {
			case 8: // "why": half the time, look at who asked
				if (rand_int32(0, 1) == 0) {
					category = speaker_category;
				}
				action = verb;
				break;
			case 9: // "rest": attend to nothing
				category = 0;
				action = verb;
				break;
			case static_cast<int32_t>(VOCAB_PRAISE):
				receive_numbered_stimulus(from_hand ? STIMULUS_PRAISED_BY_HAND : STIMULUS_PRAISED_BY_CREATURE, speaker->uid);
				break;
			case static_cast<int32_t>(VOCAB_SCOLD):
				receive_numbered_stimulus(from_hand ? STIMULUS_SCOLDED_BY_HAND : STIMULUS_SCOLDED_BY_CREATURE, speaker->uid);
				break;
			case static_cast<int32_t>(VOCAB_LOOK):
				category = look_target();
				magnitude = 255;
				break;
			case static_cast<int32_t>(VOCAB_WHAT_DOING):
				say_what_doing();
				break;
			default:
				if (verb >= 0 && verb < 16) {
					action = verb;
				}
				break;
		}
	}

	// suggested to the brain, harder when it's said by _IT_ or with the creature's name
	int32_t activation = 180, commitment = 180;
	if (speaker->uid == attention || name_used) {
		activation = commitment = magnitude = 255;
	}
	if (noun >= 0) {
		activation = activation * static_cast<int32_t>(vocabulary[static_cast<size_t>(noun)].strength & 255) >> 8;
		commitment = magnitude;
	}
	if (verb >= 0) {
		commitment = static_cast<int32_t>(vocabulary[static_cast<size_t>(verb)].strength & 255) * magnitude >> 8;
	}
	if (!brain) {
		return;
	}
	if (category >= 0 && category < static_cast<int32_t>(attention_memory.size()) && g_engine_context.objects->try_get(attention_memory[static_cast<size_t>(category)].object)) {
		brain_add_input(*brain, LOBE_STIMULUS_SOURCE, static_cast<uint32_t>(category), activation);
	}
	if (action < 0) {
		return;
	}
	// a verb about something else waits (up to 5 attention updates) until that's attended to
	if (category >= 0 && (it ? it_category : 0) != category) {
		pending_verb = action;
		pending_verb_category = category;
		pending_verb_updates_left = 5;
		pending_verb_boost = commitment;
		return;
	}
	brain_add_input(*brain, LOBE_VERB, static_cast<uint32_t>(action), commitment);
}

int32_t Creature::look_target() {
	// "look", as LibreCreatures has it: the object on the hand's side of the creature, inside
	// its limits and within 500 px of it, horizontally nearest the hand and under 150 px from
	// it; otherwise the hand itself
	auto* hand = g_engine_context.objects->try_get(g_engine_context.pointer->m_pointer_tool);
	if (!hand) {
		return -1;
	}
	const int32_t hand_category = attention_category(*hand);
	const float creature_x = get_bbox().x, pointer_x = hand->get_bbox().x;
	Object* best = nullptr;
	float nearest = 1e9f;
	for (auto* obj : *g_engine_context.objects) {
		if (obj == this || obj == hand || (obj->attr & ATTR_INVISIBLE) || attention_category(*obj) < 0) {
			continue;
		}
		const auto b = obj->get_bbox();
		const float x = b.x + b.width / 2, y = b.y + b.height / 2;
		const bool hand_side = (pointer_x < creature_x && x < creature_x) || (creature_x < pointer_x && creature_x < x);
		if (!hand_side || std::abs(x - creature_x) >= 500 || !limit.has_point(x, y)) {
			continue;
		}
		if (std::abs(pointer_x - x) < nearest) {
			nearest = std::abs(pointer_x - x);
			best = obj;
		}
	}
	if (!best || nearest >= 150) {
		return hand_category;
	}
	if (brain && hand_category >= 0) {
		brain_clear_neuron(*brain, LOBE_ATTENTION, static_cast<uint32_t>(hand_category));
	}
	return attention_category(*best);
}

std::string Creature::said_word(size_t entry) const {
	return entry < vocabulary.size() ? vocabulary[entry].out : "";
}

// --- voices (batch 3, Q26) ---

bool g_creature_speech_audible = true;

void Creature::load_voice() {
	// A .vce file is 32 syllables (a 4-character sound name and a 32-bit length in ticks)
	// followed by a table of three 32-bit masks for each of a-z and space. Norns use
	// male.vce or female.vce; any other creature uses grendel.vce.
	const char* file = genus != 1 ? "grendel.vce" : sex == 2 ? "female.vce" : "male.vce";
	auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, file);
	if (path.empty()) {
		fmt::print("WARN [Voice] Couldn't find {}\n", file);
		return;
	}
	FileReader in(path);
	for (auto& v : voices) {
		in.read(v.name.data(), v.name.size());
		v.delay_ticks = read32le(in);
	}
	for (auto& letter : voices_lookup) {
		for (auto& mask : letter) {
			mask = read32le(in);
		}
	}
}

int32_t Creature::voice(const std::string& text) {
	// Only the letters a-z and single spaces are kept. Each letter, with the letters before
	// and after it, picks a syllable; they play one after another. How a syllable is picked
	// isn't in the specs: here it's the first sounding syllable allowed by all three masks
	// (the previous letter's first mask, the letter's second and the next letter's third),
	// or by the letter's own mask alone.
	std::string letters;
	for (char ch : text) {
		char c = to_ascii_lowercase(ch);
		if (c >= 'a' && c <= 'z') {
			letters += c;
		} else if (!letters.empty() && letters.back() != ' ') {
			letters += ' ';
		}
	}
	while (!letters.empty() && letters.back() == ' ') {
		letters.pop_back();
	}
	auto index = [](char c) { return c == ' ' ? size_t(26) : static_cast<size_t>(c - 'a'); };
	auto pick = [](uint32_t mask) {
		for (uint32_t bit = 4; bit < 32; ++bit) {
			if (mask & (1u << bit)) {
				return static_cast<int32_t>(bit);
			}
		}
		return -1;
	};
	speaking.clear();
	uint32_t at = g_engine_context.world->tick;
	for (size_t i = 0; i < letters.size(); ++i) {
		const size_t c = index(letters[i]);
		const size_t before = index(i > 0 ? letters[i - 1] : ' ');
		const size_t after = index(i + 1 < letters.size() ? letters[i + 1] : ' ');
		int32_t syllable = 0; // a pause
		if (c != 26) {
			syllable = pick(voices_lookup[before][0] & voices_lookup[c][1] & voices_lookup[after][2]);
			if (syllable < 0) {
				syllable = pick(voices_lookup[c][1]);
			}
			if (syllable < 0) {
				syllable = 0;
			}
		}
		const auto& v = voices[static_cast<size_t>(syllable)];
		if (v.name[0] != 0) {
			speaking.push_back({std::string(v.name.begin(), v.name.end()), at});
		}
		at += v.delay_ticks;
	}
	return static_cast<int32_t>(at - g_engine_context.world->tick);
}

void Creature::update_voice() {
	const uint32_t now = g_engine_context.world->tick;
	while (!speaking.empty() && speaking.front().start_tick <= now) {
		if (g_creature_speech_audible && !dead) {
			auto sound = g_engine_context.sounds->play_controlled_sound(speaking.front().sound, get_bbox());
			if (sound) {
				current_sound.fade_out();
				current_sound = std::move(sound);
			}
		}
		speaking.erase(speaking.begin());
	}
}

Bubble* make_speech_bubble(Object& speaker, const std::string& text, int32_t ticks, bool thought) {
	// Batch 4, Q18: the bubble's top is 25 px above the speaker's top. A speaker in the left
	// half of the view gets a bubble to its right (tail on the left, frame 10, or 12 for a
	// thought); otherwise the bubble's left edge is 160 px left of the speaker (tail on the
	// right, frame 9 or 11). Class 2 1 2, plane 9000, invisible to creatures.
	auto handle = g_engine_context.objects->add<Bubble>();
	auto* bubble = static_cast<Bubble*>(g_engine_context.objects->try_get(handle));
	bubble->family = 2;
	bubble->genus = 1;
	bubble->species = 2;
	bubble->attr = ATTR_INVISIBLE;
	bubble->movement_status = MOVEMENT_AUTONOMOUS;
	bubble->limit = speaker.limit;
	bubble->actv = ACTV_INACTIVE;
	bubble->tick_value = bubble->timer_countdown = 0;
	bubble->obv0 = bubble->obv1 = bubble->obv2 = 0;
	bubble->z_order = BUBBLE_PLANE;
	bubble->click_bhvr = {-1, -1, -1};
	bubble->touch_bhvr = 0;
	const Rect2f at = speaker.get_bbox();
	const auto& camera = g_engine_context.viewport->get_main_camera_rect();
	float in_view = at.x - static_cast<float>(camera.x);
	// the shortest way round the world from the view's left edge
	if (in_view < -CREATURES1_WORLD_WIDTH / 2) {
		in_view += CREATURES1_WORLD_WIDTH;
	} else if (in_view > CREATURES1_WORLD_WIDTH / 2) {
		in_view -= CREATURES1_WORLD_WIDTH;
	}
	const bool to_the_right = in_view < static_cast<float>(camera.width) / 2;
	const int32_t frame = (to_the_right ? BUBBLE_FRAME_TAIL_LEFT : BUBBLE_FRAME_TAIL_RIGHT) + (thought ? 2 : 0);
	const auto& gallery = g_engine_context.images->get_image("syst", frame, 1, ImageManager::IMAGE_SPR);
	const float x = to_the_right ? at.right() : at.x - 160;
	bubble->part = DullPart(gallery, 0, x, at.y - 25, BUBBLE_PLANE);
	bubble->creator = speaker.uid;
	bubble->text = text.substr(0, BUBBLE_MAX_TEXT);
	bubble->life = static_cast<uint8_t>(std::clamp(ticks, 0, 255));
	bubble->offset_x = x - at.x;
	bubble->offset_y = -25;
	bubble->has_offset = true;
	return bubble;
}

int32_t Creature::say(const std::string& text, int32_t ticks) {
	if (text.empty() || !body) {
		return 0;
	}
	auto* bubble = make_speech_bubble(*this, text, NEED_BUBBLE_TICKS);
	fmt::print("INFO [Speech] {} ({}) says {:?}\n", history_name, moniker, bubble->text);
	const int32_t length = voice(bubble->text);
	if (ticks == BUBBLE_TICKS_VOICE_LENGTH) {
		ticks = length + 5;
	}
	bubble->life = static_cast<uint8_t>(std::clamp(ticks, 1, 255));
	return length;
}

void Creature::say_need() {
	// Q23: a sentence for the strongest drive, or "<name> <praise word>" if all are low
	if (asleep || dead) {
		return;
	}
	const std::string name = said_word(VOCAB_NAME);
	// "come Norn" and "run Norn" name its own kind (LibreCreatures, after the original)
	const int32_t own_category = attention_category(*this);
	const std::string own_kind = own_category >= 0 ? said_word(VOCAB_NAME + static_cast<size_t>(own_category)) : "";
	size_t strongest = static_cast<size_t>(std::max_element(loci.drives.begin(), loci.drives.end()) - loci.drives.begin());
	std::vector<std::string> words;
	if (loci.drives[strongest] < 64) {
		words = {name, said_word(72)};
	} else {
		switch (strongest) {
			case 0: words = {name, said_word(56)}; break;
			case 1: words = {said_word(6), said_word(72), name}; break;
			case 2: words = {name, said_word(22)}; break;
			case 3: words = {name, said_word(59)}; break;
			case 4: words = {name, said_word(60)}; break;
			case 5: words = {name, said_word(61)}; break;
			case 6: words = {name, said_word(62)}; break;
			case 7: words = {said_word(4), own_kind}; break;
			case 8: words = {said_word(5), own_kind}; break;
			case 9: words = {name, said_word(5)}; break;
			case 10: words = {name, said_word(66)}; break;
			case 11: words = {name, said_word(67)}; break;
			// the sex drive, and the unused drives 13-15 (LibreCreatures)
			default: words = {name, said_word(56 + strongest)}; break;
		}
	}
	std::string text;
	for (const auto& w : words) {
		if (!w.empty()) {
			text += (text.empty() ? "" : " ") + w;
		}
	}
	say(text, NEED_BUBBLE_TICKS);
}

void Creature::say_what_doing() {
	// As LibreCreatures has it, after the original: one time in three the phrase starts with
	// the creature's name, then comes the verb for what it's doing, and one time in three, for
	// an action on something, the noun for what it attends to. Each creature in earshot learns
	// each word from it, its own kind's noun with the name and the others once the voice has
	// finished, and may say it back. That's all: they aren't told what to do.
	static constexpr std::array<uint8_t, 16> VERB = {74, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 255, 255, 255, 255};
	if (asleep || dead || current_decision < 0 || current_decision >= 16 || VERB[static_cast<size_t>(current_decision)] == 255) {
		return;
	}
	const size_t verb = VERB[static_cast<size_t>(current_decision)];
	auto* it = g_engine_context.objects->try_get(attention);
	const int32_t it_category = it ? attention_category(*it) : -1;
	const int32_t own_category = attention_category(*this);
	std::string text;
	std::vector<std::pair<size_t, bool>> taught; // the entry, and whether it waits for the voice
	if (rand_int32(0, 2) == 0) {
		text = said_word(VOCAB_NAME) + " ";
		if (own_category >= 0) {
			taught.push_back({VOCAB_NAME + static_cast<size_t>(own_category), false});
		}
	}
	text += said_word(verb);
	if (it_category >= 0 && current_decision <= 7 && rand_int32(0, 2) == 0) {
		text += " " + said_word(VOCAB_NAME + static_cast<size_t>(it_category));
		taught.push_back({VOCAB_NAME + static_cast<size_t>(it_category), true});
	}
	taught.push_back({verb, true});
	text = text.substr(0, 24);
	// the bubble lasts as long as the voice plus 5 ticks (batch 3, Q26)
	const int32_t length = say(text, BUBBLE_TICKS_VOICE_LENGTH);
	for (auto* obj : *g_engine_context.objects) {
		if (auto* other = obj->as_creature(); other && other != this && other->can_hear(*this)) {
			for (const auto& [entry, after_voice] : taught) {
				other->words_to_hear.push_back({entry, vocabulary[entry].in, uid, after_voice ? length : 0});
			}
		}
	}
}
