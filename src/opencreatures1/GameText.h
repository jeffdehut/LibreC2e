#pragma once

#include <cstdint>
#include <string>

// The original game's wording for a line of its interface, read from the player's own copy
// (the text inside its programs), so the classic layout can say exactly what the original
// said without this project containing a word of it. Each line is known only by its length
// and a rolling hash of its characters; if the player's programs don't have it, our own
// wording is used instead.
namespace GameText {

struct Original {
	uint16_t length;
	uint64_t hash;
};

// the original line if the game's programs have it (without surrounding spaces), else ours
std::string line(Original original, const char* ours);

// the rolling hash the fingerprints use: h = h * 1000003 + c over the characters
uint64_t fingerprint(const std::string& text);

} // namespace GameText
