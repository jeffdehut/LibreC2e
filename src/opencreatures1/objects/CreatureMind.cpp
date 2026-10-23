// A creature's mind: what it notices and remembers, stimuli, the brain step, decisions and
// involuntary actions. Behaviour follows the clean-room specs in
// `Clean Specs/BATCH1-ANSWERS.md`; the question numbers (Q1, Q2, ...) refer to that file.

#include "Creature.h"

#include "CompoundObject.h"
#include "ImageManager.h"
#include "SimpleObject.h"
#include "Vehicle.h"
#include "CreatureBrain.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "ObjectManager.h"
#include "PointerManager.h"
#include "Scriptorium.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "common/Random.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "fileformats/sfc/CInstinct.h"

#include <algorithm>
#include <cmath>
#include <ctime>

void creature_tick_pose(Creature& c);

namespace {

constexpr int32_t ATTENTION_CATEGORIES = 40;
constexpr int32_t SIGHT_RANGE = 500;
constexpr int32_t HEARING_RANGE = 500;
constexpr float REACH = 55;

// genetic stimulus numbers used by the engine (Q1, Q23)
constexpr int32_t STIMULUS_DISAPPOINTMENT = 0;
constexpr int32_t STIMULUS_NEARER = 5;
constexpr int32_t STIMULUS_FARTHER = 6;
constexpr int32_t STIMULUS_PUSHED_BY_LIMIT = 7;
constexpr int32_t STIMULUS_SEEN = 8;

// general sense lobe neurons set by the engine (Q1)
constexpr uint32_t SENSE_LIMIT_AHEAD = 3;
constexpr uint32_t SENSE_CARRIED = 4;
constexpr uint32_t SENSE_IT_NEAR = 12;
constexpr uint32_t SENSE_IT_ACTIVE = 13;
constexpr uint32_t SENSE_IT_IS_CREATURE = 15;
constexpr uint32_t SENSE_IT_SIBLING = 16;
constexpr uint32_t SENSE_IT_IS_PARENT = 17;
constexpr uint32_t SENSE_IT_IS_CHILD = 18;
constexpr uint32_t SENSE_IT_OPPOSITE_SEX = 19;

// decisions (Q6)
constexpr int32_t DECISION_QUIESCENT = 0;
constexpr int32_t DECISION_REST = 9;

bool rects_overlap(const Rect2f& a, const Rect2f& b) {
	return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

Rect2f to_rectf(const Rect2i& r) {
	return Rect2f{numeric_cast<float>(r.x), numeric_cast<float>(r.y), numeric_cast<float>(r.width), numeric_cast<float>(r.height)};
}

Rect2i room_rect_at(float x, float y) {
	if (const Room* room = g_engine_context.map->find_room_containing(x, y)) {
		return Rect2i{room->left, room->top, room->width(), room->height()};
	}
	return Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
}

// The point a creature aims at on an object (Q10).
bool target_point_of(const Object* obj, int32_t aim, float* x, float* y) {
	if (!obj) {
		return false;
	}
	auto bbox = obj->get_bbox();
	*x = bbox.x + bbox.width / 2;
	*y = bbox.y + bbox.height / 2;
	if (auto* comp = obj->as_compound_object()) {
		// the hotspot KNOB assigned to the function AIM: picked, measured from part 0
		if (aim >= 0 && aim < 3) {
			int32_t spot = comp->functions_to_hotspots[static_cast<size_t>(aim)];
			if (spot >= 0 && spot < 6) {
				const auto& h = comp->hotspots[static_cast<size_t>(spot)];
				if (h.width > 0 && h.height > 0 && !comp->parts.empty()) {
					*x = comp->parts[0].x() + numeric_cast<float>(h.x) + numeric_cast<float>(h.width) / 2;
					*y = comp->parts[0].y() + numeric_cast<float>(h.y) + numeric_cast<float>(h.height) / 2;
				}
			}
		}
	} else if (auto* creature = obj->as_creature()) {
		if (aim != 1 && creature->head) {
			// the outer end of its head
			SkeletonPose pose;
			pose.body_data = &creature->body->body_data;
			pose.body_frame = creature->body->pose();
			for (Limb* l = creature->head.get(); l; l = l->next_limb.get()) {
				pose.chains[CHAIN_HEAD].push_back({&l->limb_data, l->pose()});
			}
			auto tip = skeleton_chain_tip(pose, CHAIN_HEAD);
			*x = creature->body->x() + numeric_cast<float>(tip.x);
			*y = creature->body->y() + numeric_cast<float>(tip.y);
		}
		if (aim == 1 && creature->body) {
			// the centre of the body sprite (batch 2, Q6)
			*x = creature->body->x() + numeric_cast<float>(creature->body->width()) / 2;
			*y = creature->body->y() + numeric_cast<float>(creature->body->height()) / 2;
		}
	}
	return true;
}

uint8_t clamp_byte(int32_t value) {
	return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// Finds the classifier whose script for `event` would run, trying an exact match, then
// species 0, then genus and species 0 (Q9).
bool script_exists(uint8_t family, uint8_t genus, uint8_t species, ScriptNumber event) {
	auto& s = *g_engine_context.scriptorium;
	return !s.get(family, genus, species, event).empty() || !s.get(family, genus, 0, event).empty() || !s.get(family, 0, 0, event).empty();
}

} // namespace

int32_t Creature::attention_category(const Object& obj) {
	int32_t category = 0;
	switch (obj.family) {
		case 2: if (obj.genus >= 25) return -1; category = obj.genus; break;
        case 3: if (obj.genus >= 10) return -1; category = 25 + obj.genus; break;
        case 4: if (obj.genus >= 5) return -1; category = 35 + obj.genus; break;
		default: return 0;
	}
	// a genus too large for its family is simply invalid (Q4)
	return category < ATTENTION_CATEGORIES ? category : -1;
}

// --- moving about (batch 2, Q10-Q12, Q16) ---

void Creature::update_limits() {
	auto* by = g_engine_context.objects->try_get(carrier);
	if (by && by->as_pointer_tool()) {
		limit = Rect2i{-100000, -100000, 200000, 200000}; // carried by the hand: unlimited
	} else if (by && by->as_vehicle() && movement_status == MOVEMENT_INVEHICLE) {
		auto cabin = by->as_vehicle()->cabin;
		auto bbox = by->get_bbox();
		limit = Rect2i{numeric_cast<int32_t>(bbox.x) + cabin.x, numeric_cast<int32_t>(bbox.y) + cabin.y, cabin.width, cabin.height};
	} else if (by && by->as_creature()) {
		limit = by->limit;
	} else if (movement_status == MOVEMENT_MOUSEDRIVEN || !(attr & ATTR_WALLBOUND)) {
		limit = Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
	} else {
		// the room whose x range contains its left edge and whose floor is closest to its bottom
		auto bbox = get_bbox();
		limit = g_engine_context.map->limits_for(bbox.x, bbox.bottom(), limit);
	}
}

void Creature::check_standing_on_a_floor() {
	// Product correction: the original leaves misplaced creatures hanging forever.
	// Repair freely standing creatures, including sleepers, without interfering with
	// carriers, scripted initial placement or deliberately floating agents.
	if (!body || unplaced || movement_status != MOVEMENT_AUTONOMOUS ||
		!(attr & ATTR_WALLBOUND) || g_engine_context.objects->try_get(carrier) ||
		g_engine_context.map->get_rooms().empty()) return;
	if (footx < 0 || footx >= CREATURES1_WORLD_WIDTH) reposition();
	const Room* room = g_engine_context.map->find_room_containing(ground_x(), ground_y() - 1);
	Rect2i floor = room ? Rect2i{room->left, room->top, room->width(), room->height()}
		: g_engine_context.map->limits_for(get_bbox().x, ground_y(), limit);
	if (std::abs(footy - floor.bottom()) <= 2 && limit == floor) return;
	fmt::print("INFO [Floor] Settling {} from {},{} onto floor {}\n", moniker, footx, footy, floor.bottom());
	limit = floor;
	footy = floor.bottom();
	reposition();
	settle_on_limits();
	standing_in_the_air = false;
}

void Creature::land() {
	// let go by the hand or a vehicle: stand straight on the floor of the new limits
	movement_status = MOVEMENT_AUTONOMOUS;
	carrier = {};
	update_limits();
	// Not in the original: the room it lands in is the one it was let go in (where its feet
	// are, else the first floor below them), so a Norn put down over a gap between rooms or
	// high in a room never ends up standing in the air.
	{
		auto box = get_body_bbox();
		if (const Room* room = g_engine_context.map->room_to_land_in(box.x + box.width / 2, ground_y() - 1)) {
			limit = Rect2i{room->left, room->top, room->width(), room->height()};
		}
	}
	footx = std::clamp(footx, limit.x, limit.right());
	footy = limit.bottom();
	reposition();
	settle_on_limits();
	pushed_by_limit = false;
}

void Creature::move_ground_to(float x, float y) {
	footx = numeric_cast<int32_t>(x);
	footy = numeric_cast<int32_t>(y);
	reposition();
}

void Creature::die() {
	// batch 2, Q16
	drop_carried_objects();
	dreaming = 0;
	g_engine_context.macros->delete_macros_owned_by(this->uid);
	g_engine_context.macros->queue_script(this, this, SCRIPT_DIE);
    GraveRecord grave;
    grave.moniker = moniker;
    grave.name = history_name.empty() || history_name == "<Un Named>" ? moniker : history_name;
    grave.sex = sex == 2 ? "Female" : "Male";
    const auto seconds = age_ticks / 10;
    grave.age = fmt::format("{}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60, seconds % 60);
    grave.parents = mother == father ? mother : mother + " and " + father;
    grave.born = history_birthday;
    std::time_t now = std::time(nullptr);
    char date[64];
    std::strftime(date, sizeof(date), "%d %B %Y, %H:%M", std::localtime(&now));
    grave.died = date;
    g_engine_context.world->pending_graves.push_back(std::move(grave));
	dead = 255;
	eyes_open = 0;
	update_zzz();
}

void Creature::update_zzz() {
	// batch 3, Q35: sprite zzzz, class 2 3 13, 10 planes in front of the creature, at its
	// left edge 20 px above its top. Norns send it activate 1 and others activate 2; on
	// waking or dying it gets deactivate and is destroyed.
	auto* zzz = g_engine_context.objects->try_get(zzzz);
	const bool wanted = asleep && !dead;
	if (!wanted) {
		if (zzz) {
			g_engine_context.macros->queue_script(this, zzz, SCRIPT_DEACTIVATE);
			g_engine_context.objects->destroy_later(zzzz);
		}
		zzzz = {};
		return;
	}
	auto bbox = get_bbox();
	if (!zzz) {
		zzzz = g_engine_context.objects->add<SimpleObject>();
		auto* obj = static_cast<SimpleObject*>(g_engine_context.objects->try_get(zzzz));
		obj->family = 2;
		obj->genus = 3;
		obj->species = 13;
		obj->movement_status = MOVEMENT_AUTONOMOUS;
		obj->attr = ATTR_INVISIBLE;
		obj->limit = Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
		obj->actv = ACTV_INACTIVE;
		obj->tick_value = obj->ticks_since_last_tick_event = 0;
		obj->obv0 = obj->obv1 = obj->obv2 = 0;
		obj->z_order = numeric_cast<int32_t>(z_order) + 10;
		obj->click_bhvr = {-1, -1, -1};
		obj->touch_bhvr = 0;
		obj->part = DullPart(g_engine_context.images->get_image("zzzz", 0, 10, ImageManager::IMAGE_SPR), 0, bbox.x, bbox.y - 20, obj->z_order);
		const bool norn = genus == 1;
		obj->actv = norn ? ACTV_ACTIVE1 : ACTV_ACTIVE2;
		g_engine_context.macros->queue_script(this, obj, norn ? SCRIPT_ACTIVATE1 : SCRIPT_ACTIVATE2);
		zzz = obj;
	}
	zzz->set_position(bbox.x, bbox.y - 20);
}

// --- senses ---

Rect2i Creature::sight_limits() const {
	auto* by = g_engine_context.objects->try_get(carrier);
	if (by) {
		// carried by anything but the family 3 genus 2 kind: the room around the ground point
		return room_rect_at(ground_x(), ground_y());
	}
	return limit;
}

bool Creature::can_see(const Object& obj) const {
	// Q2: not itself, not invisible, in the direction it faces, overlapping its limits and
	// within 500 px of its ground point (from the object's centre)
	if (&obj == this || obj.unplaced || (obj.attr & ATTR_INVISIBLE) || obj.as_pointer_tool()
        || ground_x() < 0 || ground_x() > CREATURES1_WORLD_WIDTH || ground_y() < 0 || ground_y() > CREATURES1_WORLD_HEIGHT
        || (!obj.as_creature() && !obj.get_part(0))) {
		return false;
	}
	if (auto* by = g_engine_context.objects->try_get(carrier); by && by->family == 3 && by->genus == 2) {
		return false;
	}
    if (const auto* c = obj.as_creature(); c && (c->ground_x() < 0 || c->ground_x() > CREATURES1_WORLD_WIDTH || c->ground_y() < 0 || c->ground_y() > CREATURES1_WORLD_HEIGHT)) return false;
	auto bbox = obj.get_bbox();
    if (bbox.y >= CREATURES1_WORLD_HEIGHT || bbox.bottom() <= 0) return false;
	if (direction == 3 && bbox.x > ground_x()) {
		return false;
	}
	if (direction == 2 && bbox.right() < ground_x()) {
		return false;
	}
	if (!rects_overlap(bbox, to_rectf(sight_limits()))) {
		return false;
	}
	return std::abs(bbox.x + bbox.width / 2 - ground_x()) < SIGHT_RANGE;
}

bool Creature::can_hear(const Object& source) const {
	// Q2: from the source's left and top edges; facing doesn't matter
    if (source.unplaced || ground_x() < 0 || ground_x() > CREATURES1_WORLD_WIDTH || ground_y() < 0 || ground_y() > CREATURES1_WORLD_HEIGHT
        || (!source.as_creature() && !source.get_part(0))) return false;
    if (const auto* c = source.as_creature(); c && (c->ground_x() < 0 || c->ground_x() > CREATURES1_WORLD_WIDTH || c->ground_y() < 0 || c->ground_y() > CREATURES1_WORLD_HEIGHT)) return false;
	auto bbox = source.get_bbox();
    if (bbox.y >= CREATURES1_WORLD_HEIGHT || bbox.bottom() <= 0) return false;
	return std::abs(bbox.x - ground_x()) < HEARING_RANGE && bbox.y > ground_y() - 200 && bbox.y < ground_y() + 32;
}

void Creature::update_target_point() {
	has_target = target_point_of(g_engine_context.objects->try_get(attention), aim_action, &target_x, &target_y);
}

// --- stimuli (Q5, Q22) ---

void Creature::queue_stimulus(const sfc::StimulusV1& stimulus, ObjectHandle from) {
	queued_stimuli.push_back({stimulus, from});
}

void Creature::receive_numbered_stimulus(int32_t number, ObjectHandle from, int32_t engine_value) {
	if (number < 0 || numeric_cast<size_t>(number) >= stimuli.size()) {
		throw Exception(fmt::format("Creature {} has no stimulus {}", moniker, number));
	}
	receive_stimulus(stimuli[numeric_cast<size_t>(number)], from, engine_value);
}

void Creature::receive_stimulus(const sfc::StimulusV1& original, ObjectHandle from, int32_t engine_value) {
	if (dead) {
		return;
	}
	int32_t significance = original.significance;
	int32_t input = original.input;
	int32_t intensity = original.intensity;
	std::array<int32_t, 4> amounts = {original.amount0, original.amount1, original.amount2, original.amount3};
	const std::array<uint8_t, 4> chems = {original.chem0, original.chem1, original.chem2, original.chem3};

	// features: bit 1 modulates by the engine value, bit 2 adds it to the input, bit 4 reaches
	// sleepers at half strength (chemicals unchanged)
	const int32_t value = std::clamp(engine_value, 0, 255);
	if (original.flags & 1) {
		significance = significance * value / 256;
		intensity = intensity * value / 256;
		for (auto& a : amounts) {
			a = a * value / 256;
		}
	} else if (original.flags & 2) {
		// the sense neuron moves by the value, a byte that wraps (as LibreCreatures has it,
		// checked against the original: and only when the stimulus isn't modulated)
		input = static_cast<uint8_t>(input + static_cast<int8_t>(static_cast<uint8_t>(engine_value)));
	}
	if (std::getenv("OPENCREATURES1_TRACE_MEALS") && std::find(chems.begin(), chems.end(), 57) != chems.end()) {
		fmt::print("TRACE [Meal] {} gets food chemicals: asleep {}, dreaming {}, glycogen {}, starch {} +{}\n", moniker, static_cast<int>(asleep), static_cast<int>(dreaming),
			biochemistry ? biochemistry->chemicals[59].concentration : -1, biochemistry ? biochemistry->chemicals[57].concentration : -1, amounts[static_cast<size_t>(std::find(chems.begin(), chems.end(), 57) - chems.begin())]);
	}
	if (asleep) {
		if (!(original.flags & 4)) {
			return;
		}
		significance /= 2;
		intensity /= 2;
	}

	const bool from_self = from == this->uid;
	auto* source = g_engine_context.objects->try_get(from);
	const int32_t category = source ? attention_category(*source) : -1;

	// the source's memory entry; a self-stimulus clears the entry for its own category
	if (from_self) {
		int32_t own = attention_category(*this);
		if (own >= 0) {
			auto& e = attention_memory[static_cast<size_t>(own)];
			const bool ever = e.ever_seen;
			e = AttentionEntry{};
			e.ever_seen = ever;
		}
	} else if (category >= 0) {
		auto& e = attention_memory[static_cast<size_t>(category)];
		// An object `kill` has parked below the world (eaten food, whose stimulus can arrive
		// after) is gone: it doesn't become the thing to attend to, and where that kind of
		// object was is kept (LibreCreatures' fix 9). The stimulus still has its effects.
		float x = 0, y = 0;
		const bool placed = target_point_of(source, 0, &x, &y);
		if (placed && y >= CREATURES1_WORLD_HEIGHT) {
			if (e.object == from) {
				e.object = {};
			}
		} else {
			e.object = from;
			e.has_position = placed;
			e.last_x = x;
			e.last_y = y;
		}
		e.input = static_cast<uint8_t>(input);
		e.intensity = clamp_byte(intensity);
		e.stimulated_since_update = true;
	}

	if (brain) {
		if (category >= 0) {
			brain_add_input(*brain, LOBE_STIMULUS_SOURCE, static_cast<uint32_t>(category), significance);
		}
		const bool about_it = (source && from == attention) || (from_self && !g_engine_context.objects->try_get(attention));
		if (about_it && input < 32) {
			brain_add_input(*brain, LOBE_GENERAL_SENSE, static_cast<uint32_t>(input), intensity);
		}
	}

	if (biochemistry) {
		for (size_t i = 0; i < chems.size(); ++i) {
			if (chems[i] != 0) {
				auto& c = biochemistry->chemicals[chems[i]].concentration;
				c = clamp_byte(c + amounts[i]);
			}
		}
	}

	// chemicals 33-48 teach the creature what each category is good for (Q1)
	if (!from_self && category >= 0) {
		for (size_t i = 0; i < chems.size(); ++i) {
			if (chems[i] >= 33 && chems[i] <= 48) {
				auto& avg = learned_chemicals[static_cast<size_t>(category)][chems[i] - 33u];
				avg = clamp_byte((amounts[i] + 4 * avg) / 5);
			}
		}
	}
}

// --- noticing things (Q1, Q3) ---

void Creature::sight_scan() {
	auto engine_stimulus = [&](int32_t number, Object* obj, int32_t value) {
		receive_numbered_stimulus(number, obj->uid, value);
	};

	// new objects
	for (auto* obj : *g_engine_context.objects) {
		int32_t category = attention_category(*obj);
		if (obj == this || category <= 0) {
			continue;
		}
		auto& e = attention_memory[static_cast<size_t>(category)];
		auto* remembered = g_engine_context.objects->try_get(e.object);
		if (!remembered) {
			if (can_see(*obj)) {
				const bool ever = e.ever_seen;
				engine_stimulus(STIMULUS_SEEN, obj, ever ? 80 : 180);
				e.object = obj->uid;
				e.in_view = true;
				e.ever_seen = true;
			}
		} else if (remembered != obj && !e.in_view && e.object != attention && can_see(*obj)) {
			const bool ever = e.ever_seen;
			engine_stimulus(STIMULUS_SEEN, obj, ever ? 80 : 180);
			e.object = obj->uid;
			e.in_view = true;
			e.ever_seen = true;
			if (brain) {
				brain_clear_neuron(*brain, LOBE_ATTENTION, static_cast<uint32_t>(category));
			}
		}
	}

	// remembered objects: back in view, out of view, moving nearer or farther
	for (int32_t category = 1; category < ATTENTION_CATEGORIES; ++category) {
		auto& e = attention_memory[static_cast<size_t>(category)];
		auto* obj = g_engine_context.objects->try_get(e.object);
		if (!obj) {
			continue;
		}
		const bool visible = can_see(*obj);
		if (!e.in_view && visible) {
			engine_stimulus(STIMULUS_SEEN, obj, e.ever_seen ? 26 : 60);
			e.in_view = true;
			e.ever_seen = true;
		} else if (!visible) {
			e.in_view = false;
		}
		// where it is now is remembered whether or not it can be seen; one parked below the
		// world by `kill` keeps where it last really was (LibreCreatures, fix 9)
		float x, y;
		if (target_point_of(obj, 0, &x, &y) && y < CREATURES1_WORLD_HEIGHT) {
			if (visible && e.has_position && std::abs(x - e.last_x) > 32) {
				const bool nearer = std::abs(x - ground_x()) < std::abs(e.last_x - ground_x());
				engine_stimulus(nearer ? STIMULUS_NEARER : STIMULUS_FARTHER, obj, numeric_cast<int32_t>(std::abs(x - e.last_x)));
			}
			e.last_x = x;
			e.last_y = y;
			e.has_position = true;
		}
	}
}

void Creature::choose_attention() {
	if (!brain) {
		return;
	}
	// the strongest category with a remembered object, against category 0 (Q3)
	int32_t winner = 0;
	auto* zero = brain_neuron(*brain, LOBE_ATTENTION, 0);
	uint8_t best = zero ? zero->output : 0;
	for (int32_t category = 1; category < ATTENTION_CATEGORIES; ++category) {
		auto* n = brain_neuron(*brain, LOBE_ATTENTION, static_cast<uint32_t>(category));
		if (n && n->output > best && g_engine_context.objects->try_get(attention_memory[static_cast<size_t>(category)].object)) {
			best = n->output;
			winner = category;
		}
	}
	ObjectHandle new_it = winner > 0 ? attention_memory[static_cast<size_t>(winner)].object : ObjectHandle{};
	if (!g_engine_context.objects->try_get(new_it)) {
		new_it = {};
	}
	// Not in the original: a Norn finishes a meal it has started. In company, other Norns
	// noticed again after every self-stimulus took its attention mid-meal so often that Norns
	// starved beside their food (batch 8). Attention stays on the food while its eat script
	// runs within reach, for at most 128 ticks. Approaches and stalled meals can be abandoned.
	if (new_it != attention && g_engine_context.objects->try_get(attention) && g_engine_context.macros->is_eating(this->uid, attention)) {
		new_it = attention;
	}
	// As LibreCreatures has it (checked against the original): asleep, a creature's attention
	// doesn't turn to nothing.
	if (!new_it && asleep) {
		new_it = attention;
	}

	if (new_it != attention) {
		attention = new_it;
		aim_action = 0;
		update_target_point();
		brain_clear_lobe(*brain, LOBE_DECISION);
		brain_clear_lobe(*brain, LOBE_GENERAL_SENSE);
		brain_clear_lobe(*brain, LOBE_STIMULUS_SOURCE);
		auto& e = attention_memory[static_cast<size_t>(winner)];
		if (winner > 0 && e.stimulated_since_update && e.input < 32) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, e.input, e.intensity);
		}
		brain_add_input(*brain, LOBE_STIMULUS_SOURCE, static_cast<uint32_t>(winner), 30);
		start_decision(DECISION_QUIESCENT);
		if (pending_verb >= 0 && pending_verb_category == winner && pending_verb_updates_left > 0) {
			brain_add_input(*brain, LOBE_VERB, static_cast<uint32_t>(pending_verb), pending_verb_boost);
			pending_verb = -1;
		}
	}
	if (pending_verb_updates_left > 0 && --pending_verb_updates_left == 0) {
		pending_verb = -1;
	}
	// The decision lobe's winner-takes-all leaves out the actions the creature can't take
	// now (those that need something to attend to, or nothing), so the strongest it can take
	// wins (LibreCreatures, as the original sets them when attention changes).
	{
		const bool has_it = g_engine_context.objects->try_get(attention) != nullptr;
		for (uint32_t d = 0; d < 16; ++d) {
			if (auto* n = brain_neuron(*brain, LOBE_DECISION, d)) {
				const bool needs_it = d >= 1 && d <= 6, needs_none = d == DECISION_REST, never = d >= 12;
				const bool eligible = !never && (needs_it ? has_it : needs_none ? !has_it : true);
				n->wta_disable = eligible ? 0 : 1;
			}
		}
	}

	for (auto& e : attention_memory) {
		e.input = 255;
		e.stimulated_since_update = false;
	}
}

void Creature::forget_out_of_reach() {
	// Q1: forget objects whose target point is outside the creature's limits (its carrier's,
	// if it's carried by anything but the family 3 genus 2 kind)
	Rect2i limits = limit;
	if (auto* by = g_engine_context.objects->try_get(carrier); by && !(by->family == 3 && by->genus == 2)) {
		limits = by->limit;
	}
	auto bounds = to_rectf(limits);
	for (int32_t category = 0; category < ATTENTION_CATEGORIES; ++category) {
		auto& e = attention_memory[static_cast<size_t>(category)];
		float x, y;
		if (target_point_of(g_engine_context.objects->try_get(e.object), 0, &x, &y) && !(x >= bounds.x && x < bounds.right() && y >= bounds.y && y < bounds.bottom())) {
			const bool ever = e.ever_seen;
			e = AttentionEntry{};
			e.ever_seen = ever;
			if (brain) {
				brain_clear_neuron(*brain, LOBE_ATTENTION, static_cast<uint32_t>(category));
			}
		}
	}
}

// --- life cycle (batch 2, Q14, Q17, Q20, Q21) ---

void Creature::age_update() {
	// every 128 ticks: move on a life stage when that stage's locus is set
	if (age >= 7 || !loci.life_stage[std::min<size_t>(age, 6)]) {
		return;
	}
	age++;
	if (age >= 7) {
		die();
		return;
	}
	fmt::print("INFO [Life] {} ({}) is now life stage {}\n", history_name, moniker, age);
	if (!genome) {
		genome = load_genome(moniker);
	}
	if (genome) {
		express_genes(*this, *genome, age);
		breeds = appearance_breeds(*genome, sex);
	}
	// rebuild the body for the new stage
	try {
		build_body(*this, breeds);
	} catch (std::exception& e) {
		fmt::print("WARN [Life] couldn't rebuild {}'s body for stage {}: {}\n", moniker, age, e.what());
	}
}

void Creature::dream_tick() {
	// While dreaming, each tick moves the first queued instinct one step on (Q14, Q20).
	if (instincts.empty() || !brain) {
		dreaming = 0;
		return;
	}
	auto& instinct = *instincts.front();
	auto reset_brain = [&] {
		for (auto& n : brain->neurons) {
			n.state = n.output = 0;
			for (auto* list : {&n.dendrites0, &n.dendrites1}) {
				for (auto& d : *list) {
					d.suscept = 0;
					d.stw = d.ltw;
				}
			}
		}
	};
	const uint32_t step = instinct.phase;
	if (step == 0) {
		reset_brain();
	} else if (step <= 39) {
		if (step >= 35 && biochemistry && instinct.reinforcement_chemical < 256) {
			biochemistry->chemicals[instinct.reinforcement_chemical].concentration = static_cast<uint8_t>(std::min<uint32_t>(instinct.reinforcement_amount, 255));
		}
		if (biochemistry) {
			creature_biochemistry_tick(*biochemistry, loci, brain.get(), dead, biotick);
		}
		for (uint32_t lobe = 0; lobe <= 7; ++lobe) {
			brain_clear_lobe(*brain, lobe);
		}
		for (const auto& d : instinct.dendrites) {
			if (d.lobe) {
				brain_set_input(*brain, d.lobe, d.cell, 255);
			}
		}
		if (auto* n = brain_neuron(*brain, LOBE_DECISION, instinct.motor_decision)) {
			n->state = n->output = 255;
		}
		brain_tick(*brain, biotick, brain_migration);
	} else {
		// learnt: the long-term weights take what the instinct taught
		for (auto& n : brain->neurons) {
			for (auto* list : {&n.dendrites0, &n.dendrites1}) {
				for (auto& d : *list) {
					d.ltw = std::max(d.ltw, d.stw);
				}
			}
		}
		reset_brain();
		instincts.erase(instincts.begin());
		if (instincts.empty()) {
			dreaming = 0;
		} else if (age > 0 && dreaming > 0) {
			dreaming--;
		}
		return; // (this step doesn't count as a biochemistry tick, as in LibreCreatures)
	}
	instinct.phase++;
	biotick++;
}

void Creature::update_fertility() {
	// every 128 ticks (batch 7): fertile above 160, not below 80; the emitters read 255 while
	// fertile or pregnant (Q21)
	if (loci.fertility > 160) {
		gamete = moniker;
	} else if (loci.fertility < 80) {
		gamete.clear();
	}
	loci.fertile_output = gamete.empty() ? 0 : 255;
	loci.pregnant_output = zygote.empty() ? 0 : 255;
}

void Creature::mate() {
	auto* it = g_engine_context.objects->try_get(attention);
	auto* female = it ? it->as_creature() : nullptr;
	if (!female || female->sex != 2 || female->family != family || female->genus != genus) {
		return;
	}
	fmt::print("INFO [Life] {} ({}) and {} ({}) try for a child: he is {}fertile, she is {}fertile{}, conception chance {}\n", history_name, moniker,
		female->history_name, female->moniker, gamete.empty() ? "not " : "", female->gamete.empty() ? "not " : "", female->zygote.empty() ? "" : " and already pregnant", female->loci.conception_chance);
	if (!gamete.empty() && !female->gamete.empty() && female->zygote.empty() && rand_int32(0, 255) < female->loci.conception_chance) {
		std::string child = make_child_genome(female->moniker, moniker);
		if (!child.empty()) {
			female->zygote = child;
			fmt::print("INFO [Life] {} ({}) conceived with {} ({}): {}\n", female->history_name, female->moniker, history_name, moniker, child);
		}
	}
	gamete.clear();
}

// --- disease (batch 2, Q15) ---

WorldState::Bacterium mutate_bacterium(WorldState::Bacterium b) {
	const int32_t roll = rand_int32(0, 100);
	if (roll >= 51 && roll <= 60) {
		b.antigen = static_cast<uint8_t>(rand_int32(248, 255));
	} else if (roll >= 61 && roll <= 70) {
		b.fatal_level = static_cast<uint8_t>(std::clamp(b.fatal_level + rand_int32(-10, 10), 130, 255));
	} else if (roll >= 71 && roll <= 80) {
		b.infect_level = static_cast<uint8_t>(std::clamp(b.infect_level + rand_int32(-10, 10), 0, 130));
	} else if (roll >= 81) {
		b.toxins[static_cast<size_t>(rand_int32(0, 3))] = static_cast<uint8_t>(rand_int32(232, 235));
	}
	b.state = 1; // incubating
	return b;
}

void disease_world_tick() {
	if (g_engine_context.world->tick % 128 != 0) {
		return;
	}
	// The chance is also 1 in 251 while a running average, new = (reading + 4 x old) / 5, is
	// below 100 (batch 3, Q40). LibreCreatures has the readings as the program's idle cycles,
	// which are that low only on a busy machine, so that's left out. As LibreCreatures has it,
	// it's 1 in 251 with more than 24 creatures too, and dead ones can be picked.
	std::vector<Creature*> creatures;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature()) {
			creatures.push_back(c);
		}
	}
	if (creatures.empty() || rand_int32(0, creatures.size() > 24 ? 250 : 500) != 0) {
		return;
	}
	auto* c = creatures[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(creatures.size()) - 1))];
	if (c->bacterium.state != 2) {
		c->bacterium = mutate_bacterium(g_engine_context.world->bacteria[static_cast<size_t>(rand_int32(0, 99))]);
	}
}

