#include "Session.h"
#include "common/case_insensitive_filesystem.h"
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <cstdint>

namespace session {
const char* name(Game g) { return g == Game::c1 ? "c1" : "c2"; }
static void require(bool ok, const std::string& message) {
	if (!ok) throw std::runtime_error(message);
}
static fs::path lookup(const fs::path& p) {
	std::error_code ec;
	auto result = case_insensitive_filesystem::canonical(p, ec);
	return ec ? fs::path{} : result;
}
Game world_game(const fs::path& path) {
	std::ifstream in(path, std::ios::binary);
	std::array<unsigned char, 17> bytes{};
	in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
	require(in.gcount() == static_cast<std::streamsize>(bytes.size()), "Missing or truncated world: " + path.string());
	require(bytes[0] == 255 && bytes[1] == 255 && bytes[4] == 7 && bytes[5] == 0 &&
		std::string(reinterpret_cast<char*>(bytes.data() + 6), 7) == "MapData", "Unsupported world: expected an SFC MapData archive: " + path.string());
	require(bytes[3] == 0 && bytes[14] == 0 && bytes[15] == 0 && bytes[16] == 0 &&
		((bytes[2] == 1 && bytes[13] == 0) || (bytes[2] == 2 && bytes[13] == 1)),
		"Unsupported SFC schema or MapData version: " + path.string());
	return bytes[13] == 0 ? Game::c1 : Game::c2;
}
Selection inspect(const fs::path& installation, const fs::path& world, const std::string& requested) {
	require(requested == "auto" || requested == "c1" || requested == "c2", "Game must be auto, c1 or c2");
	require(fs::is_directory(installation), "Installation folder does not exist: " + installation.string());
	auto dir = fs::canonical(installation);
	bool c1 = fs::is_regular_file(lookup(dir / "Creatures.exe"));
	bool c2 = fs::is_regular_file(lookup(dir / "Creatures2.exe"));
	require(c1 != c2, "Installation must contain exactly one of Creatures.exe or Creatures2.exe");
	Game game = c1 ? Game::c1 : Game::c2;
	require(requested == "auto" || requested == name(game), "Explicit game selection disagrees with installation metadata");
	for (auto part : {"Images", "Body Data", "Genetics", "Sounds"})
		require(fs::is_directory(lookup(dir / part)), std::string("Missing data directory: ") + part);
	fs::path chosen;
	if (!world.empty()) {
		chosen = lookup(world.is_absolute() ? world : dir / world);
	} else {
		for (auto file : {"World.openc1.sfc", "World.sfc", "Eden.sfc"}) {
			if (game == Game::c2 && std::string(file) == "World.openc1.sfc") continue;
			chosen = lookup(dir / file);
			if (!chosen.empty()) break;
		}
	}
	require(!chosen.empty() && fs::is_regular_file(chosen), "No supported world found in installation");
	require(chosen.parent_path() == dir, "Select a world directly inside its installation folder");
	require(world_game(chosen) == game, "World format disagrees with installation game");
	return {game, dir, chosen};
}
static std::string identity(const Selection& s) {
	std::ostringstream out;
	out << "openc2e-session-v1\n" << name(s.game) << '\n' << std::quoted(s.installation.generic_string()) << '\n' << std::quoted(s.world.generic_string()) << '\n';
	return out.str();
}
static bool within(const fs::path& child, const fs::path& parent) {
	auto a = child.begin();
	for (auto b = parent.begin(); b != parent.end(); ++b, ++a)
		if (a == child.end() || *a != *b) return false;
	return true;
}
Copy prepare(const Selection& s, const fs::path& sessions) {
	auto base = fs::weakly_canonical(fs::absolute(sessions));
	require(!within(base, s.installation), "Session directory must be outside the source installation");
	const auto id = identity(s);
	uint64_t hash = 14695981039346656037ull;
	for (unsigned char c : id) { hash ^= c; hash *= 1099511628211ull; }
	std::ostringstream key;
	key << name(s.game) << '-' << std::hex << hash;
	Copy copy;
	copy.root = base / key.str();
	copy.data = copy.root / "data";
	copy.profile = copy.root / "profile";
	copy.world = copy.data / s.world.filename();
	if (fs::exists(copy.root)) {
		std::ifstream in(copy.root / "source.txt");
		std::string recorded((std::istreambuf_iterator<char>(in)), {});
		require(recorded == id, "Session identity mismatch; existing data left untouched: " + copy.root.string());
		require(fs::is_directory(copy.data) && fs::is_directory(copy.profile) && fs::is_regular_file(copy.world), "Incomplete session; existing data left untouched: " + copy.root.string());
		return copy;
	}
	fs::create_directories(base);
	auto staging = copy.root;
	staging += ".preparing";
	require(fs::create_directory(staging), "Session preparation already exists: " + staging.string());
	try {
		// Never follow links into another installation, user saves or a bottle.
		for (const auto& entry : fs::recursive_directory_iterator(s.installation))
			require(!entry.is_symlink(), "Installation contains a symbolic link; copy data without links first: " + entry.path().string());
		fs::copy(s.installation, staging / "data", fs::copy_options::recursive);
		// Originals may be on read-only media; only the copied files become writable.
		fs::permissions(staging / "data", fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::add);
		for (const auto& entry : fs::recursive_directory_iterator(staging / "data"))
			fs::permissions(entry.path(), fs::perms::owner_write | (entry.is_directory() ? fs::perms::owner_exec : fs::perms::none), fs::perm_options::add);
		fs::create_directory(staging / "profile");
		std::ofstream manifest(staging / "source.txt");
		manifest.exceptions(std::ios::failbit | std::ios::badbit);
		manifest << id;
		manifest.close();
		fs::rename(staging, copy.root);
	} catch (...) {
		fs::remove_all(staging);
		throw;
	}
	return copy;
}
}
