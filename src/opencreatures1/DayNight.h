#pragma once

#include "common/math/Rect.h"

#include <cstdint>
#include <imgui.h>
#include <vector>

struct SDL_Renderer;

// A day and night for the modern layout (not in the original, and only to look at: the
// creatures' own sense of light stays the original's). A day lasts an hour at the original's
// speed, on the world's own clock. Daylight comes from the sun's side of the sky, is blocked by
// walls, roofs and the ground, and dims and turns blue-green in the sea; the lamps underground
// glow and pulse; and a soft glow at the edge of the view shows where the sun (or the moon) is.
namespace DayNight {

constexpr uint32_t DAY_TICKS = 36000;

// 0 is midnight, 0.25 sunrise, 0.5 noon and 0.75 sunset
float time_of_day(uint32_t tick);
// 1 on the world's first day
uint32_t day_number(uint32_t tick);

// The capsule's dial: the sky for the time of day over the ground, and the sun (or the moon) on
// its way round.
void draw_dial(ImDrawList* draw, ImVec2 centre, float radius, float t);

// While a recording plays back (Recording.h): the tick it shows, and where its eggs and its
// hand are, so its day and night and its glows are the recording's. nullptr for the world.
struct Shown {
	uint32_t tick = 0;
	std::vector<Rect2f> eggs, hands;
	std::vector<uint32_t> egg_seeds;
};
void show_playback(const Shown* shown);

// Draws the world's light over the world view, then the glow at its edge. Call after the world
// is drawn and before the interface.
void render(SDL_Renderer* renderer);

} // namespace DayNight