void Creature::disease_update() {
	if (!bacterium.state || !biochemistry) {
		return;
	}
	auto& chems = biochemistry->chemicals;
	// the antibody for antigen A is chemical A - 8 (in a byte, as LibreCreatures has it)
	const uint8_t antibody = chems[static_cast<uint8_t>(bacterium.antigen - 8u)].concentration;
	if (antibody > bacterium.fatal_level) {
		bacterium.state = 0; // cured
		return;
	}
	if (bacterium.state == 1 && antibody < bacterium.infect_level) {
		bacterium.state = 2; // active
	}
	auto add = [&](uint8_t chem, int32_t amount) {
		if (chem) {
			chems[chem].concentration = clamp_byte(chems[chem].concentration + amount);
		}
	};
	add(bacterium.antigen, 80);
	for (auto toxin : bacterium.toxins) {
		add(toxin, 30);
	}
}

void Creature::sneeze() {
	if (!bacterium.state) {
		return;
	}
	for (auto* obj : *g_engine_context.objects) {
		auto* other = obj->as_creature();
		if (other && other != this && can_see(*other)) {
			// as LibreCreatures has it: a creature already ill isn't caught again
			if (rand_int32(0, 4) == 0 && other->bacterium.state <= 1) {
				other->bacterium = mutate_bacterium(bacterium);
			}
			return;
		}
	}
	// nobody in view: it may spread into the world. As LibreCreatures reads the original, the
	// world's bacteria move down a place and a mutated copy takes the last.
	if (rand_int32(0, 10) == 0) {
		auto& world = g_engine_context.world->bacteria;
		std::move(world.begin() + 1, world.end(), world.begin());
		world.back() = mutate_bacterium(bacterium);
	}
}

