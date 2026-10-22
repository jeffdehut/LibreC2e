#include "CobInjector.h"
#include "PersistentFile.h"
#include "../../third_party/librecreatures/C1CobHeader.h"
#include "WorldSystems.h"
#include <fstream>
#include <array>
#include <sstream>

#include "EngineContext.h"
#include "Macro.h"
#include "MacroManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "Scriptorium.h"
#include "common/Ascii.h"
#include "common/Exception.h"
#include "fileformats/c1cobfile.h"

#include <algorithm>
#include <cstdio>
#include <fmt/core.h>

namespace fs = std::filesystem;

std::vector<fs::path> list_injectable_cobs() {
	auto results = g_engine_context.paths->find_path_wildcard(PATH_TYPE_MAIN, "*.cob");
	auto removers = g_engine_context.paths->find_path_wildcard(PATH_TYPE_MAIN, "*.rcb");
	results.insert(results.end(), removers.begin(), removers.end());
	std::sort(results.begin(), results.end(), [](const fs::path& a, const fs::path& b) {
		return to_ascii_lowercase(a.string()) < to_ascii_lowercase(b.string());
	});
	return results;
}

namespace {
fs::path allowance_path(const fs::path& filename) {
    return g_engine_context.paths->find_path(PATH_TYPE_MAIN, "") /
        (get_world_file_name() + "." + filename.filename().string() + ".allowance");
}
}

// Only remove a historical misinstallation when both its derived classifier
// and its complete script text identify the same package. Preserve custom scripts.
static void remove_legacy_headers(Scriptorium& staged, const c1cobfile& package) {
    for (const auto& script : package.object_scripts) {
        std::istringstream input(script); std::string keyword; std::array<int, 4> intended{};
        if (!(input >> keyword >> intended[0] >> intended[1] >> intended[2] >> intended[3]) || keyword != "scrp") continue;
        input >> std::ws; if (input.peek() == ',') input.get(); input >> std::ws;
        const auto body = input.tellg();
        openc1::librecreatures::InstalledScript legacy{};
        if (body < 0 || !openc1::librecreatures::original_script_classifier(script, legacy)) continue;
        const auto& c = legacy.classifier;
        if (intended == std::array<int, 4>{c.family, c.genus, c.species, legacy.event}) continue;
        if (staged.get(c.family, c.genus, c.species, legacy.event) == script.substr(static_cast<size_t>(body)))
            staged.remove(c.family, c.genus, c.species, legacy.event);
    }
}

int cob_remaining(const fs::path& filename) {
    const auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, filename.string());
    if (path.empty()) throw Exception("Couldn't find COB " + filename.string());
    const auto cob = read_c1cobfile(path);
    if (to_ascii_lowercase(filename.extension().string()) == ".rcb" || cob.quantity_available >= 255) return -1;
    unsigned consumed = 0;
    std::ifstream in(allowance_path(filename));
    if (in && !(in >> consumed)) throw Exception("Invalid COB allowance " + allowance_path(filename).string());
    return static_cast<int>(cob.quantity_available) - static_cast<int>(std::min<unsigned>(consumed, cob.quantity_available));
}

void inject_cob(const fs::path& filename) {
    const auto path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, filename.string());
    if (path.empty()) throw Exception("Couldn't find COB " + filename.string());
    const auto cob = read_c1cobfile(path);
    const int remaining = cob_remaining(filename);
    if (remaining == 0) throw Exception("COB allowance exhausted: " + filename.string());

    // Validate every header and scriptorium capacity before changing live definitions.
    // Stream parsing deliberately accepts tabs, extra spaces and a spaced comma.
    Scriptorium staged = *g_engine_context.scriptorium;
    remove_legacy_headers(staged, cob);
    if (to_ascii_lowercase(filename.extension().string()) == ".rcb") {
        auto source = filename; source.replace_extension(".cob");
        const auto source_path = g_engine_context.paths->find_path(PATH_TYPE_MAIN, source.string());
        if (!source_path.empty()) remove_legacy_headers(staged, read_c1cobfile(source_path));
    }
    for (const auto& script : cob.object_scripts) {
        std::istringstream input(script);
        std::string keyword;
        std::array<int, 4> c{};
        if (!(input >> keyword >> c[0] >> c[1] >> c[2] >> c[3]) || keyword != "scrp"
            || std::any_of(c.begin(), c.end(), [](int v) { return v < 0 || v > 255; }))
            throw Exception("Invalid scrp header in " + filename.string());
        input >> std::ws;
        if (input.peek() == ',') input.get();
        input >> std::ws;
        const auto position = input.tellg();
        if (position < 0) throw Exception("Missing object script in " + filename.string());
        staged.remove(c[0], c[1], c[2], c[3]);
        staged.add(c[0], c[1], c[2], c[3], script.substr(static_cast<size_t>(position)));
    }
    auto original = *g_engine_context.scriptorium;
    *g_engine_context.scriptorium = std::move(staged);
    try {
        for (const auto& script : cob.install_scripts) {
            Macro m(script);
            m.ownr = m.targ = g_engine_context.pointer->m_pointer_tool;
            // Synchronous first dispatch: a rejected command must not consume stock.
            g_engine_context.macros->run_now(std::move(m), false);
        }
        if (remaining > 0) {
            const unsigned consumed = cob.quantity_available - remaining + 1;
            write_persistent_file(allowance_path(filename), std::to_string(consumed) + "\n");
        }
    } catch (...) {
        *g_engine_context.scriptorium = std::move(original);
        throw;
    }
    fmt::print("* Injected {:?}: accepted ({} object scripts, {} install scripts)\n", cob.name, cob.object_scripts.size(), cob.install_scripts.size());
}
