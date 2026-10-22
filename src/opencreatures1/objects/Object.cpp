#include "Object.h"

#include "Blackboard.h"
#include "Bubble.h"
#include "C1SoundManager.h"
#include "CallButton.h"
#include "CompoundObject.h"
#include "Creature.h"
#include "Lift.h"
#include "MapManager.h"
#include "Macro.h"
#include "MacroManager.h"
#include "MessageManager.h"
#include "ObjectManager.h"
#include "PointerTool.h"
#include "SFCSerialization.h"
#include "Scenery.h"
#include "SimpleObject.h"
#include "Vehicle.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "common/Ascii.h"
#include "common/NumericCast.h"
#include "common/Ranges.h"
#include "common/render/RenderSystem.h"
#include "fileformats/sfc/Creature.h"
#include "fileformats/sfc/Object.h"

#include <cmath>
#include <cstdlib>

static bool world_has_at_least_one_creature() {
	for (auto* obj : *g_engine_context.objects) {
		if (obj->as_creature()) {
			return true;
		}
	}
	return false;
}

Scenery* Object::as_scenery() {
	return dynamic_cast<Scenery*>(this);
}

SimpleObject* Object::as_simple_object() {
	return dynamic_cast<SimpleObject*>(this);
}

Bubble* Object::as_bubble() {
	return dynamic_cast<Bubble*>(this);
}

CallButton* Object::as_call_button() {
	return dynamic_cast<CallButton*>(this);
}

PointerTool* Object::as_pointer_tool() {
	return dynamic_cast<PointerTool*>(this);
}

CompoundObject* Object::as_compound_object() {
	return dynamic_cast<CompoundObject*>(this);
}

Vehicle* Object::as_vehicle() {
	return dynamic_cast<Vehicle*>(this);
}

Lift* Object::as_lift() {
	return dynamic_cast<Lift*>(this);
}

Blackboard* Object::as_blackboard() {
	return dynamic_cast<Blackboard*>(this);
}

Creature* Object::as_creature() {
	return dynamic_cast<Creature*>(this);
}

const Scenery* Object::as_scenery() const {
	return dynamic_cast<const Scenery*>(this);
}

const SimpleObject* Object::as_simple_object() const {
	return dynamic_cast<const SimpleObject*>(this);
}

const Bubble* Object::as_bubble() const {
	return dynamic_cast<const Bubble*>(this);
}

const CallButton* Object::as_call_button() const {
	return dynamic_cast<const CallButton*>(this);
}

const PointerTool* Object::as_pointer_tool() const {
	return dynamic_cast<const PointerTool*>(this);
}

const CompoundObject* Object::as_compound_object() const {
	return dynamic_cast<const CompoundObject*>(this);
}

const Vehicle* Object::as_vehicle() const {
	return dynamic_cast<const Vehicle*>(this);
}

const Lift* Object::as_lift() const {
	return dynamic_cast<const Lift*>(this);
}

const Blackboard* Object::as_blackboard() const {
	return dynamic_cast<const Blackboard*>(this);
}

const Creature* Object::as_creature() const {
	return dynamic_cast<const Creature*>(this);
}

