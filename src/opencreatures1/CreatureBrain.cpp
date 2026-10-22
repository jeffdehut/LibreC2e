#include "CreatureBrain.h"

#include "Biochemistry.h"
#include "common/Random.h"
#include "fileformats/sfc/CBrain.h"

#include <algorithm>
#include <set>

namespace {

// Creatures 1 SVRule tokens (specs Q14, Q19).
enum SVRuleToken : uint8_t {
	TOKEN_END = 0,
	TOKEN_ZERO = 1,
	TOKEN_ONE = 2,
	TOKEN_64 = 3,
	TOKEN_255 = 4,
	TOKEN_CHEM0 = 5,
	TOKEN_CHEM1 = 6,
	TOKEN_CHEM2 = 7,
	TOKEN_CHEM3 = 8,
	TOKEN_STATE = 9,
	TOKEN_OUTPUT = 10,
	TOKEN_THRESHOLD = 11,
	TOKEN_TYPE0 = 12,
	TOKEN_TYPE1 = 13,
	TOKEN_ANDED0 = 14,
	TOKEN_ANDED1 = 15,
	TOKEN_INPUT = 16,
	TOKEN_CONDUCT = 17,
	TOKEN_SUSCEPT = 18,
	TOKEN_STW = 19,
	TOKEN_LTW = 20,
	TOKEN_STRENGTH = 21,
	TOKEN_TRUE = 22,
	TOKEN_PLUS = 23,
	TOKEN_MINUS = 24,
	TOKEN_TIMES = 25,
	TOKEN_INCR = 26,
	TOKEN_DECR = 27,
	// 28 and above do nothing
};

// The input lobes (0-5) never run a state rule (spec Q15).
constexpr size_t FIRST_RULE_LOBE = 6;

struct LobeView {
	sfc::LobeV1* lobe;
	sfc::NeuronV1* neurons; // first neuron of this lobe
	size_t count;
};

struct BrainView {
	sfc::CBrainV1& brain;
	std::vector<LobeView> lobes;

	explicit BrainView(sfc::CBrainV1& b)
		: brain(b) {
		size_t first = 0;
		for (auto& l : b.lobes) {
			size_t available = first < b.neurons.size() ? b.neurons.size() - first : 0;
			size_t count = std::min<size_t>(l.num_neurons, available);
			lobes.push_back({&l, count ? &b.neurons[first] : nullptr, count});
			first += l.num_neurons;
		}
	}

