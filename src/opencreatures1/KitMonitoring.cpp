#include "KitMonitoring.h"
#include "objects/Creature.h"
#include "fileformats/sfc/CBiochemistry.h"

void ChemicalHistory::sample(const Creature& c, uint32_t tick) {
	if (moniker != c.moniker) { *this = {}; moniker = c.moniker; }
	if (last_tick == tick) return;
	last_tick = tick;
	for (size_t n = 0; n < samples.size(); ++n) {
		auto& values = samples[n];
		values.push_back(c.biochemistry ? c.biochemistry->chemicals[n].concentration : 0);
		if (values.size() > capacity) values.pop_front();
	}
}

std::vector<ObservationHistory::Alert> ObservationHistory::poll(const std::string& identity, bool dead, int life, int gestation, const ObservationOptions& options) {
	std::vector<Alert> result;
	if (dead) return result;
	auto& previous = history[identity];
	const std::array<bool, 3> current{life < options.life_force_warning, gestation > 0, gestation >= 8};
	const std::array<bool, 3> enabled{options.warn_life_force, options.warn_pregnant, options.warn_birth};
	for (size_t i = 0; i < current.size(); ++i) {
		if (current[i] && !previous[i] && enabled[i]) result.push_back(static_cast<Alert>(i));
	}
	previous = current;
	return result;
}

#include "fileformats/sfc/CBrain.h"
uint8_t monitor_neuron(const sfc::NeuronV1& neuron, BrainMeasure measure, unsigned type, size_t connection) {
    if (measure == BrainMeasure::OUTPUT) return neuron.output;
    if (measure == BrainMeasure::STATE) return neuron.state;
    if (type > 1) return 0;
    const auto& dendrites = type == 0 ? neuron.dendrites0 : neuron.dendrites1;
    if (connection >= dendrites.size()) return 0;
    const auto& d = dendrites[connection];
    switch (measure) {
        case BrainMeasure::SUSCEPTIBILITY: return d.suscept;
        case BrainMeasure::SHORT_WEIGHT: return d.stw;
        case BrainMeasure::LONG_WEIGHT: return d.ltw;
        case BrainMeasure::STRENGTH: return d.strength;
        default: return 0;
    }
}
