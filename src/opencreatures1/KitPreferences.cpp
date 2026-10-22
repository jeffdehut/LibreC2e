#include "KitPreferences.h"
#include "PersistentFile.h"
#include <fstream>
#include <sstream>
#include <system_error>

void KitPreferences::load(const std::filesystem::path& file) {
	path = file;
	values.clear();
	std::ifstream in(path);
	std::string line, key;
	int value;
	while (std::getline(in, line)) {
		std::istringstream row(line);
		if (row >> key >> value) values[key] = value;
	}
}

int KitPreferences::get(const std::string& key, int fallback) const {
	auto it = values.find(key);
	return it == values.end() ? fallback : it->second;
}

void KitPreferences::set(const std::string& key, int value) {
	auto it = values.find(key);
	if (it != values.end() && it->second == value) return;
	auto updated = values;
	updated[key] = value;
	std::ostringstream out;
	for (const auto& entry : updated) out << entry.first << ' ' << entry.second << '\n';
	write_persistent_file(path, out.str());
    values = std::move(updated);
}
