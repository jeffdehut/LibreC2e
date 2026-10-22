#pragma once

#include <filesystem>
#include <map>
#include <string>

// Native kit settings live in the selected session profile, never in machine-wide
// registry keys. Loading a new profile replaces, rather than merges, its settings.
class KitPreferences {
public:
	void load(const std::filesystem::path& file);
	int get(const std::string& key, int fallback) const;
	void set(const std::string& key, int value);
private:
	std::filesystem::path path;
	std::map<std::string, int> values;
};