void Object::handle_mesg_activate1(Message msg) {
	if (as_creature()) {
		// A creature's tickle, slap and kiss scripts run alongside whatever it is doing, rather
		// than replacing it, and a dead creature ignores them (batch 7, Q6 and Q7). Replacing
		// cut meals short, and a dead body's death script, which removes it, could be lost.
		if (as_creature()->dead) {
			return;
		}
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE1, false);
		return;
	}
	auto* from = g_engine_context.objects->try_get(msg.from);
	// The pointer is a simple object too. In Eden its touch permissions are zero, so
	// creature activation is refused with disappointment (batch 1, Q23), not an exception.
	if (as_simple_object()) {
		if (from && from->as_creature() && family == 2 && (genus == 6 || genus == 4) && std::getenv("OPENCREATURES1_TRACE_MEALS")) {
			fmt::print("TRACE [Meal] {} activates {} {} {}: actv {}, touch {}\n", from->as_creature()->moniker, family, genus, species, static_cast<int>(actv), static_cast<int>(as_simple_object()->touch_bhvr));
		}
		if (from && from->as_creature()) {
			if (as_call_button() && (actv == ACTV_ACTIVE1 || actv == ACTV_ACTIVE2)) {
				return from->creature_stim_disappoint(this);
			}
			if (actv == ACTV_ACTIVE1 || !(as_simple_object()->touch_bhvr & TOUCH_ACTIVATE1)) {
				return from->creature_stim_disappoint(this);
			}
		}
		if (as_call_button()) {
			auto* lift = g_engine_context.objects->try_get(as_call_button()->lift);
			if (!lift || !lift->as_lift() || as_call_button()->floor >= lift->as_lift()->floors.size()) {
				// missing lift or unsupported floor
				printf("WARNING: Tried to activate1 a callbutton with non-existent lift!\n");
				return;
			}
			if (lift->actv == ACTV_INACTIVE && lift->as_lift()->next_or_current_floor == as_call_button()->floor) {
				// already here, don't stim disappointment though
				return;
			}

			// tell lift to come here eventually
			// told the lift to do the thing
            auto& pending = lift->as_lift()->activated_call_buttons;
            if (pending.size() == 8 && pending.find(this->uid) == pending.end()) {
                fmt::print("WARNING: lift call capacity exhausted\n");
                return;
            }
			pending.insert(this->uid);
		}
		actv = ACTV_ACTIVE1;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE1);
		// The island carrot vendor feeds the creature that pushes it. When the hand pushes it,
		// a ready-to-eat carrot drops out beside it instead (not in the original)
		if (from && from->as_pointer_tool() && family == 2 && genus == 8 && species == 3) {
			auto b = get_bbox();
			Macro m(fmt::format("inst,new: simp pars 7 0 1000 0,setv clas 33948416,setv attr 67,bhvr 0 1,pose 5,mvto {} {},endm",
				static_cast<int32_t>(b.right()) + 4, static_cast<int32_t>(b.bottom()) - 49));
			// owned by the vendor: the hand's own click script, queued next, cancels the hand's
			// other scripts, and this one waits a tick for the new carrot
			m.ownr = m.targ = this->uid;
			m.from = msg.from;
			g_engine_context.macros->add(m);
			fmt::print("INFO [Vendor] the hand pushed the carrot vendor: a carrot drops out\n");
		}
		return;
	}
	if (as_compound_object()) {
		// TODO: also disappoint and exit early if script doesn't exist
		if (actv == ACTV_ACTIVE1 || (from && from->as_creature() && as_compound_object()->functions_to_hotspots[HOTSPOT_CREATUREACTIVATE1] == -1)) {
			if (from)
				from->creature_stim_disappoint(this);
			return;
		}
		if (as_lift()) {
			if (actv != ACTV_INACTIVE || obv0 != 0 || as_lift()->next_or_current_floor + 1 >= as_lift()->floors.ssize()) {
				// different allowed check, must be ACTV = INACTIVE, OBV0 == 0, and lower floor
				if (from)
					from->creature_stim_disappoint(this);
				return;
			}
			as_lift()->next_or_current_floor++;
		}

		if (as_vehicle()) {
			// truncate fixed point position
			set_position(std::trunc(get_bbox().x), std::trunc(get_bbox().y));
		}

		actv = ACTV_ACTIVE1;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE1);
		return;
	}
	throw Exception(fmt::format("handle_mesg_activate1 not implemented on object {} {} {}", family, genus, species));
}

void Object::handle_mesg_activate2(Message msg) {
	if (as_creature()) {
		// A creature's tickle, slap and kiss scripts run alongside whatever it is doing, rather
		// than replacing it, and a dead creature ignores them (batch 7, Q6 and Q7). Replacing
		// cut meals short, and a dead body's death script, which removes it, could be lost.
		if (as_creature()->dead) {
			return;
		}
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE2, false);
		return;
	}
	auto* from = g_engine_context.objects->try_get(msg.from);
	if (as_call_button()) {
		// Call buttons never activate2, always activate1
		return handle_mesg_activate1(msg);
	}
	if (as_simple_object()) {
		if (from && from->as_creature()) {
			if (actv == ACTV_ACTIVE2 || !(as_simple_object()->touch_bhvr & TOUCH_ACTIVATE2)) {
				return from->creature_stim_disappoint(this);
			}
		}
		actv = ACTV_ACTIVE2;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE2);
		return;
	}
	if (as_compound_object()) {
		// TODO: also disappoint and exit early if script doesn't exist
		if (actv == ACTV_ACTIVE2 || (from && from->as_creature() && as_compound_object()->functions_to_hotspots[HOTSPOT_CREATUREACTIVATE2] == -1)) {
			if (from)
				from->creature_stim_disappoint(this);
			return;
		}
		if (as_lift()) {
			if (actv != ACTV_INACTIVE || obv0 != 0 || as_lift()->next_or_current_floor == 0) {
				// different allowed check, must be ACTV = INACTIVE, OBV0 == 0, and higher floor
				if (from)
					from->creature_stim_disappoint(this);
				return;
			}
			// TODO: maybe set it to ACTV_ACTIVE1 instead?
			as_lift()->next_or_current_floor--;
		}

		if (as_vehicle()) {
			// truncate fixed point position
			set_position(std::trunc(get_bbox().x), std::trunc(get_bbox().y));
		}

		actv = ACTV_ACTIVE2;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_ACTIVATE2);
		return;
	}
	throw Exception(fmt::format("handle_mesg_activate2 not implemented on object {} {} {}", family, genus, species));
}

