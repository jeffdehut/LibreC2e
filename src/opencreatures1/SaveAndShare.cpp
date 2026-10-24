#include "SaveAndShare.h"

#include "EngineContext.h"
#include "MacroManager.h"
#include "CreatureGenetics.h"
#include "CreatureSprites.h"
#include "ImageManager.h"
#include "fileformats/sfc/Body.h"
#include "fileformats/sfc/Limb.h"
#include "fileformats/sfc/CGallery.h"
#include "common/Ascii.h"
#include "common/io/FileReader.h"
#include "common/io/SpanReader.h"
#include "fileformats/genomeFile.h"
#include "PathManager.h"
#include "SFCSerialization.h"
#include "WorldState.h"
#include "common/Exception.h"
#include "common/io/FileWriter.h"
#include "fileformats/sfc/CGenome.h"
#include "fileformats/sfc/Creature.h"
#include "fileformats/sfc/EXPFile.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <fmt/core.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>

namespace {

std::shared_ptr<sfc::CGenomeV1> read_genome_record(const std::string& moniker, uint32_t sex, uint8_t life_stage) {
	auto path = g_engine_context.paths->find_path(PATH_TYPE_GENETICS, moniker + ".gen");
	if (path.empty()) {
		return nullptr;
	}
	FileReader in(path);
	auto genome = std::make_shared<sfc::CGenomeV1>();
	genome->moniker = moniker;
	genome->sex = sex;
	genome->life_stage = life_stage;
	genome->data = in.read_to_end();
	return genome;
}

void write_genome_record(const sfc::CGenomeV1& genome) {
	if (genome.moniker.empty() || !g_engine_context.paths->find_path(PATH_TYPE_GENETICS, genome.moniker + ".gen").empty()) {
		return;
	}
	auto out = g_engine_context.paths->create_file(PATH_TYPE_GENETICS, genome.moniker + ".gen");
	out.write(genome.data.data(), genome.data.size());
	out.close(); // Confirm success before creating a creature or consuming its source file.
}

void validate_moniker(const std::string& moniker) {
	if (moniker.size() != 4 || !std::all_of(moniker.begin(), moniker.end(), [](char c) { return is_ascii_alnum(c) || c == '_'; })) {
		throw Exception("The export has an invalid C1 genetic identity");
	}
}

void validate_genome(const sfc::CGenomeV1& record) {
	validate_moniker(record.moniker);
	SpanReader in(record.data);
	genomeFile parsed;
	in >> parsed;
	if (parsed.getVersion() != 1) throw Exception("This export needs a Creatures 1 genome");
}

bool conflicting_genome(const sfc::CGenomeV1& genome) {
	auto existing = read_genome_record(genome.moniker, genome.sex, genome.life_stage);
	return existing && existing->data != genome.data;
}

} // namespace

std::string export_file_name(const Creature& c) {
	std::string name;
	for (char ch : c.history_name) {
		if (std::isalnum(static_cast<unsigned char>(ch)) || ch == ' ' || ch == '-' || ch == '_') {
			name += ch;
		}
	}
	if (name.empty() || c.history_name == "<Un Named>") {
		name = c.moniker;
	}
	return name;
}

void export_creature(Creature& c, bool remove_from_world, const std::string& path) {
	sfc::EXPFile exp;
	exp.creature = sfc_dump_creature(c);
	if (!exp.creature) {
		throw Exception("couldn't save the creature record");
	}
	exp.genome = read_genome_record(c.moniker, c.sex, static_cast<uint8_t>(c.age));
	if (!exp.genome) {
		throw Exception(fmt::format("couldn't find the genome {}.gen", c.moniker));
	}
	if (!c.zygote.empty()) {
		exp.child_genome = read_genome_record(c.zygote, 0, 0);
		if (!exp.child_genome) {
			throw Exception(fmt::format("couldn't find the unborn child's genome {}.gen", c.zygote));
		}
	}
	if (path.empty()) {
		auto out = g_engine_context.paths->create_file(PATH_TYPE_MAIN, export_file_name(c) + ".exp");
		sfc::write_exp_v1_file(out, exp);
		out.close(); // F048: confirm durability before removing the in-world creature.
	} else {
		FileWriter out(path);
		sfc::write_exp_v1_file(out, exp);
		out.close(); // F048: confirm durability before removing the in-world creature.
	}
	fmt::print("INFO [Export] Exported {} to {}\n", c.moniker, path.empty() ? export_file_name(c) + ".exp" : path);

	if (remove_from_world) {
		remove_exported_creature(c);
	}
}

