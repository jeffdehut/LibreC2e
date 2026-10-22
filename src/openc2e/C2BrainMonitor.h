#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
class oldBrain;
namespace c2monitor {
struct Source { size_t lobe, neuron; };
struct Neuron {
	uint8_t state = 0, output = 0;
	std::array<std::vector<Source>, 2> sources;
};
struct Lobe {
	unsigned x = 0, y = 0, width = 0, height = 0;
	std::vector<Neuron> neurons;
};
using Brain = std::vector<Lobe>;
Brain capture(oldBrain&);
// Visible ASCII-byte reply; C++ strings supply the final NUL via c_str().
// Policy F063: reject an oversized report atomically, never truncate records.
std::string lneu(const Brain&, unsigned mask, size_t capacity = 8191);
enum class Activity { state, output };
// Engine encoding only. No Windows DDE registration or selector transport is implied.
std::string dde_activity(const Brain&, Activity, size_t capacity = 8191);
}
