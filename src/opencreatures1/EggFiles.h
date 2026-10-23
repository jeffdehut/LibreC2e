#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class Object;

// Eggs as files (not in the original), in LibreCreatures' .egg format
// (third_party/librecreatures/C1EggFile.h), so eggs can be traded with it. And each world's
// archive of every egg that appears in it, filed by generation:
//   Worlds/<world>/Eggs/Generation <n>/<moniker> (<mother> and <father>).egg
// with Eggs/eggs.txt listing them, so any Norn's egg can be found again, living or dead.
// An egg whose parents aren't known goes in "Generation unknown" (laid before the archive
// began), or "Imported" (from a file).
namespace EggFiles {

// Whether a file is named as an egg (.egg).
bool is_egg_file(const std::filesystem::path& path);

// A genome's egg as a file's bytes, for an egg that hatches as sex (1 male, 2 female, 0 either);
// empty if there's no such genome in the Genetics folder.
std::vector<uint8_t> egg_bytes(const std::string& moniker, uint32_t sex);
// Saves a copy of a creature's egg: the one archived when it was laid if there is one, otherwise
// one made from its genome. Throws if there's neither.
void save_copy(const std::string& moniker, uint32_t sex, const std::filesystem::path& to);

// The egg the hand is carrying, or nullptr.
Object* held_egg();
// Saves the held egg to a file and takes it out of the world, as exporting a creature does.
// Throws if the hand holds no egg or the file can't be written.
void export_held_egg(const std::filesystem::path& to);
// Puts an egg from a file into the world: a fresh egg by the incubator, as the Hatchery leaves
// them, which the camera moves to. Its genome keeps its name unless something in this world
// already uses it. The file is used up (removed) unless it's in a world's own Eggs archive.
// Returns the baby's moniker; throws if the file isn't an egg.
std::string import_egg(const std::filesystem::path& from);

// How the archive learns a Norn's generation and name (the family registry's): a generation of
// 0 and an empty name for one it doesn't know.
void set_lookups(std::function<int(const std::string&)> generation, std::function<std::string(const std::string&)> name);
// A Norn's generation, from the lookup or the archive (0 if unknown).
int generation(const std::string& moniker);
// The archived copy of a creature's egg, or an empty path.
std::filesystem::path archived(const std::string& moniker);

// Once a tick: archives any egg that has appeared in the world.
void tick();
// After a world is loaded.
void world_changed();

} // namespace EggFiles
