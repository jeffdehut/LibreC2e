#pragma once

#include "opencreatures1/ImageGallery.h"

#include "common/math/Rect.h"

#include <array>
#include <stdint.h>
#include <vector>

class Room {
  public:
	int32_t left;
	int32_t top;
	int32_t right;
	int32_t bottom;
	int32_t type;

	int32_t width() const {
		return right - left;
	}
	int32_t height() const {
		return bottom - top;
	}
};

class MapManager {
  public:
	void add_room(Room&&);
	const auto& get_rooms() const { return rooms; }
	// the first room containing the point, or nullptr
	const Room* find_room_containing(float x, float y) const;
	// The limits for something whose left edge is at x and bottom edge at bottom (batch 5,
	// Q1): among the rooms with left <= x <= right, the one whose floor is nearest the bottom
	// edge, the first in the room table on a tie. If there's none, the old limits with their
	// bottom at 9999.
	Rect2i limits_for(float x, float bottom, Rect2i old) const;
	// The room something let go at (x, y) should land in (not in the original): the room
	// containing the point; else the first floor below it in that column; else the nearest
	// floor above. nullptr if no room spans x.
	const Room* room_to_land_in(float x, float y) const;

	void set_background(ImageGallery background);
	const ImageGallery& get_background() const { return background; }

	void set_groundlevel(const std::array<uint32_t, 261>&);
	const auto& get_groundlevel() const { return groundlevel; }

	int32_t get_world_wrap_width() const;

	void render(class RenderSystem&) const;

	// debugging: draw room outlines and the ground level over the world
	bool show_rooms = false;
	bool show_ground_level = false;

  private:
	std::vector<Room> rooms;
	ImageGallery background;
	std::array<uint32_t, 261> groundlevel;
};