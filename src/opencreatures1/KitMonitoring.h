#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>
struct Creature;

struct ChemicalHistory {
	static constexpr size_t capacity = 2048;
	static constexpr size_t channels = 4;
	std::string moniker;
	std::array<std::deque<float>, 256> samples;
	std::optional<uint32_t> last_tick;
	void sample(const Creature&, uint32_t tick);
};

struct ObservationOptions {
	int life_force_warning = 25;
	bool warn_life_force = true, warn_pregnant = true, warn_birth = true;
	bool message_box = false; // native presentation remains nonmodal
};

class ObservationHistory {
public:
	enum Alert { LOW_LIFE, PREGNANT, BIRTH };
	std::vector<Alert> poll(const std::string& identity, bool dead, int life, int gestation, const ObservationOptions&);
	void clear() { history.clear(); }
private:
	std::map<std::string, std::array<bool, 3>> history;
};

namespace sfc { struct NeuronV1; }
enum class BrainMeasure { OUTPUT, STATE, SUSCEPTIBILITY, SHORT_WEIGHT, LONG_WEIGHT, STRENGTH };
uint8_t monitor_neuron(const sfc::NeuronV1&, BrainMeasure, unsigned connection_type = 0, size_t connection = 0);
