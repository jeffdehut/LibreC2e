#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct GraveRecord {
	std::string moniker, name, sex, age, headstone;
	int32_t photo = -1;
	std::string parents, died, born;
};
std::vector<GraveRecord> load_graveyard(const std::filesystem::path&);
void save_graveyard(const std::filesystem::path&, const std::vector<GraveRecord>&);
