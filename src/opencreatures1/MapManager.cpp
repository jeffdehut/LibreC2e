#include "MapManager.h"

#include "common/NumericCast.h"
#include <cmath>
#include <algorithm>
#include <limits>
#include "common/render/RenderSystem.h"
#include "EngineContext.h"
#include "HdArt.h"
#include "ViewportManager.h"


void MapManager::add_room(Room&& room) {
	rooms.emplace_back(std::move(room));
}

void MapManager::set_background(ImageGallery background_) {
	background = background_;
}

void MapManager::set_groundlevel(const std::array<uint32_t, 261>& groundlevel_) {
	groundlevel = groundlevel_;
}

int32_t MapManager::get_world_wrap_width() const {
	// should always be 8352
	return background.width(0);
}

void MapManager::render(RenderSystem& rendersystem) const {
	rendersystem.add(
		create_render_item_texture(
			get_background().texture,
			get_background().texture_locations[0],
			0, 0, 0),
		LAYER_BACKGROUND);
	// the higher-resolution tiles over it (each where its 1x tile is), those in view asked for
	// first so they're decoded first
	if (HdArt::enabled() && background.width(0) == 58 * 144) {
		const auto& camera = g_engine_context.viewport->get_main_camera_rect();
		const int32_t first = camera.x / 144, last = (camera.x + camera.width) / 144 + 1;
		std::vector<int32_t> order;
		for (int32_t column = first; column <= last; ++column) {
			for (int32_t row = 0; row < 8; ++row) {
				order.push_back((column % 58) * 8 + row);
			}
		}
		for (int32_t i = 0; i < 58 * 8; ++i) {
			order.push_back(i);
		}
		std::vector<bool> added(58 * 8, false);
		for (const int32_t tile : order) {
			if (added[static_cast<size_t>(tile)]) {
				continue;
			}
			added[static_cast<size_t>(tile)] = true;
			if (const auto* hd = HdArt::background_tile(tile)) {
				RenderItem item = create_render_item_texture(hd->texture, hd->src, static_cast<float>(tile / 8 * 144), static_cast<float>(tile % 8 * 150), 1);
				item.dest.width = 144;
				item.dest.height = 150;
				rendersystem.add(item, LAYER_BACKGROUND);
			}
		}
	}

	if (show_rooms) {
		for (const auto& room : get_rooms()) {
			Color color;
			if (room.type == 0) { // indoors
				color = {0xff, 0xff, 0, 0xcc}; // yellow
			} else if (room.type == 1) { // surface
				color = {0, 0xff, 0xff, 0xcc}; // aqua
			} else if (room.type == 2) { // undersea
				color = {0xff, 0, 0xff, 0xcc}; // magenta
			} else {
				color = {0, 0, 0, 0xcc}; // black
			}
			rendersystem.add(
				create_render_item_rect(
					Rect2f(
						numeric_cast<float>(room.left),
						numeric_cast<float>(room.top),
						numeric_cast<float>(room.width()),
						numeric_cast<float>(room.height())),
					color),
				LAYER_DEBUG);
		}
	}

	if (show_ground_level) {
		for (size_t i = 0; i < 261; ++i) {
			rendersystem.add(
				create_render_item_line(
					numeric_cast<float>(i * 32), // TODO: i * CREATURES1_WORLD_WIDTH / 261 ?
					numeric_cast<float>((i + 1) * 32),
					numeric_cast<float>(get_groundlevel()[i]),
					numeric_cast<float>(get_groundlevel()[(i + 1) % 261]),
					Color{0xff, 0xff, 0xff, 0xcc}), // white
				LAYER_DEBUG);
		}
	}
}
const Room* MapManager::find_room_containing(float x, float y) const {
	for (const auto& room : rooms) {
		if (x >= room.left && x < room.right && y >= room.top && y < room.bottom) {
			return &room;
		}
	}
	return nullptr;
}

Rect2i MapManager::limits_for(float x, float bottom, Rect2i old) const {
	const Room* best = nullptr;
	for (const auto& room : rooms) {
		if (x < numeric_cast<float>(room.left) || x > numeric_cast<float>(room.right)) {
			continue;
		}
		if (!best || std::abs(numeric_cast<float>(room.bottom) - bottom) < std::abs(numeric_cast<float>(best->bottom) - bottom)) {
			best = &room;
		}
	}
	if (best) {
		return Rect2i{best->left, best->top, best->width(), best->height()};
	}
    // Approved correction F022: a genuine gap must not open a floor to y=9999.
    // Choose the nearest finite room rectangle, retaining file order on a tie.
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto& room : rooms) {
        const double dx = std::max({double(room.left) - x, 0.0, double(x) - room.right});
        const double dy = std::max({double(room.top) - bottom, 0.0, double(bottom) - room.bottom});
        const double distance = dx * dx + dy * dy;
        if (room.width() > 0 && room.height() > 0 && distance < nearest) {
            best = &room;
            nearest = distance;
        }
    }
    if (best) return Rect2i{best->left, best->top, best->width(), best->height()};
    return Rect2i{0, 0, 8352, 1200};
}

const Room* MapManager::room_to_land_in(float x, float y) const {
	const Room* below = nullptr;
	const Room* above = nullptr;
	for (const auto& room : rooms) {
		if (x < numeric_cast<float>(room.left) || x > numeric_cast<float>(room.right) || room.width() <= 0 || room.height() <= 0) {
			continue;
		}
		if (y >= numeric_cast<float>(room.top) && y <= numeric_cast<float>(room.bottom)) {
			return &room;
		}
		if (numeric_cast<float>(room.bottom) >= y) {
			if (!below || room.bottom < below->bottom) {
				below = &room;
			}
		} else if (!above || room.bottom > above->bottom) {
			above = &room;
		}
	}
	return below ? below : above;
}
