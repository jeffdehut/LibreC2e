#include "ImageManager.h"

#include "EngineContext.h"
#include "PathManager.h"
#include "common/Ascii.h"
#include "common/Exception.h"
#include "common/NumericCast.h"
#include "common/backend/Backend.h"
#include "common/io/FileReader.h"
#include "common/io/FileWriter.h"
#include "fileformats/ImageUtils.h"
#include "fileformats/paletteFile.h"
#include "fileformats/sprImage.h"

#include <algorithm>
#include <filesystem>
#include <fmt/core.h>

namespace fs = std::filesystem;


ImageManager::ImageManager() {
}

static ImageGallery build_gallery(const std::string& name, int32_t absolute_base, const std::vector<int32_t>& offsets, const MultiImage& images) {
	// stupid simple atlas-ing, can make this better

	int32_t total_width = 0;
	int32_t max_height = 0;
	for (const auto& img : images) {
		total_width += img.width;
		if (img.height > max_height) {
			max_height = img.height;
		}
	}

	ImageGallery gallery;
	gallery.name = name;
	gallery.absolute_base = absolute_base;
	gallery.offsets = offsets;
	gallery.texture = get_backend()->createTexture(total_width, max_height);

	int32_t current_x = 0;
	for (const auto& img : images) {
		Rect2i location{
			current_x,
			0,
			img.width,
			img.height};
		gallery.texture_locations.push_back(location);
		// the visible part: pixels that aren't palette entry 0, the see-through colour
		Rect2i bounds{0, 0, img.width, img.height};
		if (img.format == if_index8 && img.width > 0 && img.height > 0) {
			int32_t left = img.width, top = img.height, right = -1, bottom = -1;
			for (int32_t y = 0; y < img.height; ++y) {
				for (int32_t x = 0; x < img.width; ++x) {
					if (img.data[static_cast<size_t>(y * img.width + x)] != 0) {
						left = std::min(left, x);
						right = std::max(right, x);
						top = std::min(top, y);
						bottom = std::max(bottom, y);
					}
				}
			}
			if (right >= left) {
				bounds = Rect2i{left, top, right - left + 1, bottom - top + 1};
			}
		}
		gallery.content_bounds.push_back(bounds);
		if (img.width > 0 && img.height > 0) {
			// Creatures 1 has a few sprites with 0x0 size at end of files
			get_backend()->updateTexture(gallery.texture, location, img);
		}
		current_x += img.width;
	}
	gallery.source = std::make_shared<const MultiImage>(images);
	return gallery;
}

ImageGallery make_gallery(const std::string& name, int32_t absolute_base, const std::vector<int32_t>& offsets, const MultiImage& images) {
	return build_gallery(name, absolute_base, offsets, images);
}

void ImageManager::load_default_palette() {
	auto palette_dta_path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, "PALETTE.DTA");
	if (palette_dta_path.empty()) {
		throw Exception("Couldn't find PALETTE.DTA");
	}
	m_default_palette = ReadPaletteFile(palette_dta_path);
}

const ImageGallery& ImageManager::get_image(std::string name, int32_t absolute_base, int32_t image_count, ImageType allowed_types) {
	if (image_count <= 0) {
		throw Exception(fmt::format("ImageManager::get_image called with image_count=0 (name={:?})", name));
	}

	name = to_ascii_lowercase(name);

	// do we have it loaded already?
	ImageGalleryKey key{name, absolute_base, image_count};
	auto it = m_cache.find(key);
	if (it != m_cache.end()) {
		return it->second;
	}

	// nope, load it
	GalleryOrigin origin;
	origin.files = {name};
	origin.absolute_base = absolute_base;
	origin.image_count = image_count;
	origin.background = (allowed_types & IMAGE_IS_BACKGROUND) != 0;
	if (!(allowed_types & IMAGE_SPR)) {
		throw Exception(fmt::format("Couldn't find image {:?}", name));
	}
	std::vector<int32_t> offsets;
	auto images = load_images(origin, offsets);
	m_cache[key] = build_gallery(name, absolute_base, offsets, images);
	m_cache[key].origin = std::make_shared<const GalleryOrigin>(std::move(origin));
	return m_cache[key];
}

