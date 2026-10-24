#pragma once

#include "common/StaticVector.h"
#include "objects/ObjectHandle.h"

#include <array>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

struct Macro {
	Macro() = default;
	explicit Macro(std::string script_)
		: script(std::move(script_)) {}

	bool selfdestruct = false;
	bool inst = false;
	std::string script;
	uint32_t ip = 0;
	StaticVector<int32_t, 20> stack;
	std::array<int32_t, 10> vars{};
	ObjectHandle ownr;
	ObjectHandle from;
	ObjectHandle exec;
	ObjectHandle targ;
	ObjectHandle pending_creation;
	bool stop_after_placement = false;
	ObjectHandle _it_;
	int32_t part = 0;
	// the last subroutine GSUB looked up and where its body starts: a cache, not a return
	// address (those are on the flow stack; batch 11, Q1)
	std::string subroutine_label;
	uint32_t subroutine_address = 0;
	int32_t wait = 0;
	// Runtime turns elapsed, capped at the feeding-assist budget (not part of SFC files).
	uint16_t meal_protection_ticks = 0;
	bool destroy_as_soon_as_possible = false;
	// started at once by MacroManager::start_script_now: it has had this tick's turn
	bool ran_this_tick = false;
	// the script's classifier and event, for error messages (-1 when it isn't a script)
	std::array<uint8_t, 3> classifier{};
	int32_t event = -1;

	// for each open ENUM, innermost last, the objects it has still to visit (taken from the
	// back); not saved: a loop loaded from a world file finds them again from its frame
	std::vector<std::vector<ObjectHandle>> enum_snapshots;
};