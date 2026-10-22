#pragma once

#include "SimpleObject.h"

#include <stdint.h>
#include <string>

namespace sfc {
struct BubbleV1;
}

struct Bubble : SimpleObject {
	// ticks left before the bubble disappears
	uint8_t life = 0;
	ObjectHandle creator;
	std::string text;
	// where the bubble sits relative to its speaker's top-left corner; it follows the
	// speaker every tick (batch 4, Q18). Not saved: worked out again when a world loads.
	bool has_offset = false;
	float offset_x = 0;
	float offset_y = 0;

	void serialize(SFCContext&, sfc::BubbleV1*);
	void render(RenderSystem&) const override;
	void bubble_tick();
};

// A speech bubble above the speaker, lasting the given number of ticks (batch 2, Q23), or a
// thought cloud (only the hand's typing uses those, batch 4, Q25). 0 ticks lasts until it's
// destroyed.
Bubble* make_speech_bubble(Object& speaker, const std::string& text, int32_t ticks, bool thought = false);
// the most text a bubble shows (batch 4, Q18)
constexpr size_t BUBBLE_MAX_TEXT = 24;
