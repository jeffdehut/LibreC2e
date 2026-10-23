#include "Registration.h"
#include "EngineContext.h"
#include "PathManager.h"
#include "PersistentFile.h"
#include "WorldSystems.h"
#include "objects/Creature.h"
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
std::filesystem::path path(const Creature& c) {
	return current_world_folder() / (get_world_file_name() + "." + c.moniker + ".registration");
}
auto fields(Creature& c) {
	return std::array<std::string*, 7>{&c.history_name, &c.history_birthday,
		&c.history_birthplace, &c.history_owner_name, &c.history_owner_phone,
		&c.history_owner_address, &c.history_owner_email};
}
}

void load_registration(Creature& c) {
	const auto file = path(c);
	std::ifstream in(file);
	if (!in) {
		if (std::filesystem::exists(file)) throw Exception("Couldn't read registration " + file.string());
		return;
	}
	std::array<std::string, 7> record;
	for (auto& value : record) {
		if (!(in >> std::quoted(value))) throw Exception("Invalid registration " + file.string());
	}
	const auto destinations = fields(c);
	for (size_t i = 0; i < record.size(); ++i) *destinations[i] = std::move(record[i]);
}

void save_registration(const Creature& c) {
	std::ostringstream out;
	for (const auto* value : fields(const_cast<Creature&>(c))) out << std::quoted(*value) << '\n';
	write_persistent_file(path(c), out.str());
}