void remove_exported_creature(Creature& c) {
	{
		c.drop_carried_objects();
		g_engine_context.macros->delete_macros_owned_by(c.uid);
		if (g_engine_context.world->current_norn == c.uid) {
			g_engine_context.world->current_norn = {};
		}
		c.asleep = 0;
		c.update_zzz();
		g_engine_context.objects->destroy_later(c.uid);
	}
}

Creature* import_creature(const std::string& file_name, float x, float y, bool delete_file) {
	std::error_code exists_err;
	std::filesystem::path path = std::filesystem::is_regular_file(file_name, exists_err) ? std::filesystem::path(file_name) : g_engine_context.paths->find_path(PATH_TYPE_MAIN, file_name);
	if (path.empty()) {
		throw Exception(fmt::format("couldn't find {}", file_name));
	}
	auto exp = sfc::read_exp_v1_file(path.string());
	if (!exp.creature) {
		throw Exception("the file has no creature in it");
	}
	if (!exp.creature->body || !exp.creature->brain || !exp.creature->biochemistry) {
		throw Exception("The export is missing its body, brain or biochemistry");
	}
	validate_moniker(exp.creature->moniker);
	// Some old exports omit the embedded genome. Accept these only if this
	// installation already contains the required C1 genome.
	if (!exp.genome) exp.genome = read_genome_record(exp.creature->moniker, exp.creature->sex, exp.creature->age);
	if (!exp.genome) throw Exception("The export has no genome and none is installed locally");
	if (exp.genome->moniker != exp.creature->moniker) throw Exception("The export's creature and genome identities disagree");
	validate_genome(*exp.genome);
	if (!exp.creature->zygote.empty()) {
		validate_moniker(exp.creature->zygote);
		if (exp.creature->zygote == exp.creature->moniker) throw Exception("The unborn child has its mother's identity");
		if (!exp.child_genome) exp.child_genome = read_genome_record(exp.creature->zygote, 0, 0);
		if (!exp.child_genome || exp.child_genome->moniker != exp.creature->zygote) throw Exception("The unborn child's genome is missing or has the wrong identity");
		validate_genome(*exp.child_genome);
	}
	// Include pregnancies and laid eggs, as well as creatures waiting for deletion.
	// Stored genomes with different contents must also retain their own identity.
	auto in_world = [](const std::string& moniker) {
		for (auto* obj : *g_engine_context.objects) {
			if (auto* other = obj->as_creature(); other && (other->moniker == moniker || other->zygote == moniker)) return true;
			if (obj->family == 2 && obj->genus == 5 && obj->species == 2 && obj->obv0 == moniker_to_integer(moniker)) return true;
		}
		return false;
	};
	auto unused_moniker = [&] {
		for (int attempt = 0; attempt < 1000; ++attempt) {
			auto candidate = new_moniker();
			if (candidate != exp.creature->moniker && candidate != exp.creature->zygote && !in_world(candidate)) return candidate;
		}
		throw Exception("Couldn't allocate an unused imported identity");
	};
	if (in_world(exp.creature->moniker) || conflicting_genome(*exp.genome)) {
		const std::string old = exp.creature->moniker;
		const std::string renamed = unused_moniker();
		exp.creature->moniker = renamed;
		if (register_moniker(exp.creature->history_moniker) == old) exp.creature->history_moniker = register_moniker_text(renamed);
		// F049: an active gamete must refer to this imported genome when mating.
		if (!exp.creature->gamete.empty()) exp.creature->gamete = renamed;
		exp.genome->moniker = renamed;
		fmt::print("INFO [Import] {} is already in use, so the import is {}\n", old, renamed);
	}
	if (exp.child_genome && !exp.creature->zygote.empty() && (in_world(exp.creature->zygote) || conflicting_genome(*exp.child_genome))) {
		exp.creature->zygote = unused_moniker();
		exp.child_genome->moniker = exp.creature->zygote;
	}
	// Rebuild the saved body layout under the imported identity, including after
	// a clash. Otherwise two different creatures can share one cached sprite file.
	const auto parts = creature_body_part_sprites_for_saved_gallery(*exp.creature);
	if (!exp.creature->body->gallery) throw Exception("The export has no body gallery");
	exp.creature->body->gallery->filename = exp.creature->moniker;
	if (exp.creature->gallery) exp.creature->gallery->filename = exp.creature->moniker;
	std::set<const sfc::LimbV1*> seen;
	for (auto limb : {exp.creature->head, exp.creature->left_thigh, exp.creature->right_thigh, exp.creature->left_arm, exp.creature->right_arm, exp.creature->tail}) {
		for (; limb; limb = limb->next) {
			if (!seen.insert(limb.get()).second || !limb->gallery) throw Exception("The export has an invalid limb chain");
			limb->gallery->filename = exp.creature->moniker;
		}
	}
	g_engine_context.images->add_joined_spr_image(exp.creature->moniker, parts, exp.creature->moniker);
	if (exp.genome) {
		write_genome_record(*exp.genome);
	}
	if (exp.child_genome) {
		write_genome_record(*exp.child_genome);
	}
	// nothing outside the creature comes with it
	exp.creature->carrier = nullptr;
	exp.creature->objp = nullptr;
	exp.creature->zzzz = nullptr;
	auto handle = sfc_load_creature(exp.creature.get());
	auto* c = g_engine_context.objects->try_get(handle)->as_creature();
	seed_food_memory(*c);
	c->movement_status = MOVEMENT_AUTONOMOUS;
	c->move_ground_to(x, y);
	c->land();
	fmt::print("INFO [Import] Imported {} from {}\n", c->moniker, file_name);
	if (delete_file) {
		std::error_code err;
		std::filesystem::remove(path, err);
	}
	return c;
}