void Object::handle_mesg_deactivate(Message msg) {
	if (as_creature()) {
		// A creature's tickle, slap and kiss scripts run alongside whatever it is doing, rather
		// than replacing it, and a dead creature ignores them (batch 7, Q6 and Q7). Replacing
		// cut meals short, and a dead body's death script, which removes it, could be lost.
		if (as_creature()->dead) {
			return;
		}
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_DEACTIVATE, false);
		return;
	}
	auto* from = g_engine_context.objects->try_get(msg.from);
	if (as_simple_object()) {
		if (from && from->as_creature()) {
			if (actv == ACTV_INACTIVE || !(as_simple_object()->touch_bhvr & TOUCH_DEACTIVATE)) {
				return from->creature_stim_disappoint(this);
			}
		}

		actv = ACTV_INACTIVE;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_DEACTIVATE);
		return;
	}
	if (as_compound_object()) {
		// TODO: also disappoint and exit early if script doesn't exist
		if (actv == ACTV_INACTIVE || (from && from->as_creature() && as_compound_object()->functions_to_hotspots[HOTSPOT_CREATUREDEACTIVATE] == -1)) {
			if (from)
				from->creature_stim_disappoint(this);
			return;
		}
		if (as_lift()) {
			// not allowed!!
			if (from)
				from->creature_stim_disappoint(this);
			return;
		}
		if (as_vehicle()) {
			// stop!
			as_vehicle()->xvel = 0;
			as_vehicle()->yvel = 0;
			// truncate fixed point position
			set_position(std::trunc(get_bbox().x), std::trunc(get_bbox().y));
		}

		actv = ACTV_INACTIVE;
		g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_DEACTIVATE);
		return;
	}
	throw Exception(fmt::format("handle_mesg_deactivate not implemented on object {} {} {}", family, genus, species));
}

void Object::handle_mesg_hit(Message msg) {
	// simple objects refuse to be hit by creatures (spec Q23)
	auto* from = g_engine_context.objects->try_get(msg.from);
	if (from && from->as_creature() && as_simple_object()) {
		from->creature_stim_disappoint(this);
		return;
	}
	g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_HIT);
}

void Object::handle_mesg_pickup(Message msg) {
	// The hand can pick up MOUSEABLE objects and creatures can pick up CARRYABLE ones,
	// e.g. the honey jars (attr 71) can be picked up by both, the incubator by neither.
	auto* from = g_engine_context.objects->try_get(msg.from);
	if (!from) {
		return;
	}
	const bool by_pointer = from->as_pointer_tool() != nullptr;
	// The hand can take what a creature is carrying, and the creature isn't told; a creature
	// can take what the hand is holding, and the hand lets go of it (batch 5, Q5). A vehicle
	// passenger is available for pickup too; its carrier identifies its cabin, not a holder.
	auto* held_by = g_engine_context.objects->try_get(carrier);
	const bool in_vehicle = held_by && held_by->as_vehicle() && movement_status == MOVEMENT_INVEHICLE;
	if (held_by && !in_vehicle && !(by_pointer && held_by->as_creature()) && !(!by_pointer && held_by->as_pointer_tool())) {
		return;
	}
	// creatures can only pick up carryable simple objects; compound objects always refuse (spec Q23)
	const bool refused = by_pointer ? !(attr & ATTR_MOUSEABLE) : (!as_simple_object() || !(attr & ATTR_CARRYABLE));
	if (refused) {
		if (from->as_creature()) {
			from->creature_stim_disappoint(this);
		}
		return;
	}
	if (from->as_creature()) {
		if (held_by && held_by->as_pointer_tool()) {
			if (g_engine_context.pointer->m_carried_object == this->uid) {
				g_engine_context.pointer->m_carried_object = {};
			}
			held_by->attr = static_cast<uint8_t>(held_by->attr & ~ATTR_INVISIBLE);
			if (auto* simple = as_simple_object()) {
				simple->part.set_z_order(simple->z_order);
			}
		}
		// a creature picking up something new drops what it was holding, as a normal release
		// (spec Q12; batch 5, Q5)
		for (auto* obj : *g_engine_context.objects) {
			if (obj != this && obj->carrier == from->uid) {
				obj->handle_mesg_drop(Message{from->uid, obj->uid, MESSAGE_DROP});
			}
		}
	}

	carrier = msg.from;
	movement_status = by_pointer ? MOVEMENT_MOUSEDRIVEN : MOVEMENT_CARRIED;
	if (by_pointer) {
		auto bbox = get_bbox();
		g_engine_context.pointer->pick_up(this->uid, bbox.x, bbox.y);
		// while holding something the hand is invisible to creatures (batch 4, Q7)
		from->attr = static_cast<uint8_t>(from->attr | ATTR_INVISIBLE);
		// on the top plane with no limits (batch 2, Q12; batch 3, Q21)
		if (auto* creature = as_creature()) {
			// hanging from the hand; a Norn picked up becomes the selected creature (batch 3, Q3)
			creature->z_order = HAND_PLANE - 1; // just behind the hand, which holds it
			creature->update_limits();
			if (creature->genus == 1) {
				g_engine_context.world->current_norn = this->uid;
			}
		} else {
			if (auto* simple = as_simple_object()) {
				simple->part.set_z_order(HAND_PLANE - 1); // just behind the hand, which holds it
			}
			limit = Rect2i{-100000, -100000, 200000, 200000};
		}
	}
	// carried by a creature, its limits aren't worked out again (batch 5, Q5)
	g_engine_context.macros->queue_script(msg.from, this->uid, SCRIPT_PICKUP);
}

