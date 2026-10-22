#pragma once

#include <stdint.h>
#include <string>
#include <vector>

// The kits' shops (Clean Specs/BATCH4-ANSWERS.md, Q26): the Health Kit's Doctor's page sells
// the items in the file "Health" and the Breeder's Kit's Aphrodisiac page those in "Aphro",
// both in the main directory. Each item has a stock count, the script ADD sends to the game,
// a small picture, a name and a description. The Emergency Kit's "Replenish Stocks" copies
// "HealthBU" and "AphroBU" back over them.
//
// File layout (read from the shipped files): a 16-bit count, then for each item a 16-bit
// stock, a length-prefixed script, a 32-bit width, a 32-bit height, a 16-bit row length,
// width x height bytes of 8-bit picture, and the length-prefixed name and description.
namespace KitStock {

struct Item {
	uint16_t stock = 0;
	std::string script;
	int32_t width = 0;
	int32_t height = 0;
	uint16_t row_length = 0;
	std::vector<uint8_t> pixels;
	std::string name;
	std::string description;
};

std::vector<Item> load(const std::string& file);
void save(const std::string& file, const std::vector<Item>& items);

// Copies "<file>BU" over "<file>"; returns false if it couldn't.
bool replenish(const std::string& file);

} // namespace KitStock