// --- the face (batch 2, Q4-Q5) ---

bool g_creature_angry_face = false;

void Creature::update_expression() {
	// 1 when every drive that counts is at or below 128, 2 when one is above 192, else 0.
	// Drives 10 and 12-15 don't count.
	int32_t result = 1;
	for (size_t d = 0; d < loci.drives.size(); ++d) {
		if (d == 10 || d >= 12) {
			continue;
		}
		if (loci.drives[d] > 192) {
			result = 2;
			break;
		}
		if (loci.drives[d] > 128) {
			result = 0;
		}
	}
	// 3, the angry face: Anger (drive 11) above 192 and stronger than every other drive that
	// counts; a tie goes to the other drive, so a Norn whose worst need is something else
	// still looks sad (LibreCreatures row 88)
	constexpr size_t ANGER = 11;
	if (g_creature_angry_face && loci.drives[ANGER] > 192) {
		bool strongest = true;
		for (size_t d = 0; d < loci.drives.size(); ++d) {
			if (d != ANGER && d != 10 && d < 12 && loci.drives[d] >= loci.drives[ANGER]) {
				strongest = false;
			}
		}
		if (strongest) {
			result = 3;
		}
	}
	expression = static_cast<uint8_t>(result);
}

void Creature::blink() {
	if (dead || asleep) {
		eyes_open = 0;
	} else {
		// closed 1 tick in 21
		eyes_open = rand_int32(0, 20) == 0 ? 0 : 1;
	}
}