	sfc::NeuronV1* source(uint32_t lobe, uint32_t index) {
		if (lobe >= lobes.size() || index >= lobes[lobe].count) {
			return nullptr;
		}
		return &lobes[lobe].neurons[index];
	}
};

uint8_t clamp_byte(int32_t value) {
	return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

// Moves `value` towards `goal` at a Creatures 1 rate: the distance is multiplied by the
// rate's fraction when the step applies (spec Q16, the same timing as chemical half-lives).
uint8_t relax(uint8_t value, uint8_t goal, uint8_t rate, uint32_t step) {
	if (value == goal) {
		return value;
	}
	if (value > goal) {
		return static_cast<uint8_t>(goal + decay_chemical(static_cast<uint8_t>(value - goal), rate, step));
	}
	return static_cast<uint8_t>(goal - decay_chemical(static_cast<uint8_t>(goal - value), rate, step));
}

const sfc::DendriteTypeV1& dendrite_type(const LobeView& l, int type) {
	return type == 0 ? l.lobe->d0_type : l.lobe->d1_type;
}

std::vector<sfc::DendriteV1>& dendrites_of(sfc::NeuronV1& n, int type) {
	return type == 0 ? n.dendrites0 : n.dendrites1;
}

// Batch 8: sum floor(source output * STW / 256), cap at 255, then multiply by
// input gain and divide by 256. Strength controls learning/migration, not signal weight.
// "Anded" sums are 0 unless every source is firing.
uint8_t dendrite_sum(BrainView& b, const LobeView& l, const sfc::NeuronV1& n, int type, bool anded) {
	const auto& dendrites = type == 0 ? n.dendrites0 : n.dendrites1;
	const auto& info = dendrite_type(l, type);
	if (dendrites.empty()) {
		return 0;
	}
	int32_t sum = 0;
	bool all_firing = true;
	for (const auto& d : dendrites) {
		auto* src = b.source(info.source_lobe, d.source_index);
		uint8_t output = src ? src->output : 0;
		if (output == 0) {
			all_firing = false;
		} else {
			sum += output * d.stw / 256;
		}
	}
	if (anded && !all_firing) {
		return 0;
	}
	return clamp_byte(std::min(sum, 255) * l.lobe->inputgain / 256);
}

struct RuleContext {
	BrainView& brain;
	const LobeView& lobe;
	const sfc::NeuronV1& neuron;
	// the dendrite as it was before this step's update, and its source; null for state rules
	const sfc::DendriteV1* dendrite;
	const sfc::NeuronV1* dendrite_source;
};

uint8_t operand(const RuleContext& c, uint8_t token) {
	const auto* d = c.dendrite;
	switch (token) {
		case TOKEN_ZERO: return 0;
		case TOKEN_ONE: return 1;
		case TOKEN_64: return 64;
		case TOKEN_255: return 255;
		case TOKEN_CHEM0:
		case TOKEN_CHEM1:
		case TOKEN_CHEM2:
		case TOKEN_CHEM3: return c.lobe.lobe->chemicals[token - TOKEN_CHEM0];
		case TOKEN_STATE: return c.neuron.state;
		case TOKEN_OUTPUT: return c.neuron.output;
		case TOKEN_THRESHOLD: return c.lobe.lobe->threshold;
		case TOKEN_TYPE0: return dendrite_sum(c.brain, c.lobe, c.neuron, 0, false);
		case TOKEN_TYPE1: return dendrite_sum(c.brain, c.lobe, c.neuron, 1, false);
		case TOKEN_ANDED0: return dendrite_sum(c.brain, c.lobe, c.neuron, 0, true);
		case TOKEN_ANDED1: return dendrite_sum(c.brain, c.lobe, c.neuron, 1, true);
		// the output of the dendrite's source neuron; undefined in state rules, use 0
		case TOKEN_INPUT: return c.dendrite_source ? c.dendrite_source->output : 0;
		case TOKEN_CONDUCT: return 0;
		case TOKEN_SUSCEPT: return d ? d->suscept : 0;
		case TOKEN_STW: return d ? d->stw : 0;
		case TOKEN_LTW: return d ? d->ltw : 0;
		case TOKEN_STRENGTH: return d ? d->strength : 0;
		default: return 0;
	}
}

// Evaluates a 10-byte SVRule. Arithmetic saturates (spec Q14).
uint8_t evaluate(const RuleContext& c, const std::array<uint8_t, 10>& rule) {
	int32_t acc = 0;
	for (size_t i = 0; i < rule.size(); ++i) {
		auto next = [&]() -> int32_t { return ++i < rule.size() ? operand(c, rule[i]) : 0; };
		const uint8_t token = rule[i];
		if (token == TOKEN_END) {
			break;
		}
		switch (token) {
			case TOKEN_TRUE:
				// abandon the rule if the accumulator is 0
				if (!acc) {
					return 0;
				}
				break;
			case TOKEN_PLUS: acc = std::min(255, acc + next()); break;
			case TOKEN_MINUS: acc = std::max(0, acc - next()); break;
			case TOKEN_TIMES: acc = acc * next() / 256; break;
			case TOKEN_INCR: acc = std::min(255, acc + 1); break;
			case TOKEN_DECR: acc = std::max(0, acc - 1); break;
			default:
				if (token < TOKEN_TRUE) {
					acc = operand(c, token);
				}
				// tokens 28 and above do nothing
				break;
		}
	}
	return clamp_byte(acc);
}

void reconnect(sfc::DendriteV1& d, uint32_t source_index, const sfc::DendriteTypeV1& info) {
	// spec Q18: LTW and STW get one random value in range, strength another; susceptibility
	// is left alone
	d.source_index = source_index;
	d.ltw = d.stw = rand_uint8(std::min(info.min_ltw, info.max_ltw), std::max(info.min_ltw, info.max_ltw));
	d.strength = rand_uint8(std::min(info.min_strength, info.max_strength), std::max(info.min_strength, info.max_strength));
}

void tick_dendrites(BrainView& b, const LobeView& l, sfc::NeuronV1& n, int type, uint32_t step) {
	auto& dendrites = dendrites_of(n, type);
	const auto& info = dendrite_type(l, type);
	for (auto& d : dendrites) {
		const auto before = d;
		const auto* src = b.source(info.source_lobe, d.source_index);
		RuleContext c{b, l, n, &before, src};

		// susceptibility rises an eighth of the way to the rule's result, otherwise decays to 0
		uint8_t target = evaluate(c, info.suscept_svrule);
		if (target > d.suscept) {
			d.suscept = static_cast<uint8_t>(d.suscept + (target - d.suscept) / 8);
		} else if (d.suscept) {
			d.suscept = relax(d.suscept, 0, info.relax_suscept, step);
		}

		// reinforcement only raises STW (spec Q17)
		if (d.suscept) {
			int32_t amount = evaluate(c, info.reinforce_svrule) * d.suscept / 256;
			if (amount && d.stw < d.ltw + amount) {
				d.stw = clamp_byte(d.ltw + amount);
			}
		}

		// STW relaxes towards LTW, and LTW follows STW one step at a time
		d.stw = relax(d.stw, d.ltw, info.relax_stw, step);
		if (info.ltw_gain_rate && step % info.ltw_gain_rate == 0) {
			if (d.ltw < d.stw) {
				d.ltw++;
			} else if (d.ltw > d.stw) {
				d.ltw--;
			}
		}

		// strength rises to the gain rule's result, and drops by 1 while the loss rule is non-zero
		if (info.gain_rate && step % info.gain_rate == 0 && d.strength < 255) {
			uint8_t gain = evaluate(c, info.gain_svrule);
			if (gain > d.strength) {
				d.strength = gain;
			}
		}
		if (info.lose_rate && step % info.lose_rate == 0 && d.strength > 0) {
			if (evaluate(c, info.lose_svrule)) {
				d.strength--;
				if (!d.strength) {
					// the dendrite is now loose
					d.suscept = d.stw = d.ltw = 0;
					n.state = n.output = 0;
				}
			}
		}
	}
}

std::vector<uint32_t> firing_neurons(const LobeView& l) {
	std::vector<uint32_t> result;
	for (size_t i = 0; i < l.count; ++i) {
		if (l.neurons[i].output) {
			result.push_back(static_cast<uint32_t>(i));
		}
	}
	return result;
}

// Migration flag 1: firing neurons reconnect their first loose dendrite to a random firing
// source neuron (spec Q18).
void migrate_one_at_a_time(BrainView& b, LobeView& l, int type) {
	const auto& info = dendrite_type(l, type);
	if (info.source_lobe >= b.lobes.size()) {
		return;
	}
	auto sources = firing_neurons(b.lobes[info.source_lobe]);
	if (sources.empty()) {
		return;
	}
	for (size_t i = 0; i < l.count; ++i) {
		auto& n = l.neurons[i];
		if (!n.output) {
			continue;
		}
		auto& dendrites = dendrites_of(n, type);
		auto loose = std::find_if(dendrites.begin(), dendrites.end(), [](auto& d) { return d.strength == 0; });
		if (loose == dendrites.end()) {
			continue;
		}
		uint32_t source = sources[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(sources.size()) - 1))];
		bool already = std::any_of(dendrites.begin(), dendrites.end(), [&](auto& d) { return d.strength != 0 && d.source_index == source; });
		if (!already) {
			reconnect(*loose, source, info);
			if (l.lobe->loose_dendrites[static_cast<size_t>(type)])
				--l.lobe->loose_dendrites[static_cast<size_t>(type)];
		}
	}
}

