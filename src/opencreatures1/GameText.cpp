#include "GameText.h"

#include "EngineContext.h"
#include "PathManager.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace fs = std::filesystem;

namespace GameText {

namespace {

constexpr uint64_t BASE = 1000003;

// runs of printable text in the game's programs: UTF-16 (their dialogs and string tables)
// and plain bytes
const std::vector<std::string>& texts() {
	static std::vector<std::string> runs;
	static uint64_t generation = UINT64_MAX;
	if (generation == g_engine_context.generation()) {
		return runs;
	}
	generation = g_engine_context.generation();
	runs.clear();
	fs::path folder;
	try {
		folder = g_engine_context.paths->find_path(PATH_TYPE_MAIN, "");
	} catch (const std::exception&) {
		return runs; // no game folder: our own wording everywhere
	}
	std::error_code err;
	for (auto& entry : fs::directory_iterator(folder, err)) {
		std::string ext = entry.path().extension().string();
		for (auto& ch : ext) {
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		if (ext != ".exe" && ext != ".dll") {
			continue;
		}
		std::ifstream in(entry.path(), std::ios::binary);
		const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		auto printable = [](unsigned c) { return (c >= 0x20 && c < 0x7f) || c == '\r' || c == '\n'; };
		std::string run;
		// UTF-16 little-endian, from either alignment
		for (size_t start = 0; start < 2; ++start) {
			for (size_t i = start; i + 1 < data.size(); i += 2) {
				const unsigned lo = static_cast<unsigned char>(data[i]), hi = static_cast<unsigned char>(data[i + 1]);
				if (hi == 0 && printable(lo)) {
					run += static_cast<char>(lo);
					continue;
				}
				if (run.size() >= 6) {
					runs.push_back(run);
				}
				run.clear();
			}
			if (run.size() >= 6) {
				runs.push_back(run);
			}
			run.clear();
		}
		for (char ch : data) {
			if (printable(static_cast<unsigned char>(ch))) {
				run += ch;
				continue;
			}
			if (run.size() >= 6) {
				runs.push_back(run);
			}
			run.clear();
		}
	}
	return runs;
}

std::string trim(std::string s) {
	const auto a = s.find_first_not_of(" \r\n\t");
	const auto b = s.find_last_not_of(" \r\n\t");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

} // namespace

uint64_t fingerprint(const std::string& text) {
	uint64_t h = 0;
	for (char c : text) {
		h = h * BASE + static_cast<unsigned char>(c);
	}
	return h;
}

std::string line(Original original, const char* ours) {
	static std::map<uint64_t, std::string> found; // by hash; empty if the game hasn't it
	static uint64_t generation = UINT64_MAX;
	if (generation != g_engine_context.generation()) {
		found.clear();
		generation = g_engine_context.generation();
	}
	auto it = found.find(original.hash);
	if (it == found.end()) {
		std::string match;
		const size_t n = original.length;
		uint64_t top = 1; // BASE^n, for rolling the oldest character out
		for (size_t i = 0; i < n; ++i) {
			top *= BASE;
		}
		for (const auto& run : texts()) {
			if (run.size() < n || n == 0) {
				continue;
			}
			uint64_t h = 0;
			for (size_t i = 0; i < run.size(); ++i) {
				h = h * BASE + static_cast<unsigned char>(run[i]);
				if (i >= n) {
					h -= top * static_cast<unsigned char>(run[i - n]);
				}
				if (i + 1 >= n && h == original.hash) {
					match = run.substr(i + 1 - n, n);
					break;
				}
			}
			if (!match.empty()) {
				break;
			}
		}
		it = found.emplace(original.hash, trim(match)).first;
	}
	return it->second.empty() ? std::string(ours) : it->second;
}

} // namespace GameText