void Object::handle_mesg_drop(Message msg) {
	if (carrier != msg.from) {
		return;
	}
	carrier = {};
	movement_status = MOVEMENT_AUTONOMOUS;
	if (g_engine_context.pointer->m_carried_object == this->uid) {
		g_engine_context.pointer->m_carried_object = {};
	}
	auto* from = g_engine_context.objects->try_get(msg.from);
	if (from && from->as_pointer_tool()) {
		// the hand is visible to creatures again once it lets go (batch 4, Q7)
		from->attr = static_cast<uint8_t>(from->attr & ~ATTR_INVISIBLE);
	}
	if (!as_creature()) {
		// back on its own plane (batch 3, Q21)
		if (auto* simple = as_simple_object()) {
			simple->part.set_z_order(simple->z_order);
		}
		land_object(from);
		// an attr 32 object let go runs no drop script (batch 5, Q4)
		if (attr & ATTR_FLOATABLE) {
			return;
		}
		// The drop script starts at once, and only after its first part has run is the
		// object seated on its floor, with the picture it shows then (batch 6, Q1b): the
		// carrot's "pose 6" lays it down before it's seated.
		g_engine_context.macros->start_script_now(from, this, SCRIPT_DROP);
		if (!g_engine_context.objects->try_get(carrier) || movement_status == MOVEMENT_INVEHICLE) {
			auto here = get_bbox();
			set_position(here.x, numeric_cast<float>(limit.bottom()) - here.height);
		}
		return;
	}
	if (auto* creature = as_creature()) {
		// creatures land straight on the floor below, or a container under them takes them
		// (batch 2, Q11)
		creature->z_order = creature->saved_z_order;
		auto here = get_bbox();
		for (auto* obj : *g_engine_context.objects) {
			auto there = obj->get_bbox();
			if (obj != this && (obj->attr & ATTR_CONTAINER) && obj->as_vehicle() && there.x < here.right() && here.x < there.right() && there.y < here.bottom() && here.y < there.bottom()) {
				obj->vehicle_grab_passengers();
				break;
			}
		}
		if (movement_status != MOVEMENT_INVEHICLE) {
			creature->land();
		}
	}
	g_engine_context.macros->start_script_now(from, this, SCRIPT_DROP);
}

void Object::set_position(float newx, float newy) {
    // A compound object between NEW: COMP and NEW: PART has no position yet.
    if (auto* compound = as_compound_object(); compound && compound->parts.empty()) return;
    unplaced = false;
	// TODO: if the object's current x-position is 100.1 and the new x-position
	// is 100 (low-precision), should we actually move it? do we lose the
	// high-precision part of the current position? This is only relevant to Vehicles.

	// TODO: is this correct?
	if (get_bbox().x == newx && get_bbox().y == newy) {
		return;
	}

	const auto old_bbox = get_bbox();

	// update object position
	if (as_scenery()) {
		as_scenery()->part.set_position(newx, newy);
	} else if (as_simple_object()) {
		as_simple_object()->part.set_position(newx, newy);
	} else if (as_compound_object()) {
		for (auto& p : as_compound_object()->parts) {
			p.set_position(newx + p.relx, newy + p.rely);
		}
	} else if (auto* creature = as_creature()) {
		auto bbox = get_bbox();
		creature->move_by(newx - bbox.x, newy - bbox.y);
	} else {
		throw Exception("set_position not implemented for non-scenery, non-simpleobject, non-compoundobject");
	}

	// passengers move with their vehicle
	if (as_vehicle()) {
		float xdiff = newx - old_bbox.x, ydiff = newy - old_bbox.y;
		for (auto* obj : *g_engine_context.objects) {
			if (obj->carrier == this->uid && obj->movement_status == MOVEMENT_INVEHICLE) {
                const auto& room = as_vehicle()->cabin;
                const Rect2f previous_cabin{old_bbox.x + room.x, old_bbox.y + room.y, float(room.width), float(room.height)};
                if (!obj->get_bbox().intersects_modx(previous_cabin, float(CREATURES1_WORLD_WIDTH))) {
                    obj->carrier = {};
                    obj->movement_status = MOVEMENT_AUTONOMOUS;
                    if (auto* c = obj->as_creature()) c->update_limits();
                    else obj->update_free_limits();
                    continue;
                }
				// shifted by the same amount, with the cabin as their limits again (batch 5, Q1)
				if (auto* creature = obj->as_creature()) {
					// whole pixels, by how far the vehicle's drawn position moved, so a
					// creature doesn't drift off the cabin floor
					creature->move_by(std::floor(newx) - std::floor(old_bbox.x), std::floor(newy) - std::floor(old_bbox.y));
					creature->update_limits();
					continue;
				}
				obj->add_position(xdiff, ydiff);
				const auto& cabin = as_vehicle()->cabin;
				obj->limit = Rect2i{static_cast<int32_t>(std::floor(newx)) + cabin.x, static_cast<int32_t>(std::floor(newy)) + cabin.y, cabin.width, cabin.height};
			}
		}
	}

	// update any controlled sound
	if (current_sound) {
		auto bbox = get_bbox();
		current_sound.set_position(bbox.x, bbox.y, bbox.width, bbox.height);
	}
}

