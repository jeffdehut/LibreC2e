#pragma once

#include "ObjectHandle.h"
#include "Vehicle.h"
#include "common/StaticVector.h"

#include <algorithm>
#include <array>
#include <stdint.h>

namespace sfc {
struct LiftV1;
}

// The lift's calls in the original's eight slots: a call takes the first free one, and on a tie
// the earliest is answered first (LibreCreatures).
struct CallSlots {
	std::array<ObjectHandle, 8> slot{};
	size_t size() const {
		return static_cast<size_t>(std::count_if(slot.begin(), slot.end(), [](ObjectHandle h) { return bool(h); }));
	}
	bool contains(ObjectHandle h) const {
		return std::find(slot.begin(), slot.end(), h) != slot.end();
	}
	// false when all eight are taken
	bool insert(ObjectHandle h) {
		if (contains(h)) {
			return true;
		}
		for (auto& s : slot) {
			if (!s) {
				s = h;
				return true;
			}
		}
		return false;
	}
	void erase(ObjectHandle h) {
		for (auto& s : slot) {
			if (s == h) {
				s = {};
			}
		}
	}
};

struct Lift : Vehicle {
	int32_t next_or_current_floor = -1;
	StaticVector<int32_t, 8> floors;
	CallSlots activated_call_buttons;
	// the call slot being answered, or -1, and how many 16-tick call checks to skip (one after
	// each arrival), as the original saves them
	int32_t answering_call = -1;
	uint8_t call_checks_to_skip = 0;

	void serialize(SFCContext&, sfc::LiftV1*);
};
