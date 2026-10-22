#include "ObjectManager.h"
#include "CallButton.h"
#include "Creature.h"
#include "Lift.h"
#include "PointerManager.h"
#include "WorldState.h"

void ObjectManager::destroy_pending() {
	for (auto handle : m_to_destroy) {
		if (!m_pool.contains(handle)) continue;
		auto& hand = *g_engine_context.pointer;
		if (hand.m_carried_object == handle) hand.m_carried_object = {};
		if (hand.m_edited_object == handle) {
			hand.m_edited_object = {};
			hand.m_edit_free = false;
		}
		if (g_engine_context.world->current_norn == handle) g_engine_context.world->current_norn = {};
		for (auto* obj : *this) {
            if (obj->objp == handle) obj->objp = {};
			if (obj->carrier == handle) {
				obj->carrier = {};
				obj->movement_status = MOVEMENT_AUTONOMOUS;
			}
			if (auto* c = obj->as_creature()) {
				if (c->attention == handle) {
					c->attention = {};
					c->has_target = false;
				}
				for (auto& memory : c->attention_memory) {
					if (memory.object == handle) memory = {};
				}
			}
			if (auto* button = obj->as_call_button(); button && button->lift == handle) {
				button->lift = {};
				button->actv = ACTV_INACTIVE;
			}
			if (auto* lift = obj->as_lift()) lift->activated_call_buttons.erase(handle);
		}
		m_pool.erase(handle);
        ++m_revision;
	}
	m_to_destroy.clear();
}