MultiImage ImageManager::load_images(const GalleryOrigin& origin, std::vector<int32_t>& offsets) {
	MultiImage images;
	offsets.clear();
	if (origin.image_count > 0) {
		// part of one file
		const std::string name = origin.files.empty() ? std::string() : origin.files.front();
		auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, name + ".spr");
		if (path.empty()) {
			throw Exception(fmt::format("Couldn't find image {:?}", name));
		}
		// TODO: faster to memory map file?
		FileReader in(path);
		auto data = ReadSprFileWithMetadata(in, origin.absolute_base, origin.image_count);
		for (auto& i : data.images) {
			if (i.format == if_index8 && !i.palette) {
				i.palette = m_default_palette;
			}
		}
		if (origin.background) {
			// ugly, stitch directly into a Texture?
			data.images = {ImageUtils::StitchBackground(data.images)};
		}
		offsets = data.offsets;
		return data.images;
	}
	for (const auto& source : origin.files) {
		auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, source + ".spr");
		if (path.empty()) {
			throw Exception(fmt::format("Couldn't find image {:?}", source));
		}
		FileReader in(path);
		for (auto& img : ReadSprFile(in)) {
			if (origin.remapped) {
				for (size_t pixel = 0; pixel < img.data.size(); ++pixel) img.data[pixel] = origin.remap[img.data[pixel]];
			}
			img.palette = m_default_palette;
			images.push_back(img);
		}
	}
	// header offsets as the original engine writes them: data directly after the header
	int32_t offset = numeric_cast<int32_t>(2 + 8 * images.size());
	for (const auto& img : images) {
		offsets.push_back(offset);
		offset += img.width * img.height;
	}
	return images;
}

bool ImageManager::has_spr_image(const std::string& name) {
	return !g_engine_context.paths->find_path(PATH_TYPE_IMAGE, to_ascii_lowercase(name) + ".spr").empty();
}

void ImageManager::add_joined_spr_image(std::string name, const std::vector<std::string>& sources, const std::string& file_name, const std::array<uint8_t, 256>* palette_remap) {
	name = to_ascii_lowercase(name);

	GalleryOrigin origin;
	origin.files = sources;
	if (palette_remap) {
		origin.remapped = true;
		origin.remap = *palette_remap;
	}
	std::vector<int32_t> offsets;
	MultiImage images = load_images(origin, offsets);

	ImageGalleryKey key{name, 0, numeric_cast<int32_t>(images.size())};
	m_cache[key] = build_gallery(name, 0, offsets, images);
	m_cache[key].origin = std::make_shared<const GalleryOrigin>(std::move(origin));

	if (!file_name.empty()) {
		auto out = g_engine_context.paths->create_file(PATH_TYPE_IMAGE, file_name + ".spr");
		// F083: canonical offsets point to the actual first image byte. The reader
        // still accepts the historical two-byte C1 gap in original sprite files.
        WriteSprFile(out, images, false);
		out.close();
	}
}

ImageGallery ImageManager::get_charset_dta(uint32_t bgcolor, uint32_t textcolor, uint32_t aliascolor) {
	// TODO: cache this?

	fs::path path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, "charset.dta");
	if (path.empty()) {
		throw Exception("Couldn't find charset.dta");
	}

	auto images = ImageUtils::ReadImage(path);
	for (auto& i : images) {
		if (i.format != if_index8) {
			throw Exception("Expected charset.dta to have format if_index8");
		}
		i.palette = m_default_palette;
		for (ptrdiff_t j = 0; j < numeric_cast<ptrdiff_t>(i.data.size()); ++j) {
			if (i.data[j] == 0) {
				i.data[j] = numeric_cast<uint8_t>(bgcolor);
			} else if (i.data[j] == 1) {
				i.data[j] = numeric_cast<uint8_t>(textcolor);
			} else if (i.data[j] > 1) {
				// Creatures 1 has some character pixels > 2 that just read colors
				// randomly from memory. Don't do that, just give them the aliascolor.
				i.data[j] = numeric_cast<uint8_t>(aliascolor);
			}
		}
	}

	return build_gallery("charset.dta", 0, std::vector<int32_t>{}, images);
}