#include "PointerManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "MacroManager.h"
#include "MessageManager.h"
#include "ViewportManager.h"
#include "common/backend/BackendEvent.h"
#include "objects/Creature.h"
#include "objects/CompoundObject.h"
#include "common/math/ModularArithmetic.h"
#include <algorithm>
#include <vector>
#include "objects/ObjectManager.h"
#include "objects/PointerTool.h"

#include <fmt/format.h>
#include <stdint.h>

constexpr float HAND_HOTSPOT = 2;
// where the hand's grabbing frame (syst.spr frame 6) closes, from its top-left corner; a held
// object's top hides this far behind the fingertips
constexpr float HAND_GRIP_X = 21;
constexpr float HAND_GRIP_Y = 22;
constexpr float HAND_GRIP_TUCK = 8;

void PointerManager::update() {
	Object* obj = g_engine_context.objects->try_get(m_pointer_tool);
	if (!obj) {
		return;
	}

	auto worldx = g_engine_context.viewport->window_x_to_world_x(m_screenx);
	auto worldy = g_engine_context.viewport->window_y_to_world_y(m_screeny);

	constexpr const int32_t too_big = (1 << 20);
	if (worldx >= too_big || worldx <= -too_big || worldy >= too_big || worldy <= -too_big) {
		// this can happen before the viewport manager is properly initialized. it causes
		// issues later because we try to convert some floats back to int32_t but the floats
		// are too big and would lose precision! these aren't real coordinates anyways so
		// just skip and wait until the viewport manager is correctly returning information.
		// TODO: properly make sure viewport manager is initialized instead of this hack
		fmt::print("warning: pointermanager got nonsense world coordinates {} {} from viewportmanager, skipping update\n",
			worldx, worldy);
		return;
	}

	// the hand's top-left corner follows the mouse (batch 4, Q9)
	obj->set_position(worldx, worldy);

	if (auto* edited = g_engine_context.objects->try_get(m_edited_object)) {
		edited->set_position(worldx, worldy);
	}
	if (auto* carried = g_engine_context.objects->try_get(m_carried_object)) {
		// Not in the original, which hangs the object's top-left corner from the mouse point
		// (batch 10, Q1), so things seemed to float off to one side. Here whatever is held
		// settles, over about a tenth of a second from where it was grabbed, into the grip of
		// the hand's grabbing frame: centred under the gap between fingers and thumb, its
		// top tucked behind the fingertips (it's drawn just behind the hand).
		using namespace std::chrono;
		static auto last = steady_clock::now();
		const auto now = steady_clock::now();
		const float seconds = std::clamp(duration<float>(now - last).count(), 0.f, 0.1f);
		last = now;
		const auto box = carried->get_bbox();
		const float aim_x = HAND_GRIP_X - box.width / 2;
		const float aim_y = HAND_GRIP_Y - HAND_GRIP_TUCK;
		const float part = 1 - std::exp(-seconds / 0.035f);
		m_carried_offset_x += (aim_x - m_carried_offset_x) * part;
		m_carried_offset_y += (aim_y - m_carried_offset_y) * part;
		if (auto* creature = carried->as_creature()) {
			// a held creature hangs from the hand by its top (batch 2, Q12)
			creature->move_ground_to(worldx + m_carried_offset_x + box.width / 2, worldy + m_carried_offset_y + box.height);
		} else {
			carried->set_position(worldx + m_carried_offset_x, worldy + m_carried_offset_y);
		}
	}
}

void PointerManager::run_hand_script(Object* target, int32_t event) {
	auto* hand = g_engine_context.objects->try_get(m_pointer_tool);
	if (!hand) {
		return;
	}
	const auto script = static_cast<ScriptNumber>(event);
	if (target && g_engine_context.macros->queue_script(hand, hand, target->family, target->genus, target->species, script)) {
		return;
	}
	g_engine_context.macros->queue_script(hand, hand, script);
}