void Object::add_position(float xdiff, float ydiff) {
    if (auto* compound = as_compound_object(); compound && compound->parts.empty()) return;
    unplaced = false;
	// TODO: if the object's current x-position is 100.1 and the x-diff is 5, do we move
	// it to 105.1 or 105? e.g. should we lose the high-precision part of the current position?
	// This is only relevant to Vehicles.

	// TODO: is using get_bbox() correct?
	set_position(get_bbox().x + xdiff, get_bbox().y + ydiff);
}

void Object::land_object(Object* released_by) {
	// Batch 4, Q1: when the hand or a creature lets go of an object, the engine places it at
	// once, before the drop script runs.
	// - attr 32: it stays exactly where it was let go, with unlimited limits (movs 2);
	// - over a container (attr 8), when the hand lets go: the container takes it, with the
	//   container's plane and the cabin as its limits, its x kept inside the cabin;
	// - otherwise its limits are worked out again and it moves straight down (or up) so
	//   that its bottom edge sits on the bottom of its limits. Its x doesn't change.
	if (attr & ATTR_FLOATABLE) {
		movement_status = MOVEMENT_FLOATING;
		limit = Rect2i{-100000, -100000, 200000, 200000};
		return;
	}
	// whoever lets go, a container (attr 8) whose cabin overlaps the object's own rectangle
	// takes it; the top-most by plane wins (batch 5, Q5)
	auto here = get_bbox();
	Object* container = nullptr;
	Rect2i cabin;
	for (auto* obj : *g_engine_context.objects) {
		auto* vehicle = obj->as_vehicle();
		if (obj == this || !vehicle || !(obj->attr & ATTR_CONTAINER)) {
			continue;
		}
		auto there = obj->get_bbox();
		Rect2i c{numeric_cast<int32_t>(there.x) + vehicle->cabin.x, numeric_cast<int32_t>(there.y) + vehicle->cabin.y, vehicle->cabin.width, vehicle->cabin.height};
		const bool overlaps = numeric_cast<float>(c.x) < here.right() && here.x < numeric_cast<float>(c.right()) && numeric_cast<float>(c.y) < here.bottom() && here.y < numeric_cast<float>(c.bottom());
		if (overlaps && (!container || obj->get_z_order() > container->get_z_order())) {
			container = obj;
			cabin = c;
		}
	}
	if (container) {
		carrier = container->uid;
		movement_status = MOVEMENT_INVEHICLE;
		limit = cabin;
		if (auto* simple = as_simple_object()) {
			simple->part.set_z_order(container->get_z_order());
		}
		const float x = std::clamp(here.x, numeric_cast<float>(limit.x), std::max(numeric_cast<float>(limit.x), numeric_cast<float>(limit.right()) - here.width));
		set_position(x, numeric_cast<float>(limit.bottom()) - here.height);
		return;
	}
	movement_status = MOVEMENT_AUTONOMOUS;
	update_free_limits();
	// F027 / Batch 9 Q9a: nearest floor can be an upper room's floor when
	// releasing high in a lower room. Prefer the containing room instead.
	// A creature can hold an item beyond a ledge; use its planted feet as the
	// support probe. Other releases use the object's centre. Existing finite
	// gap recovery remains the fallback when neither probe lies in a room.
	float x = here.x;
	if (attr & ATTR_WALLBOUND) {
		const auto* creature = released_by ? released_by->as_creature() : nullptr;
		const float probe_x = creature ? creature->ground_x() : here.x + here.width / 2;
		const float probe_y = creature ? creature->ground_y() - 1 : here.y + here.height / 2;
		if (const auto* room = g_engine_context.map->find_room_containing(probe_x, probe_y)) {
			limit = Rect2i{room->left, room->top, room->width(), room->height()};
			if (creature) x = std::clamp(x, numeric_cast<float>(limit.x), std::max(numeric_cast<float>(limit.x), numeric_cast<float>(limit.right()) - here.width));
		}
	}
	set_position(x, numeric_cast<float>(limit.bottom()) - here.height);
}

void Object::update_free_limits() {
	// A free object with attr 64 is limited to the room its bottom edge is nearest to; any
	// other is limited to the whole world (batch 3, Q15 and Q22). Rooms are the ones whose
	// x range contains its left edge, as for creatures (batch 2, Q10).
	if (!(attr & ATTR_WALLBOUND)) {
		limit = Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
		return;
	}
	auto bbox = get_bbox();
	limit = g_engine_context.map->limits_for(bbox.x, bbox.bottom(), limit);
}

int32_t Object::get_z_order() const {
	auto* main_part = get_part(0);
	if (!main_part) {
		throw_exception("Can't get main part of object without any parts: {}", *this);
	}

	return main_part->z_order();
}

Rect2f Object::get_bbox() const {
	if (auto* creature = as_creature()) {
		return creature->get_body_bbox();
	}
	auto* main_part = get_part(0);
	if (!main_part) {
		throw_exception("Can't get main part of object without any parts: {}", *this);
	}

	// TODO: is this correct for CompoundObjects? should we find the max of widths and heights?
	// and find any parts with negative relx/rely?
	return Rect2f{main_part->x(), main_part->y(), numeric_cast<float>(main_part->width()), numeric_cast<float>(main_part->height())};
}

DullPart* Object::get_part(int32_t partnum) {
	return const_cast<DullPart*>(const_cast<const Object*>(this)->get_part(partnum));
}

