#pragma once

#include "common/Color.h"
#include "common/shared_array.h"
#include "opencreatures1/ImageGallery.h"

#include <array>
#include <map>
#include <stdint.h>
#include <string>
#include <vector>


class ImageManager {
  public:
	enum ImageType {
		IMAGE_IS_BACKGROUND = 1,
		IMAGE_SPR = 2,
	};

	ImageManager();
	void load_default_palette();
	const shared_array<Color>& default_palette() const { return m_default_palette; }
	const ImageGallery& get_image(std::string name, int32_t absolute_base, int32_t image_count, ImageType allowed_types);
	bool has_spr_image(const std::string& name);
	// Makes `name` available to get_image() as the given sprite files joined end to end,
	// the way Creatures 1 builds a creature's sprite file from its body parts.
	// Joins sprite files into one gallery called name. If file_name isn't empty, the joined
	// images are also written to Images/<file_name>.spr in the layout the original uses for
	// creatures' sprite files.
	void add_joined_spr_image(std::string name, const std::vector<std::string>& sources, const std::string& file_name = "", const std::array<uint8_t, 256>* palette_remap = nullptr);
	ImageGallery get_charset_dta(uint32_t bgcolor, uint32_t textcolor, uint32_t aliascolor);

  private:
	struct ImageGalleryKey {
		std::string name;
		int32_t absolute_base;
		int32_t image_count;

		bool operator<(const ImageGalleryKey& other) const {
			// stupid std::map and STL
			if (name != other.name)
				return name < other.name;
			if (absolute_base != other.absolute_base)
				return absolute_base < other.absolute_base;
			return image_count < other.image_count;
		}
	};
	std::map<ImageGalleryKey, ImageGallery> m_cache;
	shared_array<Color> m_default_palette;
};