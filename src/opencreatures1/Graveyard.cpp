#include "Graveyard.h"
#include "PersistentFile.h"
#include "common/Exception.h"
#include <array>
#include <fstream>
#include <iomanip>
#include <sstream>

std::vector<GraveRecord> load_graveyard(const std::filesystem::path& path) {
	std::vector<GraveRecord> graves;
	std::ifstream in(path);
	if (!in) {
		if (std::filesystem::exists(path)) throw Exception("Couldn't read graveyard " + path.string());
		return graves;
	}
    std::string line;
    if (!std::getline(in, line)) return graves;
    if (line == "OPENC1-GRAVES 2") {
        while (in >> std::ws && in.peek() != std::char_traits<char>::eof()) {
            GraveRecord g;
            if (!(in >> std::quoted(g.moniker) >> std::quoted(g.name) >> std::quoted(g.sex)
                >> std::quoted(g.age) >> std::quoted(g.headstone) >> g.photo
                >> std::quoted(g.parents) >> std::quoted(g.died) >> std::quoted(g.born)))
                throw Exception("Invalid grave record in " + path.string());
            graves.push_back(std::move(g));
        }
        return graves;
    }
    do {
        if (line.empty()) continue;
        GraveRecord g;
        std::string photo;
        const std::array<std::string*, 8> fields{&g.moniker, &g.name, &g.sex, &g.age, &g.headstone, &photo, &g.parents, &g.died};
        size_t start = 0;
        for (auto* field : fields) {
            const auto bar = line.find('|', start);
            *field = line.substr(start, bar == std::string::npos ? bar : bar - start);
            start = bar == std::string::npos ? line.size() : bar + 1;
        }
        if (!photo.empty()) {
            try { g.photo = std::stoi(photo); }
            catch (...) { throw Exception("Invalid photo selection in " + path.string()); }
        }
        graves.push_back(std::move(g));
    } while (std::getline(in, line));
	return graves;
}

void save_graveyard(const std::filesystem::path& path, const std::vector<GraveRecord>& graves) {
	std::ostringstream out;
	out << "OPENC1-GRAVES 2\n";
	for (const auto& g : graves) {
		out << std::quoted(g.moniker) << ' ' << std::quoted(g.name) << ' ' << std::quoted(g.sex)
			<< ' ' << std::quoted(g.age) << ' ' << std::quoted(g.headstone) << ' ' << g.photo
			<< ' ' << std::quoted(g.parents) << ' ' << std::quoted(g.died) << ' ' << std::quoted(g.born) << '\n';
	}
	write_persistent_file(path, out.str());
}