// Creatures 1 vehicles only carry creatures, as in openc2e's Creatures 1 and 2 support.
void Object::vehicle_grab_passengers() {
	// every free creature whose bounding box overlaps the cabin (batch 5, Q3)
	auto box = get_bbox();
	Rect2f here = box;
	if (auto* vehicle = as_vehicle()) {
		here = Rect2f{box.x + numeric_cast<float>(vehicle->cabin.x), box.y + numeric_cast<float>(vehicle->cabin.y), numeric_cast<float>(vehicle->cabin.width), numeric_cast<float>(vehicle->cabin.height)};
	}
	for (auto* obj : *g_engine_context.objects) {
		if (!obj->as_creature() || g_engine_context.objects->try_get(obj->carrier)) {
			continue;
		}
		auto there = obj->get_bbox();
		if (there.x < here.right() && here.x < there.right() && there.y < here.bottom() && here.y < there.bottom()) {
			obj->carrier = this->uid;
			obj->movement_status = MOVEMENT_INVEHICLE;
			// inside, its limits are the cabin and it stands on the cabin floor (batch 2, Q12)
			auto* creature = obj->as_creature();
			creature->update_limits();
			creature->footx = std::clamp(creature->footx, creature->limit.x, creature->limit.right());
			creature->footy = creature->limit.bottom();
			creature->reposition();
			creature->settle_on_limits();
			creature->pushed_by_limit = false;
		}
	}
}

void Object::vehicle_drop_passengers() {
	// only creatures: an egg in the incubator stays there while the incubator lets the newborn
	// out
	for (auto* obj : *g_engine_context.objects) {
		if (obj->carrier == this->uid && obj->movement_status == MOVEMENT_INVEHICLE && obj->as_creature()) {
			obj->carrier = {};
			obj->movement_status = MOVEMENT_AUTONOMOUS;
			if (auto* creature = obj->as_creature()) {
				creature->land();
			}
		}
	}
}

void Object::vehicle_teleport_passengers(float x, float y) {
	for (auto* obj : *g_engine_context.objects) {
		// everything inside is taken out, moved there with no limits, and then gets its room
		// limits (batch 5, Q1)
		if (obj->carrier == this->uid) {
			obj->carrier = {};
			obj->movement_status = MOVEMENT_AUTONOMOUS;
			obj->limit = Rect2i{-100000, -100000, 200000, 200000};
			if (auto* creature = obj->as_creature()) {
				creature->move_ground_to(x, y);
				creature->settle_on_limits();
				creature->update_limits();
				creature->pushed_by_limit = false;
			} else {
				obj->set_position(x, y);
				auto bbox = obj->get_bbox();
				obj->limit = g_engine_context.map->limits_for(bbox.x, bbox.bottom(), obj->limit);
			}
		}
	}
}