// --- engine inputs to the brain (Q1) ---

void Creature::update_four_tick_inputs() {
	if (!brain) {
		return;
	}
	for (uint32_t i = 0; i < loci.drives.size(); ++i) {
		brain_set_input(*brain, LOBE_DRIVE, i, loci.drives[i]);
	}
	if (auto* it = g_engine_context.objects->try_get(attention)) {
		brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_ACTIVE, it->actv ? 255 : 0);
		float distance = std::abs(get_bbox().x - target_x);
		if (has_target && distance < 128) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_NEAR, clamp_byte(255 - 2 * numeric_cast<int32_t>(distance)));
		}
	}
	if (pushed_by_limit && !g_engine_context.objects->try_get(carrier)) {
		receive_numbered_stimulus(STIMULUS_PUSHED_BY_LIMIT, this->uid);
	}
	pushed_by_limit = false;
}

void Creature::update_eight_tick_inputs() {
	if (!brain) {
		return;
	}
	if (auto* it = g_engine_context.objects->try_get(attention); it && it->as_creature()) {
		auto* other = it->as_creature();
		brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_IS_CREATURE, 255);
		if (!other->moniker.empty() && (other->moniker == mother || other->moniker == father)) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_IS_PARENT, 255);
		}
		if (!moniker.empty() && (other->mother == moniker || other->father == moniker)) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_IS_CHILD, 255);
		}
		if ((!mother.empty() && other->mother == mother) || (!father.empty() && other->father == father)) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_SIBLING, 255);
		}
		if (other->family == family && other->genus == genus && other->sex != sex) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_IT_OPPOSITE_SEX, 255);
		}
	}
	// a room limit close ahead of the head
	if (head) {
		float ahead = -1;
		if (direction == 2) {
			ahead = numeric_cast<float>(limit.right()) - (head->x() + numeric_cast<float>(head->width()));
		} else if (direction == 3) {
			ahead = head->x() - numeric_cast<float>(limit.x);
		}
		// (also once the head is past the limit, stronger the further, as LibreCreatures has it)
		if ((direction == 2 || direction == 3) && ahead < 80) {
			brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_LIMIT_AHEAD, clamp_byte(200 - 2 * numeric_cast<int32_t>(ahead)));
		}
	}
	if (g_engine_context.objects->try_get(carrier)) {
		brain_set_input(*brain, LOBE_GENERAL_SENSE, SENSE_CARRIED, 127);
	}
}