void PointerManager::restore_hold() {
	m_carried_object = {};
	auto* hand = g_engine_context.objects->try_get(m_pointer_tool);
	if (!hand) {
		return;
	}
	for (auto* obj : *g_engine_context.objects) {
		if (obj->carrier != m_pointer_tool || obj == hand) {
			continue;
		}
		if (!m_carried_object) {
			obj->held_by_hand(*hand);
		} else {
			obj->carrier = {};
			obj->movement_status = MOVEMENT_AUTONOMOUS;
		}
	}
}

void PointerManager::pick_up(ObjectHandle object, float object_x, float object_y) {
	m_carried_object = object;
	m_carried_offset_x = object_x - g_engine_context.viewport->window_x_to_world_x(m_screenx);
	m_carried_offset_y = object_y - g_engine_context.viewport->window_y_to_world_y(m_screeny);
}


// The topmost object under the pointer with any of the given ATTR flags.
// Source-informed F028 policy from LibreCreatures objects/object.cpp: prefer a
// real control hotspot; discard enclosing backgrounds; use plane for partial overlaps.
Object* find_object_under_pointer(float worldx, float worldy, uint8_t required_attr) {
    struct Hit { Object* object; Rect2f box; bool control; };
    std::vector<Hit> hits;
    for (auto* obj : *g_engine_context.objects) {
        if (obj->as_pointer_tool() || !(obj->attr & required_attr)) continue;
        if (required_attr == ATTR_MOUSEABLE && obj->as_creature() && !g_engine_context.pointer->m_editing) continue;
        if (!obj->as_creature() && !obj->get_part(0)) continue;
        if (auto* c = obj->as_creature(); c && !c->body) continue;
        auto box = obj->get_bbox();
        if (!box.has_point_modx(worldx, worldy, CREATURES1_WORLD_WIDTH)) continue;
        box.x = worldx - mod_remainder(worldx - box.x, float(CREATURES1_WORLD_WIDTH));
        bool control = false;
        if (required_attr == ATTR_ACTIVATEABLE) {
            if (auto* compound = obj->as_compound_object()) {
                for (size_t function = 3; function < 6; ++function) {
                    const auto spot = compound->functions_to_hotspots[function];
                    if (spot >= 0 && static_cast<size_t>(spot) < compound->hotspots.size()
                        && compound->hotspots[static_cast<size_t>(spot)].has_point(worldx - box.x, worldy - box.y)) control = true;
                }
            }
        }
        hits.push_back({obj, box, control});
    }
    Object* best = nullptr;
    const bool control_hit = std::any_of(hits.begin(), hits.end(), [](const Hit& h) { return h.control; });
    for (const auto& hit : hits) {
        if (control_hit && !hit.control) continue;
        if (!control_hit && std::any_of(hits.begin(), hits.end(), [&](const Hit& other) {
            return hit.object != other.object && hit.box != other.box
                && hit.box.x <= other.box.x && hit.box.y <= other.box.y
                && hit.box.right() >= other.box.right() && hit.box.bottom() >= other.box.bottom();
        })) continue;
        if (!best || hit.object->get_z_order() > best->get_z_order()) best = hit.object;
    }
    return best;
}

