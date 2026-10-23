#pragma once

#include "common/backend/BackendTexture.h"
#include "common/math/Rect.h"
#include "common/render/RenderItem.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class ImageGallery;

// Higher-resolution art for the modern layout (not in the original): LibreCreatures'
// NAME@2x.s32 files, a PNG for each frame of NAME.spr at twice its size (the format is in
// HdArt.cpp). Each is drawn in its 1x frame's place, so everything lines up as it always did.
// A file is used for a gallery only if it has each of the gallery's frames at exactly twice
// the size, as LibreCreatures requires; otherwise, and until a frame has been decoded (a few
// each frame, so nothing stalls), the 1x frame is drawn. The art is the player's own: it's
// read from their folders, never shipped.
namespace HdArt {

struct Frame {
	Texture texture;
	Rect2i src;
};

// Where to look for NAME@2x.s32 files, earliest first (a name found in two places uses the first).
void set_folders(const std::vector<std::filesystem::path>& folders);
// Makes a texture from a frame's PNG, which decodes to width x height; an empty texture if it
// can't. The program sets it (decoding needs its renderer).
void set_loader(std::function<Texture(const std::vector<uint8_t>& png, int32_t width, int32_t height)> loader);
// On in the modern layout; the interface sets it each frame.
void set_enabled(bool on);
bool enabled();
// How many NAME@2x.s32 files were found.
size_t file_count();

// The higher-resolution frame for one of a gallery's frames, or nullptr: draw the 1x frame.
const Frame* frame(const ImageGallery& gallery, int32_t index);
// What to draw for one of a gallery's frames at a world position: its higher-resolution frame,
// sized to the 1x frame, or the 1x frame itself.
RenderItem render_item(const ImageGallery& gallery, int32_t index, float x, float y, int32_t z);
// The background's tile (back.spr's frame column * 8 + row, each 144 x 150), or nullptr.
const Frame* background_tile(int32_t index);
// Decodes frames asked for, for up to budget seconds. Call once a frame.
void update(double budget_seconds = 0.006);

} // namespace HdArt