void Creature::update_sixteen_tick_loci() {
	loci.senses[0] = 255; // constant
	loci.senses[1] = asleep ? 255 : 0;
	update_expression();
	muscle_counter = 0;
}

void Creature::update_environment_loci() {
	// Q20. Temperature: by the world's period and a horizontal band of the world, except in
	// rooms of a type other than 1. The original's band lookup puts every x in band 0, so
	// only that column has any effect.
	// TODO: worth checking with TEMP in the running game
	static const int32_t TEMPERATURE[5][5] = {
		{0, 1, -1, 1, 0},
		{0, 2, -1, 1, 0},
		{0, 3, 0, 1, 0},
		{0, 0, 0, 1, 0},
		{-1, -1, -1, 1, -1},
	};
	static const uint8_t LIGHT[5] = {150, 200, 255, 64, 0};
	const uint32_t period = std::min<uint32_t>(g_engine_context.world->time_of_day, 4);
	auto bbox = get_bbox();
	int32_t t = 0;
	const Room* room = g_engine_context.map->find_room_containing(bbox.x, bbox.y);
	if (!room || room->type == 1) {
		t = TEMPERATURE[period][0];
	}
	int32_t level = t * 255 / 3;
	loci.senses[2] = level < 0 ? clamp_byte(-level) : 0; // air coldness
	loci.senses[3] = level > 0 ? clamp_byte(level) : 0; // air hotness
	loci.senses[4] = LIGHT[period];

	// crowdedness: the creature it remembers for its own category
	loci.senses[5] = 0;
	int32_t own = attention_category(*this);
	if (own >= 0) {
		if (auto* other = g_engine_context.objects->try_get(attention_memory[static_cast<size_t>(own)].object)) {
			float distance = std::abs(other->get_bbox().x - bbox.x);
			loci.senses[5] = distance <= 200 ? clamp_byte(250 - numeric_cast<int32_t>(distance)) : 50;
		}
	}
}