void Object::tick() {
	// TODO: if this function gets too slow, break out various bits into separate Systems that only
	// know about Objects that need to be updated (e.g. an Object would have a TimerSystemHandle into
	// the TimerSystem, and the TimerSystem only updates timer scripts for its managed objects).

	// timer updates
	if (tick_value > 0) {
		ticks_since_last_tick_event += 1;
		if (ticks_since_last_tick_event >= tick_value) {
			ticks_since_last_tick_event = 0;
			if (!g_engine_context.macros->queue_script(this, this, SCRIPT_TIMER)) {
				fmt::print("ERRO [Object::tick] Disabling timer for {}\n", *this);
				tick_value = 0;
			}
			// fmt::print("Fired timer script for {}, {}, {}\n", family, genus, species);
		}
	}

	// animation updates
	for (int32_t partno = 0; true; ++partno) {
		auto* r = get_part(partno);
		if (!r) {
			break;
		}
		r->update_animation();
		// debugging: say once if a part shows a frame it doesn't have (an ANIM, BASE or POSE
		// past the end of its images); it isn't drawn and has no size
		if (!r->gallery().has_frame(r->frame()) && r->gallery().size() > 0 && !reported_missing_frame) {
			reported_missing_frame = true;
			fmt::print("WARN [Frame] {} part {} shows frame {} but has {} images (base {}, pose {})\n",
				format_as(*this), partno, r->frame(), r->gallery().size(), r->base(), r->pose());
		}
	}

	// vehicle position updates
	if (as_vehicle()) {
		if (as_lift() && as_vehicle()->yvel != 0) {
            if (as_lift()->next_or_current_floor < 0 || as_lift()->next_or_current_floor >= as_lift()->floors.ssize()) {
                as_vehicle()->xvel = as_vehicle()->yvel = 0;
                actv = ACTV_INACTIVE;
                return;
            }
			auto current_cabin_bottom = get_bbox().y + as_vehicle()->cabin.bottom();
			auto next_cabin_bottom = current_cabin_bottom + as_vehicle()->yvel;

			auto next_floor_y = as_lift()->floors[as_lift()->next_or_current_floor];

			if ((as_vehicle()->yvel > 0 && next_cabin_bottom >= next_floor_y) || (as_vehicle()->yvel < 0 && next_cabin_bottom <= next_floor_y)) {
				set_position(get_bbox().x + as_vehicle()->xvel, next_floor_y - as_vehicle()->cabin.bottom());
				// stop!
				as_vehicle()->xvel = 0;
				as_vehicle()->yvel = 0;
				// truncate any fractional position
				// TODO: does this result in the correct y position? should we just set it to the next_floor_y + height?
				set_position(std::trunc(get_bbox().x), std::trunc(get_bbox().y));
				if (actv != ACTV_INACTIVE) {
					actv = ACTV_INACTIVE;
					g_engine_context.macros->queue_script(this, this, SCRIPT_DEACTIVATE);
				}
				erase_if(as_lift()->activated_call_buttons, [&](auto& cb) {
					auto call_button = g_engine_context.objects->try_get(cb);
					if (call_button == nullptr) {
						return true;
					}
					if (call_button->as_call_button()->floor == as_lift()->next_or_current_floor) {
						call_button->actv = ACTV_INACTIVE;
						g_engine_context.macros->queue_script(this, call_button, SCRIPT_DEACTIVATE);
						return true;
					}
					return false;
				});
				return;
			}
		}

		auto* vehicle = as_vehicle();
		if (as_lift() || (vehicle->xvel == 0 && vehicle->yvel == 0)) {
			add_position(vehicle->xvel, vehicle->yvel);
		} else {
			// Batch 4, Q3: a vehicle with attr 128 (and not 64) tests its bottom edge against
			// the ground array under its left and right edges each tick it moves; one with
			// attr 64 tests its edges against its limits. A new hit stops it, undoes the move
			// and sends it its collision event (6).
			const auto before = get_bbox();
			add_position(vehicle->xvel, vehicle->yvel);
			const auto after = get_bbox();
			uint32_t bump = 0;
			if ((attr & ATTR_GROUNDBOUND) && !(attr & ATTR_WALLBOUND)) {
				const auto& ground = g_engine_context.map->get_groundlevel();
				auto strip = [&](float x) {
					const int32_t i = static_cast<int32_t>(std::floor(x / 32));
					return ground[static_cast<size_t>(((i % 261) + 261) % 261)];
				};
				const float left = static_cast<float>(strip(after.x)), right = static_cast<float>(strip(after.right()));
				const float bottom = after.bottom();
				if (bottom > left && bottom > right) {
					bump = 8;
				} else if (bottom > left) {
					bump = 1;
				} else if (bottom > right) {
					bump = 2;
				} else if (after.y < 0) {
					bump = 4;
				}
			} else if (attr & ATTR_WALLBOUND) {
				if (after.x < static_cast<float>(limit.x)) {
					bump |= 1;
				}
				if (after.right() > static_cast<float>(limit.right())) {
					bump |= 2;
				}
				if (after.y < static_cast<float>(limit.y)) {
					bump |= 4;
				}
				if (after.bottom() > static_cast<float>(limit.bottom())) {
					bump |= 8;
				}
			}
			if (bump != 0 && bump != vehicle->bump) {
				vehicle->xvel = vehicle->yvel = 0;
				set_position(before.x, before.y);
				g_engine_context.macros->queue_script(this, this, SCRIPT_COLLISION);
			}
			vehicle->bump = bump;
		}
	}

	// lift caller updates
	if (as_lift() && actv == ACTV_INACTIVE && obv0 == 0) {
		// any callers?

		auto current_cabin_bottom = get_bbox().y + as_vehicle()->cabin.bottom();

		int32_t best_y;
		int32_t best_floor_id;
		ObjectHandle best_call_button;
		erase_if(as_lift()->activated_call_buttons, [&](auto& handle) {
			auto* cb = g_engine_context.objects->try_get(handle);
			if (!cb || !cb->as_call_button() || cb->as_call_button()->floor >= as_lift()->floors.size()) {
				return true;
			}
			auto& floor = as_lift()->floors[cb->as_call_button()->floor];
			if (!best_call_button || abs(current_cabin_bottom - floor) < abs(current_cabin_bottom - best_y)) {
				best_y = floor;
				best_floor_id = numeric_cast<int32_t>(cb->as_call_button()->floor);
				best_call_button = handle;
			}
			return false;
		});

		if (best_call_button) {
			if (best_y >= current_cabin_bottom) {
				as_lift()->next_or_current_floor = best_floor_id;
				actv = ACTV_ACTIVE1;
				g_engine_context.macros->queue_script(this->uid, this->uid, SCRIPT_ACTIVATE1);
			} else {
				as_lift()->next_or_current_floor = best_floor_id;
				actv = ACTV_ACTIVE2;
				g_engine_context.macros->queue_script(this->uid, this->uid, SCRIPT_ACTIVATE2);
			}
		}
	}

	// creatures
	if (as_creature()) {
		as_creature()->creature_tick();
	}
	if (auto* bubble = as_bubble()) {
		bubble->bubble_tick();
	}
}

