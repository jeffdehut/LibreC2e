#pragma once
#include <filesystem>
#include <string>

namespace session {
namespace fs = std::filesystem;
enum class Game { c1, c2 };
const char* name(Game);
// Validates the leading MFC MapData record only, not the complete archive.
Game world_game(const fs::path&);
struct Selection {
	Game game;
	fs::path installation;
	fs::path world;
};
Selection inspect(const fs::path& installation, const fs::path& world = {}, const std::string& requested = "auto");
struct Copy {
	fs::path root, data, profile, world;
};
// Copies the installation once per canonical installation/world identity. Never overwrites
// a prior session or writes into the source installation. Symlinks are refused.
Copy prepare(const Selection&, const fs::path& sessions);
}