void Creature::need_driven_recall() {
	// Q1: every 128 ticks, the creature's needs bring to mind the category that has best
	// satisfied them before. As LibreCreatures has it (checked against the original): not
	// while asleep, dreaming or riding something.
	auto* by = g_engine_context.objects->try_get(carrier);
	if (!brain || dead || asleep || dreaming || (by && !by->as_pointer_tool())) {
		return;
	}
	static const int32_t WEIGHTS[16] = {255, 120, 255, 255, 255, 220, 200, 180, 200, 255, 100, 180, 255, 255, 255, 255};
	std::array<int32_t, 16> weighted;
	int32_t boost = 10;
	for (size_t d = 0; d < 16; ++d) {
		weighted[d] = loci.drives[d] * WEIGHTS[d] / 256;
		if (weighted[d] > boost) {
			boost = weighted[d] / 4;
		}
	}
	int32_t best_category = -1;
	int32_t best_score = 0;
	for (int32_t c = 1; c < ATTENTION_CATEGORIES; ++c) { // (never category 0, nothing)
		if (!attention_memory[static_cast<size_t>(c)].has_position) {
			continue;
		}
		int32_t score = 0;
		for (size_t d = 0; d < 16; ++d) {
			score += learned_chemicals[static_cast<size_t>(c)][d] * weighted[d] / 256;
		}
		if (score > best_score) {
			best_score = score;
			best_category = c;
		}
	}
	if (best_category < 0) {
		return;
	}

	auto& e = attention_memory[static_cast<size_t>(best_category)];
	auto* remembered = g_engine_context.objects->try_get(e.object);
	if (remembered) {
		// already attending to it, or think of it more
		if (e.object != attention) {
			brain_add_input(*brain, LOBE_STIMULUS_SOURCE, static_cast<uint32_t>(best_category), boost);
		}
		return;
	}
	// facing the remembered place, left or right (not towards or away from us), and near it
	const bool facing_it = (direction == 2 && e.last_x >= ground_x()) || (direction == 3 && e.last_x <= ground_x());
	if (!remembered && g_engine_context.map->find_room_containing(ground_x(), ground_y()) == g_engine_context.map->find_room_containing(e.last_x, e.last_y) && facing_it && std::abs(e.last_x - ground_x()) < 500) {
		e.has_position = false;
		return;
	}

	// suggest one of six travel actions to itself: (noun category, verb)
	struct Travel {
		int32_t noun;
		int32_t verb;
	};
	static const Travel TRAVEL[6] = {{27, 1}, {27, 2}, {2, 1}, {2, 4}, {-1, 10}, {-1, 11}};
	std::array<int32_t, 6> score{};
	auto add = [&](std::array<int32_t, 6> points) {
		for (size_t i = 0; i < 6; ++i) {
			score[i] += points[i];
		}
	};
	auto* lift = g_engine_context.objects->try_get(attention_memory[27].object);
	auto* cat2 = g_engine_context.objects->try_get(attention_memory[2].object);
	add(lift ? std::array<int32_t, 6>{0, 0, -9, 0, 0, 0} : std::array<int32_t, 6>{-9, -9, 0, 0, 0, 0});
	if (!cat2) {
		add({0, 0, -9, -9, 1, 1});
	} else if (cat2->actv) {
		add({0, 0, -9, 8, -1, -1});
	}
	if (e.last_x > ground_x()) {
		add({0, 0, 0, 0, -5, 5});
	} else {
		add({0, 0, 0, 0, 5, -5}); // to the left, or right here
	}
	if (e.last_y < numeric_cast<float>(limit.y)) {
		add({0, 9, 5, 0, 1, 1});
	} else if (e.last_y > numeric_cast<float>(limit.bottom())) {
		add({9, 0, 5, 0, 1, 1});
	}
	// indoors (or no room) against outdoors and undersea, as the original groups rooms
	const Room* room = g_engine_context.map->find_room_containing(ground_x(), ground_y());
	if (!room || room->type < 1) {
		add({0, 2, 1, 0, 0, 0});
	} else {
		add({0, 0, 0, 0, 2, 2});
	}
	size_t chosen = 0;
	for (size_t i = 1; i < 6; ++i) {
		if (score[i] > score[chosen]) {
			chosen = i;
		}
	}
	if (score[chosen] < 1) {
		return;
	}
	const auto& travel = TRAVEL[chosen];
	if (travel.noun >= 0 && g_engine_context.objects->try_get(attention_memory[static_cast<size_t>(travel.noun)].object)) {
		brain_add_input(*brain, LOBE_STIMULUS_SOURCE, static_cast<uint32_t>(travel.noun), boost);
	}
	auto* it = g_engine_context.objects->try_get(attention);
	if (travel.noun < 0 || (it && attention_category(*it) == travel.noun)) {
		brain_add_input(*brain, LOBE_VERB, static_cast<uint32_t>(travel.verb), boost);
	} else {
		// waits for attention to switch to the noun, for up to 5 attention updates
		pending_verb = travel.verb;
		pending_verb_category = travel.noun;
		pending_verb_updates_left = 5;
		pending_verb_boost = boost;
	}
}