// Migration flag 2: at most one neuron per step rewires every dendrite of the type to a run
// of distinct firing source neurons (spec Q18).
// Lobe-0 neurons copied from a lobe whose perceptible value is 2 are marked with that lobe's
// number; no two sources in a run may share a mark (batch 2, Q7).
std::vector<uint32_t> perception_marks(BrainView& b) {
	std::vector<uint32_t> marks(b.lobes.empty() ? 0 : b.lobes[0].count, 0);
	size_t offset = 0;
	for (size_t i = 1; i < b.lobes.size(); ++i) {
		if (!b.lobes[i].lobe->perceptible) {
			continue;
		}
		for (size_t j = 0; j < b.lobes[i].count && offset < marks.size(); ++j, ++offset) {
			if (b.lobes[i].lobe->perceptible == 2) {
				marks[offset] = static_cast<uint32_t>(i);
			}
		}
	}
	return marks;
}

void migrate_whole_set(BrainView& b, LobeView& l, size_t lobe_index, int type, BrainMigrationState& state) {
	const auto& info = dendrite_type(l, type);
	if (info.source_lobe >= b.lobes.size() || !l.count) {
		return;
	}
	if (state.next_neuron.size() < b.lobes.size()) {
		state.next_neuron.resize(b.lobes.size(), {0, 0});
	}
	// cycle down through neurons with loose dendrites of this type, wrapping round
	uint32_t& cursor = state.next_neuron[lobe_index][static_cast<size_t>(type)];
	sfc::NeuronV1* chosen = nullptr;
	for (size_t tries = 0; tries < l.count; ++tries) {
		cursor = cursor == 0 ? static_cast<uint32_t>(l.count - 1) : cursor - 1;
		auto& n = l.neurons[cursor];
		auto& dendrites = dendrites_of(n, type);
		if (std::any_of(dendrites.begin(), dendrites.end(), [](auto& d) { return d.strength == 0; })) {
			chosen = &n;
			break;
		}
	}
	if (!chosen) {
		return;
	}

	auto rewire = [&](int t) {
		auto& dendrites = dendrites_of(*chosen, t);
		const auto& tinfo = dendrite_type(l, t);
		if (dendrites.empty() || tinfo.source_lobe >= b.lobes.size()) {
			return;
		}
		auto sources = firing_neurons(b.lobes[tinfo.source_lobe]);
		if (sources.size() < dendrites.size()) {
			return;
		}
		for (size_t i = sources.size(); i > 1; --i) {
			std::swap(sources[i - 1], sources[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(i) - 1))]);
		}
		const auto marks = tinfo.source_lobe == LOBE_PERCEPTION ? perception_marks(b) : std::vector<uint32_t>{};
		for (size_t start = 0; start + dendrites.size() <= sources.size(); ++start) {
			std::multiset<uint32_t> run(sources.begin() + static_cast<ptrdiff_t>(start), sources.begin() + static_cast<ptrdiff_t>(start + dendrites.size()));
			// no two sources from the same mutually exclusive lobe
			std::set<uint32_t> seen_marks;
			bool clash = false;
			for (uint32_t src : run) {
				uint32_t mark = src < marks.size() ? marks[src] : 0;
				if (mark && !seen_marks.insert(mark).second) {
					clash = true;
				}
			}
			if (clash) {
				continue;
			}
			// skip runs another firing neuron of this lobe is already wired to
			bool taken = false;
			for (size_t i = 0; i < l.count && !taken; ++i) {
				auto& other = l.neurons[i];
				if (&other == chosen || !other.output) {
					continue;
				}
				auto& od = dendrites_of(other, t);
				std::multiset<uint32_t> other_sources;
				for (auto& d : od) {
					other_sources.insert(d.source_index);
				}
				taken = other_sources == run;
			}
			if (taken) {
				continue;
			}
			auto it = run.begin();
			for (auto& d : dendrites) {
				reconnect(d, *it++, tinfo);
			}
			return;
		}
		// no run worked: keep the old sources, but with nothing learned
		for (auto& d : dendrites) {
			d.stw = d.ltw = d.strength = 0;
		}
	};
	rewire(type);
	const int other = 1 - type;
	if ((other == 0 ? l.lobe->d0_type : l.lobe->d1_type).migration == 2) {
		rewire(other);
	}
}

