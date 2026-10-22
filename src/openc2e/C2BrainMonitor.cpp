#include "C2BrainMonitor.h"
#include "creatures/oldBrain.h"
#include "common/Exception.h"
#include <unordered_map>

namespace c2monitor {
static void require(bool ok, const char* message) { if (!ok) throw Exception(message); }
Brain capture(oldBrain& brain) {
	Brain result;
	std::unordered_map<const oldNeuron*, Source> identities;
	for (auto* lobe : brain.lobes) {
		require(lobe && lobe->getGene(), "C2 monitor: missing lobe metadata");
		Lobe snapshot;
		snapshot.x = lobe->getGene()->x; snapshot.y = lobe->getGene()->y;
		snapshot.width = lobe->getWidth(); snapshot.height = lobe->getHeight();
		for (unsigned n = 0; n < lobe->getNoNeurons(); ++n) {
			auto* neuron = lobe->getNeuron(n);
			identities.emplace(neuron, Source{result.size(), n});
			snapshot.neurons.push_back({neuron->state, neuron->output, {}});
		}
		result.push_back(std::move(snapshot));
	}
	for (size_t l = 0; l < result.size(); ++l)
		for (size_t n = 0; n < result[l].neurons.size(); ++n)
			for (unsigned c = 0; c < 2; ++c)
				for (auto& dendrite : brain.lobes[l]->getNeuron(static_cast<unsigned>(n))->dendrites[c]) {
					if (!dendrite.src) continue;
					auto found = identities.find(dendrite.src);
					require(found != identities.end(), "C2 monitor: connection source is outside the brain");
					result[l].neurons[n].sources[c].push_back(found->second);
				}
	return result;
}
static void validate(const Brain& brain) {
	for (const auto& lobe : brain) {
		require(lobe.width <= 64 && lobe.height <= 64 && lobe.x <= 63 && lobe.y <= 63 && lobe.x + lobe.width <= 64 && lobe.y + lobe.height <= 64,
			"C2 monitor: lobe lies outside the supported 64 by 64 grid");
		require(lobe.neurons.size() == lobe.width * lobe.height, "C2 monitor: lobe geometry and neuron count disagree");
	}
}
static std::string triplet(const Lobe& lobe, size_t n, uint8_t value, unsigned divisor) {
	return {static_cast<char>(48 + lobe.x + n % lobe.width), static_cast<char>(48 + lobe.y + n / lobe.width), static_cast<char>(48 + value / divisor)};
}
static void append(std::string& output, const std::string& record, size_t capacity) {
	require(record.size() <= capacity && output.size() <= capacity - record.size(), "C2 monitor: report exceeds receiving capacity");
	output += record;
}
std::string lneu(const Brain& brain, unsigned mask, size_t capacity) {
	require(mask <= 3, "C2 monitor: lneu mask must be 0 through 3");
	validate(brain);
	std::string output;
	for (const auto& lobe : brain) for (size_t n = 0; n < lobe.neurons.size(); ++n) {
		const auto& neuron = lobe.neurons[n];
		if (!neuron.state) continue;
		auto record = triplet(lobe, n, neuron.state, 4); // C2-COMM-019
		for (unsigned c = 0; c < 2; ++c) if (mask & (1u << c)) {
			std::string sources;
			unsigned count = 0;
			for (const auto& source : neuron.sources[c]) {
				require(source.lobe < brain.size() && source.neuron < brain[source.lobe].neurons.size(), "C2 monitor: invalid source identity");
				const auto& src = brain[source.lobe];
				auto state = src.neurons[source.neuron].state;
				if (state) { sources += triplet(src, source.neuron, state, 4); if (++count == 8) break; }
			}
			record += static_cast<char>(48 + count); // C2-COMM-021
			record += sources;
		}
		append(output, record, capacity);
	}
	return output;
}
std::string dde_activity(const Brain& brain, Activity selection, size_t capacity) {
	validate(brain);
	std::string output;
	for (const auto& lobe : brain) for (size_t n = 0; n < lobe.neurons.size(); ++n) {
		auto value = selection == Activity::state ? lobe.neurons[n].state : lobe.neurons[n].output;
		if (value) append(output, triplet(lobe, n, value, 16), capacity); // C2-COMM-031/032
	}
	return output;
}
}
