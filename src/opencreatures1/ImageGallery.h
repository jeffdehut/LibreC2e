#pragma once

#include "common/Image.h"
#include "common/NumericCast.h"
#include "common/backend/BackendTexture.h"
#include "common/math/Rect.h"

#include <array>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

// Where a gallery's pictures came from: sprite files in the game's Images folder (one, or
// several joined end to end as a creature's are, with its colours remapped), the part of the
// file used, and whether it's the background. A recording names these instead of keeping
// the pictures when they're the game's own (Recording.h).
struct GalleryOrigin {
	std::vector<std::string> files; // without .spr
	int32_t absolute_base = 0;
	int32_t image_count = 0; // 0 for all of each file (joined)
	bool background = false;
	bool remapped = false;
	std::array<uint8_t, 256> remap{};
};

class ImageGallery {
  public:
	std::string name;
	int32_t absolute_base = 0;
	std::vector<int32_t> offsets;

	// TODO:
	// should we pull out Texture and texture locations / atlas into their own, shared
	// and cached object? then we could load all sprites in the file once and share
	// it between all objects with similar galleries
	Texture texture;
	std::vector<Rect2i> texture_locations;
	// the part of each frame that isn't see-through (relative to the frame), for showing it in
	// the interface without its empty margins
	std::vector<Rect2i> content_bounds;
	// the pictures the texture was made from, kept for recordings (Recording.h)
	std::shared_ptr<const MultiImage> source;
	std::shared_ptr<const GalleryOrigin> origin; // null if not made from sprite files

	explicit operator bool() const {
		return !name.empty() || texture || !texture_locations.empty();
	}

	int32_t size() const {
		return numeric_cast<int32_t>(texture_locations.size());
	}
	// A script can set a pose past the end of an object's images; such a frame has no size
	// and isn't drawn, rather than reading past the end of the list.
	bool has_frame(int32_t frame) const {
		return frame >= 0 && frame < size();
	}
	int32_t width(int32_t frame) const {
		return has_frame(frame) ? texture_locations[static_cast<size_t>(frame)].width : 0;
	}
	int32_t height(int32_t frame) const {
		return has_frame(frame) ? texture_locations[static_cast<size_t>(frame)].height : 0;
	}
	// bool transparentAt(unsigned int frame, unsigned int x, unsigned int y) const;
};