void tick_lobe(BrainView& b, LobeView& l, size_t index, uint32_t step, BrainMigrationState& migration) {
	auto& lobe = *l.lobe;
	size_t firing = 0;
	for (size_t i = 0; i < l.count; ++i) {
		auto& n = l.neurons[i];
		// input lobes keep their state; the rest run their state rule (empty gives 0)
		uint8_t state = n.state;
		if (index >= FIRST_RULE_LOBE) {
			RuleContext c{b, l, n, nullptr, nullptr};
			state = evaluate(c, lobe.svrule);
		}
		n.state = state;
		n.output = state >= lobe.threshold ? static_cast<uint8_t>(state - lobe.threshold) : 0;
		if (n.output) {
			firing++;
		}
		// after the output is worked out, the state relaxes towards rest
		n.state = relax(n.state, lobe.reststate, lobe.leakage, step);
		tick_dendrites(b, l, n, 0, step);
		tick_dendrites(b, l, n, 1, step);
	}
	lobe.activity = l.count ? static_cast<uint8_t>(firing * 255 / l.count) : 0;

	// winner takes all
	if (lobe.flags & 1) {
		sfc::NeuronV1* best = nullptr;
		for (size_t i = 0; i < l.count; ++i) {
			if (!l.neurons[i].wta_disable && l.neurons[i].output && (!best || l.neurons[i].output > best->output)) {
				best = &l.neurons[i];
			}
		}
		for (size_t i = 0; i < l.count; ++i) {
			if (&l.neurons[i] != best) {
				l.neurons[i].output = 0;
			}
		}
	}

	// Source-informed C1 emitter counters: flag 1 uses the minimum loose count
	// across neurons; other modes count neurons with any loose connection.
	// These are chemical feedback signals, not the lobe's four rule chemicals.
	for (int type = 0; type < 2; ++type) {
		const bool minimum = dendrite_type(l, type).migration == 1;
		size_t count = minimum && l.count ? 255 : 0;
		for (size_t i = 0; i < l.count; ++i) {
			const auto& ds = dendrites_of(l.neurons[i], type);
			const size_t loose = static_cast<size_t>(std::count_if(ds.begin(), ds.end(), [](const auto& d) { return d.strength == 0; }));
			if (minimum) count = std::min(count, loose);
			else if (loose) ++count;
		}
		lobe.loose_dendrites[static_cast<size_t>(type)] = static_cast<uint8_t>(std::min<size_t>(255, count));
	}

	for (int type = 0; type < 2; ++type) {
		switch (dendrite_type(l, type).migration) {
			case 1: migrate_one_at_a_time(b, l, type); break;
			case 2:
				// a combined rewire of both types happens once, from type 0
				if (type == 0 || lobe.d0_type.migration != 2) {
					migrate_whole_set(b, l, index, type, migration);
				}
				break;
			default: break;
		}
	}
}

} // namespace