// --- decisions (Q6-Q9, Q24) ---

void Creature::start_decision(int32_t decision) {
	// Every replacement by a voluntary action, including attention's quiescent action,
	// releases the old reflex and wakes the creature (batch 1 Q3, Q6 and Q24; batch 2 Q14).
	// Otherwise an interrupted sleep can lose ASLP 0/DONE and keep the brain blocked.
	asleep = 0;
	eyes_open = 1;
	running_involuntary = -1;
	current_decision = decision;
	auto* it = g_engine_context.objects->try_get(attention);
	auto event_for = [&](int32_t d) {
		return static_cast<ScriptNumber>((it ? SCRIPT_EXTRA_QUIESCENT : SCRIPT_INTRO_QUIESCENT) + d);
	};
	// look under _IT_'s classifier first; when _IT_ is a creature, under 0 0 0 (the kiss, cuddle,
	// slap and Grendel punch scripts; batch 1 Q9, batch 7 Q4); then the creature's own
	auto run = [&](int32_t d) {
		auto event = event_for(d);
		if (it && !it->as_creature() && script_exists(it->family, it->genus, it->species, event)) {
			return g_engine_context.macros->queue_script(this, this, it->family, it->genus, it->species, event);
		}
		if (it && it->as_creature() && script_exists(0, 0, 0, event)) {
			return g_engine_context.macros->queue_script(this, this, 0, 0, 0, event);
		}
		if (script_exists(family, genus, species, event)) {
			return g_engine_context.macros->queue_script(this, this, event);
		}
		return false;
	};
	if (run(decision)) {
		return;
	}
	// no script: fall back to quiescent, and stop there if that's missing too
	if (brain) {
		brain_clear_neuron(*brain, LOBE_DECISION, static_cast<uint32_t>(decision));
	}
	current_decision = DECISION_QUIESCENT;
	if (decision != DECISION_QUIESCENT) {
		run(DECISION_QUIESCENT);
	}
}

void Creature::done() {
	// Q8: DONE doesn't end the script
	if (running_involuntary >= 0) {
		running_involuntary = -1;
		return;
	}
	if (brain) {
		brain_clear_neuron(*brain, LOBE_DECISION, static_cast<uint32_t>(current_decision));
	}
	importance = 0;
}