CreatureExportTransfer::CreatureExportTransfer(Creature& creature, const std::filesystem::path& recovery_directory)
	: m_creature(creature.uid), m_moniker(creature.moniker), m_generation(g_engine_context.generation()) {
	m_keep_recovery = !recovery_directory.empty();
	const auto parent = m_keep_recovery ? recovery_directory : std::filesystem::temp_directory_path();
	if (m_keep_recovery) std::filesystem::create_directories(parent);
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	for (int attempt = 0; attempt < 100; ++attempt) {
		auto folder = parent / fmt::format("opencreatures1-export-{}-{}", stamp, attempt);
		if (std::filesystem::create_directory(folder)) { m_folder = folder; break; }
	}
	if (m_folder.empty()) throw Exception("Couldn't create a private export folder");
	m_file = m_folder / (export_file_name(creature) + ".exp");
	try {
		export_creature(creature, false, m_file.string());
	} catch (...) {
		std::error_code error;
		std::filesystem::remove_all(m_folder, error);
		throw;
	}
}

CreatureExportTransfer::~CreatureExportTransfer() {
	if (m_keep_recovery) return;
	std::error_code error;
	std::filesystem::remove_all(m_folder, error);
}

bool CreatureExportTransfer::complete(bool accepted) {
	if (m_finished) return false;
	m_finished = true;
	if (!accepted || m_generation != g_engine_context.generation()) return false;
	auto* object = g_engine_context.objects->try_get(m_creature);
	auto* creature = object ? object->as_creature() : nullptr;
	if (!creature || creature->moniker != m_moniker) return false;
	remove_exported_creature(*creature);
	return true;
}