void PointerManager::handle_event(const BackendEvent& event) {
	if (event.type == eventmousemove || event.type == eventmousebuttondown || event.type == eventmousebuttonup) {
		m_screenx = event.x;
		m_screeny = event.y;
	}
	if ((m_over_interface || m_locked) && (event.type == eventmousebuttondown || event.type == eventmousebuttonup)) {
		return;
	}
	if (event.type == eventmousebuttondown && event.button == buttonright) {
		// Right-clicks pick up and drop objects
		float worldx = g_engine_context.viewport->window_x_to_world_x(event.x);
		float worldy = g_engine_context.viewport->window_y_to_world_y(event.y);
		Object* pntr = g_engine_context.objects->try_get(m_pointer_tool);
		if (pntr) {
			// the click point is the tip of the pointing finger, 2 px right and down of the
			// hand's corner; the saved offset isn't used (batch 4, Q9)
			worldx += HAND_HOTSPOT;
			worldy += HAND_HOTSPOT;
		}

		if (g_engine_context.objects->try_get(m_carried_object)) {
			g_engine_context.messages->hand_message(m_pointer_tool, m_carried_object, MESSAGE_DROP);
			run_hand_script(g_engine_context.objects->try_get(m_carried_object), SCRIPT_POINTER_DROP);
		} else if (Object* obj = find_object_under_pointer(worldx, worldy, ATTR_MOUSEABLE)) {
			g_engine_context.messages->hand_message(m_pointer_tool, obj->uid, MESSAGE_PICKUP);
			run_hand_script(obj, SCRIPT_POINTER_PICKUP);
		}
		return;
	}
	if (event.type == eventmousebuttondown && event.button == buttonleft) {
		// Left-button clicks
		// What happens here? We look over all objects whose bounding boxes contain
		// the click location, and find the topmost (largest z-order) one that
		// has ATTR & Activateable.
		// Note that we look for Objects, not Parts - in Creatures 1, CompoundObjects
		// must have all of their parts w/in the bounding box of the first/main part.
		// We use the main part's z-order as the object overall z-order.

		float worldx = g_engine_context.viewport->window_x_to_world_x(event.x);
		float worldy = g_engine_context.viewport->window_y_to_world_y(event.y);

		Object* pntr = g_engine_context.objects->try_get(m_pointer_tool);
		if (pntr) {
			// the click point is the tip of the pointing finger, 2 px right and down of the
			// hand's corner; the saved offset isn't used (batch 4, Q9)
			worldx += HAND_HOTSPOT;
			worldy += HAND_HOTSPOT;
		}

		fmt::print("click @ {} {}\n", worldx, worldy);

		// a click puts down an object attached by EDIT
		if (auto* edited = g_engine_context.objects->try_get(m_edited_object)) {
			// a Shift-dragged object stays where it's let go (only possible with privileges);
			// anything else is placed as if the hand had dropped it (batch 4, Q1 and Q5)
			edited->carrier = {};
			if (m_edit_free) {
				// its limits are worked out again where it was let go (batch 5, Q1)
				edited->movement_status = MOVEMENT_AUTONOMOUS;
				edited->update_free_limits();
			} else {
				edited->land_object(pntr);
			}
			m_edited_object = {};
			m_edit_free = false;
			return;
		}

		// Shift + left-click picks up a creature (not in the original, where only privileged
		// players could), unless the editing option's free drag is on
		if (m_shift_down && !m_editing && !g_engine_context.objects->try_get(m_carried_object)) {
			Creature* found = nullptr;
			for (auto* obj : *g_engine_context.objects) {
				auto* c = obj->as_creature();
				if (c && c->body && c->get_bbox().has_point_modx(worldx, worldy, CREATURES1_WORLD_WIDTH) && (!found || c->z_order > found->z_order)) {
					found = c;
				}
			}
			if (found) {
				g_engine_context.messages->hand_message(m_pointer_tool, found->uid, MESSAGE_PICKUP);
				run_hand_script(found, SCRIPT_POINTER_PICKUP);
				return;
			}
		}

		// Shift + left-click with the editing option on grabs any object to drag it freely,
		// ignoring its limits (batch 3, Q3); the next click puts it down
		if (m_editing && m_shift_down) {
			if (Object* obj = find_object_under_pointer(worldx, worldy, 0xff); obj && !obj->as_creature()) {
				obj->movement_status = MOVEMENT_MOUSEDRIVEN;
				obj->carrier = m_pointer_tool;
				m_edited_object = obj->uid;
				m_edit_free = true;
			}
			return;
		}

		// TODO: move this into some sort of generic spatial query system, shared with
		// ENUM, ESEE, collisions, etc.
		Object* best_object = find_object_under_pointer(worldx, worldy, ATTR_ACTIVATEABLE);

		if (best_object) {
			auto bbox = best_object->get_bbox();
			// TODO: better way to handle world wrap?
			float relx = worldx - bbox.x;
			float rely = worldy - bbox.y;
			if (relx < 0) {
				relx += CREATURES1_WORLD_WIDTH;
			}
			fmt::print("clickrel {} {}\n", relx, rely);
			best_object->handle_left_click(relx, rely);
		}
	}
}