bool Creature::try_involuntary_action() {
	int32_t winner = -1;
	uint8_t best = 0;
	for (size_t n = 0; n < involuntary_latency.size(); ++n) {
		if (involuntary_latency[n] > 0) {
			involuntary_latency[n]--;
			continue;
		}
		const uint8_t v = loci.involuntary[n];
		if (v && rand_int32(0, 255) < v && v > best) {
			best = v;
			winner = static_cast<int32_t>(n);
		}
	}
	if (winner < 0 || asleep) {
		return false;
	}
	// F011, adapted from LibreCreatures' missing-reflex correction. A reflex
	// must exist and be queued before it can replace the current voluntary action.
	// See third_party/librecreatures/NOTICE.md for source and license attribution.
	auto event = static_cast<ScriptNumber>(SCRIPT_INVOLUNTARY0 + winner);
	if (!script_exists(family, genus, species, event)
		|| !g_engine_context.macros->queue_script(this, this, event)) return false;
	running_involuntary = winner;
	importance = 0;
	current_decision = DECISION_QUIESCENT;
	return true;
}

void Creature::choose_voluntary_action() {
	if (!brain) {
		return;
	}
	const bool has_it = g_engine_context.objects->try_get(attention) != nullptr;
	auto allowed = [&](int32_t d) {
		if (d >= 12) {
			return false;
		}
		if (d >= 1 && d <= 6) {
			return has_it;
		}
		if (d == DECISION_REST) {
			return !has_it;
		}
		return true;
	};
	int32_t winner = DECISION_QUIESCENT;
	uint8_t best = 0;
	for (int32_t d = 0; d < 16; ++d) {
		auto* n = brain_neuron(*brain, LOBE_DECISION, static_cast<uint32_t>(d));
		if (n && allowed(d) && n->output > best) {
			best = n->output;
			winner = d;
		}
	}
	if (winner == current_decision) {
		return;
	}
	// not in the original: a meal that has started isn't replaced by another decision either
	// (see choose_attention)
	if (g_engine_context.objects->try_get(attention) && g_engine_context.macros->is_eating(this->uid, attention)) {
		return;
	}
	brain_clear_neuron(*brain, LOBE_DECISION, static_cast<uint32_t>(current_decision));
	auto* it = g_engine_context.objects->try_get(attention);
	fmt::print("INFO [Brain] {} ({}) decides {} about {}\n", history_name, moniker, winner, it ? format_as(it) : "nothing");
	start_decision(winner);
	// one time in six it speaks: its need or what it's doing, evenly (batch 1, Q6)
	if (rand_int32(0, 5) == 0) {
		if (rand_int32(0, 1) == 0) {
			say_need();
		} else {
			say_what_doing();
		}
	}
	brain_add_input(*brain, LOBE_DECISION, static_cast<uint32_t>(winner), 10);
}

void Creature::brain_step() {
	// a dead creature's chemistry keeps going, but nothing else (batch 2, Q16)
	if (dead) {
		if (biochemistry) {
			creature_biochemistry_tick(*biochemistry, loci, brain.get(), dead, biotick);
		}
		biotick++;
		return;
	}
	if (!brain) {
		return;
	}
	// IMPT: twice the importance is added to the current decision before the brain updates
	brain_add_input(*brain, LOBE_DECISION, static_cast<uint32_t>(current_decision), 2 * importance);
	brain_tick(*brain, biotick, brain_migration);
	if (biochemistry) {
		creature_biochemistry_tick(*biochemistry, loci, brain.get(), dead, biotick);
	}
	biotick++;
	if (dreaming) {
		return;
	}
	if (running_involuntary >= 0) {
		const int32_t event = SCRIPT_INVOLUNTARY0 + running_involuntary;
		const bool still_running = std::any_of(g_engine_context.macros->begin(), g_engine_context.macros->end(), [&](const Macro& m) {
			return m.ownr == uid && m.event == event && !m.destroy_as_soon_as_possible;
		});
		if (!still_running) {
			// Some original reflexes STOP without DONE, notably laying an egg while carried
			// (batch 7 Q4). Further reflexes remain blocked until a voluntary choice replaces
			// them, but that choice must still be possible once their script has stopped.
			choose_voluntary_action();
		}
		return;
	}
	if (!try_involuntary_action()) {
		choose_voluntary_action();
	}
}

void Creature::creature_tick() {
	const uint32_t tick = g_engine_context.world->tick;
	// a body doesn't age (as LibreCreatures has it), so the Funeral Kit has its age at death
	if (!dead) {
		age_ticks++;
	}
	check_standing_on_a_floor();
	blink();
	creature_tick_pose(*this);
	update_target_point();
	update_zzz();
	update_voice();
	loci.muscle_energy = clamp_byte(muscle_counter);
	if (tick % 128 == 0) {
		disease_update();
	}
	if (dead) {
		// chemistry keeps going on the brain-step schedule
		if (g_engine_context.world->tick % 5 == 0) {
			brain_step();
		}
		return;
	}

	if (tick % 4 == 0) {
		auto stimuli_now = std::move(queued_stimuli);
		queued_stimuli.clear();
		for (const auto& q : stimuli_now) {
			receive_stimulus(q.stimulus, q.from);
		}
		update_four_tick_inputs();
	}
	if (tick % 8 == 0) {
		update_eight_tick_inputs();
		if (!asleep) {
			sight_scan();
		}
		choose_attention();
		forget_out_of_reach();
	}
	if (tick % 16 == 0) {
		update_sixteen_tick_loci();
	}
	if (tick % 128 == 0) {
		// in LibreCreatures' order (checked against the original): after the bacterium, the
		// environment, fertility, the life stage and the death signal, then the recall
		update_environment_loci();
		update_fertility();
		age_update();
		if (!dead && loci.death_signal) {
			fmt::print("INFO [Death] {} ({}) dies: its death signal is {}\n", history_name, moniker, loci.death_signal);
			die();
		}
		need_driven_recall();
	}

	// Approved F001: one brain/chemistry/action opportunity per five ticks for
	// every creature, independent of population ordering and deletions.
	if (dreaming) {
		dream_tick();
		return;
	}
	if (tick % 5 == 0) {
		brain_step();
	}
}

void Creature::handle_left_click(float relx, float rely) {
	// Q26: the head tickles (activate 1), the body slaps (deactivate), the limbs do nothing
	auto bbox = get_bbox();
	const float x = bbox.x + relx;
	const float y = bbox.y + rely;
	auto inside = [&](const DullPart* part) {
		return part && x >= part->x() && x < part->x() + numeric_cast<float>(part->width()) && y >= part->y() && y < part->y() + numeric_cast<float>(part->height());
	};
	auto pointer = g_engine_context.pointer->m_pointer_tool;
	if (inside(head.get())) {
		// the hand tickles: the creatures' "4 0 0 50" script animates it (batch 4, Q6)
		g_engine_context.messages->hand_message(pointer, this->uid, MESSAGE_ACTIVATE1);
		g_engine_context.pointer->run_hand_script(this, SCRIPT_POINTER_ACTIVATE1);
	} else if (inside(body.get())) {
		// the hand slaps: "4 0 0 52"
		g_engine_context.messages->hand_message(pointer, this->uid, MESSAGE_DEACTIVATE);
		g_engine_context.pointer->run_hand_script(this, SCRIPT_POINTER_DEACTIVATE);
	}
}
