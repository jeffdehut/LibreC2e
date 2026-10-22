// Source-informed adaptation from h4rm0n1c/LibreCreatures, GNU GPL version 3.
// Pin 907b8642f79c2688bd8a1c3f43e401a5f9431c8c:
// src/c1/creatures/learned_words.cpp and creature.cpp. See NOTICE.md and LICENSE.
#pragma once

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace openc1::librecreatures {
inline constexpr size_t word_capacity = 24;

inline std::string normalize_word_response(std::string_view input) {
	std::string response(input.substr(0, word_capacity));
	static constexpr std::pair<std::string_view, std::string_view> substitutions[] = {
		{"ph", "f"}, {"ss", "s"}, {"sh", "th"}, {"s", "th"}, {"th", "d"},
		{"j", "d"}, {"r", "w"}, {"ion", "un"}, {"in", "im"}, {"nn", "n"},
		{"rr", "r"}, {"ea", "ee"}, {"ai", "ay"}, {"v", "w"}, {"ck", "k"},
		{"ng", "nk"}, {"oa", "ow"}, {"or", "aw"}};
	for (auto [from, to] : substitutions) {
		const auto at = response.find(from);
		if (at != std::string::npos) {
			response.replace(at, from.size(), to);
			response.resize(std::min(response.size(), word_capacity));
			break; // only the first matching table entry, once
		}
	}
	return response;
}

inline std::string regenerate_word_response(std::string_view input, std::string_view previous) {
	const auto heard = input.substr(0, word_capacity);
	if (heard.empty() || previous.empty()) return normalize_word_response(heard);
	constexpr std::string_view vowels = "aeiouAEIOU";
	constexpr std::string_view consonants = "bcdfghjklmnpqrstvwxyzBCDFGHJKLMNPQRSTVWXYZ";
	const bool starts_with_vowel = vowels.find(heard.front()) != std::string_view::npos;
	size_t end = 0;
	while (end < heard.size() &&
		(vowels.find(heard[end]) != std::string_view::npos) == starts_with_vowel) ++end;
	// Include the first opposite-class letter, but never scan past the word (F080).
	const auto opposite = starts_with_vowel ? consonants : vowels;
	if (end < heard.size() && opposite.find(heard[end]) != std::string_view::npos) ++end;
	std::string response(1, previous.front());
	if (end > 1) response.append(heard.substr(1, end - 1));
	if (response.size() < 3 || (heard.size() > 5 && response.size() < 5)) {
		const auto suffix = response.substr(0, 9);
		response += suffix;
	}
	response.resize(std::min(response.size(), word_capacity));
	return response;
}

// The first partition belongs to Norns; other C1 genera use the second partition.
inline constexpr std::array<std::array<const char*, 5>, 8> newborn_word_banks{{
	{{"goo", "dis", "gaa", "ba", "em"}},
	{{"foo", "dat", "oogh", "coo", "me"}},
	{{"flib", "bab", "bibble", "flub", "eem"}},
	{{"doo", "daa", "boop", "na", "bub"}},
	{{"grah", "narg", "graaaah", "grish", "grndl"}},
	{{"beh", "buh", "bleh", "bih", "beh"}},
	{{"urg", "arg", "oogle", "nug", "ung"}},
	{{"urrh", "bilp", "narg", "flabber", "fig"}}
}};
} // namespace openc1::librecreatures
