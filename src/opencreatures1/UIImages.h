#pragma once

#include "ImageGallery.h"
#include "common/math/Rect.h"

#include <imgui.h>
#include <stdint.h>
#include <string>
#include <vector>

class RenderSystem;
struct SDL_Renderer;
struct Creature;

// The game's own artwork in interface windows: sprite frames, and live views of the world.
namespace UIImages {
void world_changed();

// Draws one frame of a sprite gallery as an ImGui image, scaled; returns false if there's
// nothing to draw. The frame keeps its own proportions within the given height (0 for its
// own size).
bool sprite(const ImageGallery& gallery, int32_t frame, float height = 0, ImU32 tint = IM_COL32_WHITE);
// The same, by sprite file name, e.g. sprite("eggs", 0).
bool sprite(const char* name, int32_t frame, float height = 0, ImU32 tint = IM_COL32_WHITE);

// A bitmap from the game's folder (e.g. "Hatchery/EGG0.bmp"), as a texture. In the kits'
// 8-bit pictures palette entry 0 is the see-through colour. Returns nullptr if it can't be
// loaded.
struct Bitmap {
	ImTextureID texture{};
	int32_t width = 0;
	int32_t height = 0;
};
const Bitmap* bitmap(const std::string& path);
// a bitmap resource from a Windows program in the game's folder (e.g. Creatures.exe's
// toolbar strip, resource 128)
const Bitmap* exe_bitmap(const std::string& program, int resource_id);
// Pictures are made for the main window unless another renderer is set (classic mode's kit
// windows set their own while they draw). forget_renderer frees a closed window's pictures.
void set_target_renderer(SDL_Renderer* renderer);
void forget_renderer(SDL_Renderer* renderer);
// An 8-bit picture in the game's palette (palette entry 0 see-through), e.g. a kit's item
// picture, cached under a key. Rows run top to bottom.
const Bitmap* indexed_image(const std::string& key, int32_t width, int32_t height, const std::vector<uint8_t>& pixels);
// Draws part of a bitmap (all of it by default) at a screen position, scaled.
void draw_bitmap(ImDrawList* draw, const Bitmap* bitmap, ImVec2 pos, float scale, Rect2i part = {0, 0, 0, 0});

// A creature's face, from its head part looking at the camera.
bool creature_portrait(const Creature& creature, float height);
// The face scaled to fit inside a box (keeping its proportions) and centred in it.
bool draw_creature_face(ImDrawList* draw, const Creature& creature, ImVec2 min, ImVec2 max, ImU32 tint = IM_COL32_WHITE);
// The same from a sprite frame (a head looking at the camera), for a creature that isn't here.
bool draw_face(ImDrawList* draw, const ImageGallery& gallery, int32_t frame, ImVec2 min, ImVec2 max, ImU32 tint = IM_COL32_WHITE);

// A live view of part of the world, drawn into its own texture each frame.
class WorldView {
  public:
	// what to show next frame (world coordinates) and how big the image is
	void look_at(Rect2i world_rect, int32_t width, int32_t height);
	// draws the view; call while the world is drawn, before the main view
	void render(RenderSystem& system);
	// shows the last frame drawn as an ImGui image of the given size
	void image(ImVec2 size);
	// draws the last frame into a draw list, cropped to fill the box (a circle with rounding
	// half its size); returns false if nothing has been drawn yet
	bool draw(ImDrawList* draw, ImVec2 min, ImVec2 max, float rounding) const;
	bool active() const { return m_active; }

  private:
	struct State;
	State* m_state = nullptr;
	Rect2i m_rect;
	int32_t m_width = 0;
	int32_t m_height = 0;
	bool m_active = false;
};

// the porthole behind Creature's View
WorldView& creatures_view();

// Photographs: the next time the world is drawn, a picture of a part of it is taken and
// handed over as 8-bit pixels in the game's palette (rows top to bottom).
using PhotoCallback = void (*)(int32_t width, int32_t height, const std::vector<uint8_t>& pixels);
void request_photo(Rect2i world_rect, PhotoCallback callback);
// takes any photo asked for; call while drawing the world, before the main view
void take_requested_photo(RenderSystem& system);

} // namespace UIImages