// Which creatures a stimulus reaches (spec Q2): every creature except the source that can
// hear it (SHOU), see it (SIGN) or is touching it (TACT); WRIT reaches only the named creature.
static std::vector<Creature*> stimulus_receivers(Object* source, Object::StimulusScope scope, Object* writ_target) {
	std::vector<Creature*> result;
	if (scope == Object::STIM_WRIT) {
		if (writ_target && writ_target->as_creature()) {
			result.push_back(writ_target->as_creature());
		}
		return result;
	}
    if (source->unplaced) return result;
	auto here = source->get_bbox();
    if (here.y >= CREATURES1_WORLD_HEIGHT || here.bottom() <= 0) return result;
	for (auto* obj : *g_engine_context.objects) {
		auto* creature = obj->as_creature();
		if (!creature || obj == source || creature->ground_x() < 0 || creature->ground_x() > CREATURES1_WORLD_WIDTH
            || creature->ground_y() < 0 || creature->ground_y() > CREATURES1_WORLD_HEIGHT) {
			continue;
		}
		bool reaches = false;
		switch (scope) {
			case Object::STIM_SHOU: reaches = creature->can_hear(*source); break;
			case Object::STIM_SIGN: reaches = creature->can_see(*source); break;
			case Object::STIM_TACT: {
				auto there = obj->get_bbox();
				reaches = (there.x < here.right() && here.x < there.right() && there.y < here.bottom() && here.y < there.bottom()) || ((source->attr & ATTR_CONTAINER) && obj->carrier == source->uid);
			} break;
			case Object::STIM_WRIT: break;
		}
		if (reaches) {
			result.push_back(creature);
		}
	}
	return result;
}

void Object::stimulate(StimulusScope scope, Object* writ_target, const sfc::StimulusV1& stimulus) {
	// script stimuli are queued and delivered every 4 ticks
	for (auto* creature : stimulus_receivers(this, scope, writ_target)) {
		creature->queue_stimulus(stimulus, this->uid);
	}
}

void Object::stimulate_numbered(StimulusScope scope, Object* writ_target, int32_t stimulus_number) {
	for (auto* creature : stimulus_receivers(this, scope, writ_target)) {
		if (stimulus_number < 0 || numeric_cast<size_t>(stimulus_number) >= creature->stimuli.size()) {
			throw Exception(fmt::format("Creature has no stimulus {}", stimulus_number));
		}
		creature->queue_stimulus(creature->stimuli[numeric_cast<size_t>(stimulus_number)], this->uid);
	}
}

void Object::creature_stim_disappoint(Object* refused_by) {
	// disappointment is genetic stimulus 0, from the object that refused (spec Q23)
	if (auto* creature = as_creature()) {
		creature->receive_numbered_stimulus(0, refused_by ? refused_by->uid : ObjectHandle{});
	}
}

void Object::render(RenderSystem& rendersystem) const {
	int32_t partnum = 0;
	while (const auto* part = this->get_part(partnum++)) {
		if (!part->gallery().has_frame(part->frame())) {
			continue;
		}
		rendersystem.add(create_render_item_texture(
			part->gallery().texture,
			part->gallery().texture_locations[numeric_cast<size_t>(part->frame())],
			part->x(),
			part->y(),
			part->z_order()));
	}
}

void Object::serialize(SFCContext& ctx, sfc::ObjectV1* p) {
	if (ctx.is_storing()) {
		p->species = species;
		p->genus = genus;
		p->family = family;
		p->movement_status = movement_status;
		p->attr = attr;
		p->limit = limit;
		p->carrier = ctx.dump_object(g_engine_context.objects->try_get(carrier)).get();
		p->actv = actv;
		if (p->gallery == nullptr) {
			throw Exception("Expected gallery to be non-null by this point, it should have been"
							" set by a serialization function for a more specialized object type");
		}
		p->tick_value = tick_value;
		p->ticks_since_last_tick_event = ticks_since_last_tick_event;
		p->objp = ctx.dump_object(g_engine_context.objects->try_get(objp)).get();
		if (current_sound.get_looping()) {
			p->current_sound = current_sound.get_name();
		}
		p->obv0 = obv0;
		p->obv1 = obv1;
		p->obv2 = obv2;
		// TODO: I think we can skip scripts here, since they'll be loaded from the global
		// scriptorium anyways?
		// p->scripts = scripts;
	} else {
		species = p->species;
		genus = p->genus;
		family = p->family;
		movement_status = MovementStatus(p->movement_status);
		attr = p->attr;
		limit = p->limit;
		carrier = ctx.load_object(p->carrier);
		actv = ActiveFlag(p->actv);
		// creaturesImage sprite;
		tick_value = p->tick_value;
		ticks_since_last_tick_event = p->ticks_since_last_tick_event;
		// Set the sound later, once we've loaded the object's position from its Entities
		// current_sound = p->current_sound;
		objp = ctx.load_object(p->objp);
		obv0 = p->obv0;
		obv1 = p->obv1;
		obv2 = p->obv2;

		// do this _after_ loading the SimpleObject or CompoundObject data,
		// since only then do we know the object's position / bounding box
		// TODO: should probably somewhere else? like global initialization after everything's been loaded?
		if (!p->current_sound.empty()) {
			// these won't be audible immediately, since the SoundManager thinks
			// they're out of hearing range. once the game starts and the
			// listener viewport gets set these will start being audible.
			current_sound = g_engine_context.sounds->play_controlled_sound(p->current_sound, get_bbox(), true);
		}
	}
}
