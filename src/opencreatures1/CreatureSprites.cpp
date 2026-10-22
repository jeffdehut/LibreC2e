#include "CreatureSprites.h"

#include "EngineContext.h"
#include "PathManager.h"
#include "common/Exception.h"
#include "common/io/FileReader.h"
#include "common/endianlove.h"
#include "fileformats/sfc/CGallery.h"
#include "fileformats/sfc/Creature.h"

#include <array>
#include <fmt/core.h>
#include <map>
#include <utility>

namespace {

constexpr std::array<char, 12> PART_LETTERS = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L'};

CreatureBodyRegion region_for_part(char part) {
	switch (part) {
		case 'A': return CreatureBodyRegion::HEAD;
		case 'B': return CreatureBodyRegion::BODY;
		case 'C':
		case 'D':
		case 'E':
		case 'F':
		case 'G':
		case 'H': return CreatureBodyRegion::LEGS;
		default: return CreatureBodyRegion::ARMS;
	}
}

using ImageSizes = std::vector<std::pair<int32_t, int32_t>>;

// Reads just the header of a sprite file. Empty if the file doesn't exist.
const ImageSizes& sprite_sizes(const std::string& name) {
	static std::map<std::string, ImageSizes> cache;
    static uint64_t generation = 0;
    if (generation != g_engine_context.generation()) { cache.clear(); generation = g_engine_context.generation(); }
	auto it = cache.find(name);
	if (it != cache.end()) {
		return it->second;
	}
	ImageSizes sizes;
	auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, name + ".spr");
	if (!path.empty()) {
		FileReader in(path);
		uint16_t count = read16le(in);
		for (uint16_t i = 0; i < count; ++i) {
			read32le(in); // offset
			int32_t width = read16le(in);
			int32_t height = read16le(in);
			sizes.emplace_back(width, height);
		}
	}
	return cache[name] = sizes;
}

int sex_digit_for(const sfc::CreatureV1& creature) {
	// Male Norns use 0 and female Norns use 4, confirmed against saved creatures.
	// Assumption, not yet confirmed by a saved creature: Grendels (genus 2) use 1 and 5,
	// matching the "100" and "500" sprites shipped with the game.
	int digit = creature.sex == 2 ? 4 : 0;
	if (creature.genus == 2) {
		digit += 1;
	}
	return digit;
}

} // namespace

std::string creature_body_part_sprite(char part, int sex_digit, int age, int breed) {
	for (int a = age; a >= 0; --a) {
		auto name = fmt::format("{}{}{}{}", part, sex_digit, a, breed);
		if (!sprite_sizes(name).empty()) {
			return name;
		}
	}
	return {};
}

std::vector<std::string> creature_body_part_sprites_for_saved_gallery(const sfc::CreatureV1& creature) {
	if (!creature.gallery) {
		throw Exception(fmt::format("Creature {:?} has no gallery", creature.moniker));
	}
	const auto& saved = creature.gallery->images;
	const int sex_digit = sex_digit_for(creature);

	// The parts of each body region are next to each other in the gallery, so walk through
	// it one region at a time, picking the first breed whose sprites match the saved sizes.
	std::vector<std::string> names;
	size_t pos = 0;
	size_t i = 0;
	while (i < PART_LETTERS.size()) {
		const auto region = region_for_part(PART_LETTERS[i]);
		size_t region_end = i;
		while (region_end < PART_LETTERS.size() && region_for_part(PART_LETTERS[region_end]) == region) {
			region_end++;
		}

		bool found = false;
		for (int breed = 0; breed <= 9 && !found; ++breed) {
			std::vector<std::string> region_names;
			size_t region_pos = pos;
			bool matches = true;
			for (size_t j = i; j < region_end && matches; ++j) {
				auto name = creature_body_part_sprite(PART_LETTERS[j], sex_digit, creature.age, breed);
				const auto& sizes = sprite_sizes(name);
				matches = !name.empty() && region_pos + sizes.size() <= saved.size();
				for (size_t k = 0; k < sizes.size() && matches; ++k) {
					matches = saved[region_pos + k].width == sizes[k].first && saved[region_pos + k].height == sizes[k].second;
				}
				region_names.push_back(name);
				region_pos += sizes.size();
			}
			if (matches) {
				names.insert(names.end(), region_names.begin(), region_names.end());
				pos = region_pos;
				found = true;
			}
		}
		if (!found) {
			throw Exception(fmt::format("Couldn't find body-part sprites matching the saved gallery of creature {:?} (body region {})",
				creature.moniker, static_cast<int>(region)));
		}
		i = region_end;
	}
	if (pos != saved.size()) {
		throw Exception(fmt::format("Body-part sprites for creature {:?} have {} images, but its saved gallery has {}",
			creature.moniker, pos, saved.size()));
	}
	return names;
}