sfc::NeuronV1* brain_neuron(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron) {
	return BrainView(brain).source(lobe, neuron);
}

void brain_set_input(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron, uint8_t value) {
	if (auto* n = brain_neuron(brain, lobe, neuron)) {
		n->state = value;
	}
}

void brain_add_input(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron, int32_t value) {
	if (auto* n = brain_neuron(brain, lobe, neuron)) {
		n->state = clamp_byte(n->state + value);
	}
}

void brain_clear_lobe(sfc::CBrainV1& brain, uint32_t lobe) {
	BrainView b(brain);
	if (lobe < b.lobes.size()) {
		for (size_t i = 0; i < b.lobes[lobe].count; ++i) {
			b.lobes[lobe].neurons[i].state = b.lobes[lobe].neurons[i].output = 0;
		}
	}
}

void brain_clear_neuron(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron) {
	if (auto* n = brain_neuron(brain, lobe, neuron)) {
		n->state = n->output = 0;
	}
}

void brain_tick(sfc::CBrainV1& brain, uint32_t step, BrainMigrationState& migration) {
	BrainView b(brain);
	if (b.lobes.empty()) {
		return;
	}
	// Batch 2 Q1 and batch 8: lobes 1-5 run first so lobe 0 can
	// copy their outputs, then lobe 0, then the rest in order.
	std::vector<size_t> order;
	for (size_t i = 1; i < std::min<size_t>(b.lobes.size(), FIRST_RULE_LOBE); ++i) {
		order.push_back(i);
	}
	order.push_back(LOBE_PERCEPTION);
	for (size_t i = FIRST_RULE_LOBE; i < b.lobes.size(); ++i) {
		order.push_back(i);
	}

	for (size_t index : order) {
		if (index == LOBE_PERCEPTION) {
			// lobe 0 takes the larger of its own state and the output of each perceptible lobe,
			// in lobe order: lobes 1-5 from this step, 6 and up (e.g. attention) from the
			// previous one (batch 2, Q1)
			size_t offset = 0;
			for (size_t i = 1; i < b.lobes.size(); ++i) {
				if (!b.lobes[i].lobe->perceptible) {
					continue;
				}
				for (size_t j = 0; j < b.lobes[i].count && offset < b.lobes[0].count; ++j, ++offset) {
					auto& dest = b.lobes[0].neurons[offset];
					dest.state = std::max(dest.state, b.lobes[i].neurons[j].output);
				}
			}
		}
		tick_lobe(b, b.lobes[index], index, step, migration);
	}
}

int32_t brain_winner(const sfc::CBrainV1& brain, uint32_t lobe) {
	BrainView b(const_cast<sfc::CBrainV1&>(brain));
	if (lobe >= b.lobes.size()) {
		return -1;
	}
	int32_t best = -1;
	uint8_t best_output = 0;
	for (size_t i = 0; i < b.lobes[lobe].count; ++i) {
		if (b.lobes[lobe].neurons[i].output > best_output) {
			best_output = b.lobes[lobe].neurons[i].output;
			best = static_cast<int32_t>(i);
		}
	}
	return best;
}
