#pragma once

#include "objects/ObjectHandle.h"

#include <stdint.h>

class Object;
// Intentional native hit testing: controls, enclosed objects, then drawing plane.
Object* find_object_under_pointer(float worldx, float worldy, uint8_t required_attr);

struct BackendEvent;

class PointerManager {
  public:
	ObjectHandle m_pointer_tool;
	// After a world loads: whatever the hand held when it was saved is held again (only the
	// object's side is saved). Only one thing fits; anything else is let go.
	void restore_hold();
	// object attached to the pointer by EDIT, e.g. a newly injected object being placed
	ObjectHandle m_edited_object;
	// object picked up with a right-click, held at the offset it was grabbed at
	ObjectHandle m_carried_object;
	float m_carried_offset_x = 0;
	float m_carried_offset_y = 0;

	void pick_up(ObjectHandle object, float object_x, float object_y);

	// The hand's own animation scripts (events 50-54: activate 1, activate 2, deactivate,
	// pick up, let go) run on the hand, using the script under the target's classifier if
	// there is one, otherwise the hand's own (batch 4, Q6-Q8).
	void run_hand_script(Object* target, int32_t event);

	// The editing option (batch 2, Q12; batch 3, Q2): the hand can pick up creatures, and
	// Shift + left-click grabs any object to drag it freely. The interface sets shift_down.
	bool m_editing = false;
	bool m_shift_down = false;
	// set while m_edited_object was grabbed with Shift (it stays where it's let go)
	bool m_edit_free = false;
	// set while the mouse is over the interface rather than the world
	bool m_over_interface = false;
	// set during a strict or relaxed wolfling run: the hand can't touch the world
	bool m_locked = false;

	float m_screenx;
	float m_screeny;

	PointerManager() {}
	void update();
	void handle_event(const BackendEvent& event);
};