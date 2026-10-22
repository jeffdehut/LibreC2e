#pragma once

#include <string>
#include <filesystem>
#include "objects/ObjectHandle.h"

struct Creature;

// Exporting and importing creatures (Clean Specs/BATCH3-ANSWERS.md, Q28). An .exp file holds
// the creature record followed by its genome, and its child's genome if it's pregnant.

// The file name a creature is exported as, without ".exp": its name, or its moniker if it
// has no name.
std::string export_file_name(const Creature&);

// Writes the creature to an .exp file: <name>.exp in the main directory, or the given path.
// The original then removes the creature from the world (batch 6, Q12).
void export_creature(Creature&, bool remove_from_world, const std::string& path = {});

// Takes an exported creature out of the world, as the original's export does: it drops what
// it carries, its scripts stop, and it's deselected and removed.
void remove_exported_creature(Creature&);

// Reads an .exp file (a full path, or a name in the main directory) and puts the creature into
// the world with its ground point at (x, y). If a creature with its moniker is already in the
// world, it (and its unborn child) get new monikers. Its genomes are written to the Genetics
// folder. The original deletes the file afterwards and imports at (2591, 927) (batch 6, Q12);
// here deleting is optional.
// where the original puts an imported creature (room 7's floor in the shipped world)
constexpr float ORIGINAL_IMPORT_X = 2591, ORIGINAL_IMPORT_Y = 927;

Creature* import_creature(const std::string& file_name, float x, float y, bool delete_file = false);

// A drag offers a closed, private temporary export. Cancellation keeps the creature;
// acceptance removes it only from the world in which the drag began. Supplying a
// recovery directory retains the closed export after success, failure or cancellation.
class CreatureExportTransfer {
public:
	explicit CreatureExportTransfer(Creature&, const std::filesystem::path& recovery_directory = {});
	~CreatureExportTransfer();
	CreatureExportTransfer(const CreatureExportTransfer&) = delete;
	CreatureExportTransfer& operator=(const CreatureExportTransfer&) = delete;
	const std::filesystem::path& file() const { return m_file; }
	bool complete(bool accepted);
private:
	std::filesystem::path m_folder, m_file;
	ObjectHandle m_creature;
	std::string m_moniker;
	uint64_t m_generation;
	bool m_finished = false;
	bool m_keep_recovery = false;
};
