#include "CreatureBrain.h"

#include "Biochemistry.h"
#include "common/Random.h"
#include "fileformats/sfc/CBrain.h"

#include <algorithm>
#include <set>

namespace {

// SVRule tokens (specs Q14, Q19). Tokens 1 to 21 read the rule's registers (below); 22 to 27
// are operators; 28 and 29 do nothing. A genome's bytes are folded into 0-29 as a lobe grows.
enum SVRuleToken : uint8_t {
	TOKEN_END = 0,
	TOKEN_TRUE = 22,
	TOKEN_PLUS = 23,
	TOKEN_MINUS = 24,
	TOKEN_TIMES = 25,
	TOKEN_INCR = 26,
	TOKEN_DECR = 27,
	TOKEN_COUNT = 30,
};

// What a rule's tokens 1 to 21 read, as the original keeps them: constants (1-4: 0, 1, 64,
// 255), the lobe's four chemicals (5-8), the neuron (9 state, 10 output, 11 threshold), its two
// dendrite sums and their "anded" forms (12-15), and the dendrite being updated (16 its
// source's output, 17 never set, 18 susceptibility, 19 STW, 20 LTW, 21 strength). The
// registers are filled in as a lobe updates and not cleared in between, so a rule sees what
// the last neuron or dendrite left there; a dendrite's rules see the neuron's new state from
// its rule (before it relaxes), its output from before, and its sums from before the rule.
using Registers = std::array<uint8_t, 22>;

constexpr size_t FIRST_RULE_LOBE = 6; // lobes 0-5 take input and never run rules (Q15)
constexpr uint32_t NO_CANDIDATE = 0xffffffffu;
constexpr uint32_t SEARCH_FROM_TOP = 99999;

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

// a random byte from min to max (a world from before the ranges were put right can still have
// them the wrong way round)
uint8_t random_between(uint8_t a, uint8_t b) {
	return rand_uint8(std::min(a, b), std::max(a, b));
}

// Evaluates a 10-byte SVRule on the registers. Sums saturate; TRUE stops the rule when the
// accumulator is 0; an operator's operand is the next token's register (0 for an operator).
uint8_t evaluate(const std::array<uint8_t, 10>& rule, const Registers& r) {
	uint8_t acc = 0;
	size_t i = 0;
	while (i < rule.size()) {
		uint8_t token = rule[i];
		if (token >= TOKEN_COUNT) {
			token = static_cast<uint8_t>(token % TOKEN_COUNT); // as a growing lobe folds them
		}
		if (token == TOKEN_END) {
			break;
		}
		if (token < TOKEN_TRUE) {
			acc = r[token];
			++i;
			continue;
		}
		switch (token) {
			case TOKEN_TRUE:
				if (acc == 0) {
					return acc;
				}
				++i;
				break;
			case TOKEN_PLUS:
			case TOKEN_MINUS:
			case TOKEN_TIMES: {
				if (i + 1 >= rule.size()) {
					return acc;
				}
				uint8_t next = rule[i + 1];
				if (next >= TOKEN_COUNT) {
					next = static_cast<uint8_t>(next % TOKEN_COUNT);
				}
				const uint8_t operand = next < TOKEN_TRUE ? r[next] : 0;
				if (token == TOKEN_PLUS) {
					acc = static_cast<uint8_t>(std::min(255, acc + operand));
				} else if (token == TOKEN_MINUS) {
					acc = acc < operand ? 0 : static_cast<uint8_t>(acc - operand);
				} else {
					acc = static_cast<uint8_t>((acc * operand) >> 8);
				}
				i += 2;
				break;
			}
			case TOKEN_INCR:
				if (acc != 255) {
					++acc;
				}
				++i;
				break;
			case TOKEN_DECR:
				if (acc != 0) {
					--acc;
				}
				++i;
				break;
			default:
				++i; // 28 and 29 do nothing
				break;
		}
	}
	return acc;
}

std::vector<uint32_t>& active_of(BrainMigrationState& rt, size_t lobe) {
	if (rt.active.size() <= lobe) {
		rt.active.resize(lobe + 1);
	}
	return rt.active[lobe];
}

uint32_t& candidate_of(BrainMigrationState& rt, size_t lobe, int type) {
	if (rt.candidate.size() <= lobe) {
		rt.candidate.resize(lobe + 1, {SEARCH_FROM_TOP, SEARCH_FROM_TOP});
	}
	return rt.candidate[lobe][static_cast<size_t>(type)];
}

void winner_takes_all(LobeView& l, const std::vector<uint32_t>& active) {
	if (active.empty() || !(l.lobe->flags & 1)) {
		return;
	}
	// the first of the strongest that isn't kept out of it; every other output goes
	sfc::NeuronV1* winner = nullptr;
	uint8_t strongest = 0;
	for (size_t i = 0; i < l.count; ++i) {
		auto& n = l.neurons[i];
		if (!n.wta_disable && strongest < n.output) {
			winner = &n;
			strongest = n.output;
		}
		n.output = 0;
	}
	if (winner) {
		winner->output = strongest;
	}
}

uint8_t activity(size_t active, size_t count) {
	return count ? static_cast<uint8_t>(std::min<size_t>((active << 8) / count, 255)) : 0;
}

// An input lobe (0-5): output is how far its state is above the threshold, then the state
// relaxes towards rest.
void early_phase(LobeView& l, size_t index, uint32_t tick, BrainMigrationState& rt) {
	auto& lobe = *l.lobe;
	auto& active = active_of(rt, index);
	active.clear();
	for (size_t i = 0; i < l.count; ++i) {
		auto& n = l.neurons[i];
		const int strength = static_cast<int>(n.state) - static_cast<int>(lobe.threshold);
		if (strength < 1) {
			n.output = 0;
		} else {
			n.output = static_cast<uint8_t>(strength);
			active.push_back(static_cast<uint32_t>(i));
		}
		n.state = relax(n.state, lobe.reststate, lobe.leakage, tick);
	}
	lobe.activity = activity(active.size(), l.count);
	winner_takes_all(l, active);
}

// Lobe 0 neurons copied from a lobe whose perceptible value is 2 are marked with that lobe's
// number: no two dendrites of a rewired set may come from the same such lobe (batch 2, Q7).
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

// Migration flag 2: rewires all of a neuron's dendrites of a type to a run of the source
// lobe's active neurons, shuffled, avoiding runs another active neuron of the lobe is already
// wired to; if none will do, the dendrites keep their sources with nothing learned.
void migrate_set(BrainView& b, LobeView& l, size_t index, uint32_t neuron, int type, BrainMigrationState& rt, std::array<uint8_t, 2>& loose) {
	const auto& info = dendrite_type(l, type);
	if (info.source_lobe >= b.lobes.size() || neuron >= l.count) {
		return;
	}
	auto& dendrites = dendrites_of(l.neurons[neuron], type);
	auto& pool = active_of(rt, info.source_lobe);
	const size_t count = dendrites.size();
	if (count > pool.size()) {
		return;
	}
	std::vector<uint32_t> saved;
	for (const auto& d : dendrites) {
		saved.push_back(d.source_index);
	}
	// the shuffle is the source lobe's own list's, and stays for any later draw this step
	for (size_t i = 0; i < pool.size(); ++i) {
		std::swap(pool[i], pool[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(pool.size()) - 1))]);
	}
	const auto marks = info.source_lobe == LOBE_PERCEPTION ? perception_marks(b) : std::vector<uint32_t>{};
	const auto& own_active = active_of(rt, index);
	for (size_t start = 0; start + count <= pool.size(); ++start) {
		bool clash = false;
		for (size_t a = 0; a < count && !clash; ++a) {
			const uint32_t mark = pool[start + a] < marks.size() ? marks[pool[start + a]] : 0;
			for (size_t c = a + 1; mark && c < count && !clash; ++c) {
				clash = pool[start + c] < marks.size() && marks[pool[start + c]] == mark;
			}
		}
		if (clash) {
			continue;
		}
		for (size_t k = 0; k < count; ++k) {
			auto& d = dendrites[k];
			d.source_index = pool[start + k];
			d.ltw = d.stw = random_between(info.min_ltw, info.max_ltw);
			d.strength = random_between(info.min_strength, info.max_strength);
		}
		std::multiset<uint32_t> run;
		for (const auto& d : dendrites) {
			run.insert(d.source_index);
		}
		bool taken = false;
		for (const uint32_t other : own_active) {
			if (other == neuron || other >= l.count) {
				continue;
			}
			const auto& od = dendrites_of(l.neurons[other], type);
			if (od.size() != count) {
				continue;
			}
			std::multiset<uint32_t> theirs;
			for (const auto& d : od) {
				theirs.insert(d.source_index);
			}
			taken = theirs == run;
			if (taken) {
				break;
			}
		}
		if (taken) {
			continue;
		}
		if (loose[static_cast<size_t>(type)]) {
			--loose[static_cast<size_t>(type)];
		}
		return;
	}
	for (size_t k = 0; k < count; ++k) {
		dendrites[k].source_index = saved[k];
		dendrites[k].stw = dendrites[k].ltw = dendrites[k].strength = 0;
	}
}

// A lobe that runs rules (6 and up): each neuron's state from its rule, then its dendrites
// learn, weaken and grow, then winner-takes-all and migration.
void late_phase(BrainView& b, LobeView& l, size_t index, uint32_t tick, BrainMigrationState& rt) {
	auto& lobe = *l.lobe;
	Registers r{};
	r[2] = 1;
	r[3] = 64;
	r[4] = 255;
	for (size_t c = 0; c < 4; ++c) {
		r[5 + c] = lobe.chemicals[c];
	}
	auto& active = active_of(rt, index);
	active.clear();
	std::array<uint8_t, 2> loose{};
	std::array<uint32_t, 2> found{NO_CANDIDATE, NO_CANDIDATE};
	for (int type = 0; type < 2; ++type) {
		loose[static_cast<size_t>(type)] = dendrite_type(l, type).migration == 1 ? 255 : 0;
	}
	for (size_t i = 0; i < l.count; ++i) {
		auto& n = l.neurons[i];
		r[9] = n.state;
		r[10] = n.output;
		r[11] = lobe.threshold;
		r[12] = r[13] = r[14] = r[15] = 0;
		// each dendrite type's sum of its sources' outputs times STW / 256, at most 255, times
		// the input gain / 256; "anded", the same if every source fires and 0 if not (batch 8)
		for (int type = 0; type < 2; ++type) {
			const auto& dendrites = dendrites_of(n, type);
			if (dendrites.empty()) {
				continue;
			}
			const auto& info = dendrite_type(l, type);
			int sum = 0;
			uint8_t all_firing = 0xff;
			for (const auto& d : dendrites) {
				const auto* src = b.source(info.source_lobe, d.source_index);
				const uint8_t output = src ? src->output : 0;
				if (output == 0) {
					all_firing = 0;
				} else {
					sum += (d.stw * output) >> 8;
				}
			}
			const uint8_t scaled = static_cast<uint8_t>((lobe.inputgain * std::clamp(sum, 0, 255)) >> 8);
			r[12 + static_cast<size_t>(type)] = scaled;
			r[14 + static_cast<size_t>(type)] = static_cast<uint8_t>(all_firing & scaled);
		}
		r[9] = evaluate(lobe.svrule, r);
		n.state = r[9];
		const int strength = static_cast<int>(r[9]) - static_cast<int>(lobe.threshold);
		if (strength < 1) {
			n.output = 0;
		} else {
			n.output = static_cast<uint8_t>(strength);
			active.push_back(static_cast<uint32_t>(i));
		}
		n.state = relax(n.state, lobe.reststate, lobe.leakage, tick);

		for (int type = 0; type < 2; ++type) {
			const auto& info = dendrite_type(l, type);
			int loose_here = 0;
			for (auto& d : dendrites_of(n, type)) {
				const auto* src = b.source(info.source_lobe, d.source_index);
				r[16] = src ? src->output : 0;
				r[18] = d.suscept;
				r[19] = d.stw;
				r[20] = d.ltw;
				r[21] = d.strength;
				if (d.strength == 0) {
					++loose_here;
				}
				// susceptibility rises an eighth of the way to its rule's result, or decays
				const uint8_t signal = evaluate(info.suscept_svrule, r);
				if (d.suscept < signal) {
					d.suscept = static_cast<uint8_t>(d.suscept + ((signal - d.suscept) >> 3));
				} else if (d.suscept != 0) {
					d.suscept = decay_chemical(d.suscept, info.relax_suscept, tick);
				}
				// reinforcement raises STW to LTW plus the reinforcement (spec Q17); a rise past
				// what's there is taken as a byte, as the original does
				if (d.suscept != 0) {
					const uint32_t weighted = (static_cast<uint32_t>(d.suscept) * evaluate(info.reinforce_svrule, r)) >> 8;
					const int headroom = static_cast<int>(d.stw) - static_cast<int>(d.ltw);
					if (weighted != 0 && headroom < static_cast<int>(weighted)) {
						const uint32_t raised = d.stw + static_cast<uint8_t>(static_cast<int>(weighted) - headroom);
						d.stw = static_cast<uint8_t>(std::min<uint32_t>(raised, 255));
					}
				}
				// STW relaxes towards LTW, and LTW follows STW a step at a time
				if (d.stw != d.ltw) {
					d.stw = relax(d.stw, d.ltw, info.relax_stw, tick);
					if (d.stw != d.ltw && info.ltw_gain_rate && tick % info.ltw_gain_rate == 0) {
						d.ltw = d.ltw < d.stw ? static_cast<uint8_t>(d.ltw + 1) : static_cast<uint8_t>(d.ltw - 1);
					}
				}
				// strength rises to the gain rule's result, and drops by 1 while the loss rule holds
				if (info.gain_rate && d.strength != 255 && tick % info.gain_rate == 0) {
					const uint8_t growth = evaluate(info.gain_svrule, r);
					if (d.strength < growth) {
						d.strength = growth;
					}
				}
				if (info.lose_rate && d.strength != 0 && tick % info.lose_rate == 0 && evaluate(info.lose_svrule, r) != 0) {
					if (--d.strength == 0) {
						// loose now: it has learned nothing, and the neuron stops
						++loose_here;
						d.suscept = d.stw = d.ltw = 0;
						n.output = 0;
						n.state = 0;
					}
				}
			}
			auto& count = loose[static_cast<size_t>(type)];
			if (info.migration == 1) {
				count = static_cast<uint8_t>(std::min<int>(count, loose_here));
			} else if (loose_here != 0) {
				if (count != 255) {
					++count;
				}
				// the highest neuron with loose dendrites below where the last rewiring was
				if (i < candidate_of(rt, index, type)) {
					found[static_cast<size_t>(type)] = static_cast<uint32_t>(i);
				}
			}
		}
	}
	lobe.activity = activity(active.size(), l.count);
	winner_takes_all(l, active);

	// Migration flag 1: a firing neuron moves its first loose dendrite to a random neuron that
	// fired in the source lobe, unless another of its dendrites already has that one (spec Q18)
	for (int type = 0; type < 2; ++type) {
		const auto& info = dendrite_type(l, type);
		if (info.migration != 1 || info.source_lobe >= b.lobes.size()) {
			continue;
		}
		const auto& pool = active_of(rt, info.source_lobe);
		if (pool.empty()) {
			continue;
		}
		for (size_t i = 0; i < l.count; ++i) {
			auto& n = l.neurons[i];
			auto& dendrites = dendrites_of(n, type);
			if (dendrites.empty() || n.output == 0) {
				continue;
			}
			auto loose_one = std::find_if(dendrites.begin(), dendrites.end(), [](auto& d) { return d.strength == 0; });
			if (loose_one == dendrites.end()) {
				continue;
			}
			const uint32_t chosen = pool[static_cast<size_t>(rand_int32(0, static_cast<int32_t>(pool.size()) - 1))];
			const bool already = std::any_of(dendrites.begin(), dendrites.end(), [&](auto& d) { return &d != &*loose_one && d.strength != 0 && d.source_index == chosen; });
			if (already) {
				continue;
			}
			loose_one->source_index = chosen;
			loose_one->ltw = loose_one->stw = random_between(info.min_ltw, info.max_ltw);
			loose_one->strength = random_between(info.min_strength, info.max_strength);
			if (loose[static_cast<size_t>(type)]) {
				--loose[static_cast<size_t>(type)];
			}
		}
	}

	// Migration flag 2: one neuron per type a step (and its other type too, if that rewires
	// whole sets as well)
	for (int type = 0; type < 2; ++type) {
		if (dendrite_type(l, type).migration != 2) {
			continue;
		}
		uint32_t& stored = candidate_of(rt, index, type);
		const uint32_t chosen = found[static_cast<size_t>(type)];
		if (chosen == NO_CANDIDATE) {
			stored = SEARCH_FROM_TOP;
			continue;
		}
		migrate_set(b, l, index, chosen, type, rt, loose);
		if (dendrite_type(l, 1 - type).migration == 2) {
			migrate_set(b, l, index, chosen, 1 - type, rt, loose);
		}
		stored = chosen;
	}
	// what the lobe's emitters read (not saved)
	lobe.loose_dendrites = loose;
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
	// Lobes 1-5 first; lobe 0 then takes the larger of its own state and the output of each
	// perceptible lobe, in lobe order (1-5 from this step, 6 and up, such as attention, from
	// the previous one), and updates; then the rest in order (batch 2 Q1, batch 8).
	for (size_t i = 1; i < std::min<size_t>(b.lobes.size(), FIRST_RULE_LOBE); ++i) {
		early_phase(b.lobes[i], i, step, migration);
	}
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
	early_phase(b.lobes[LOBE_PERCEPTION], LOBE_PERCEPTION, step, migration);
	for (size_t i = FIRST_RULE_LOBE; i < b.lobes.size(); ++i) {
		late_phase(b, b.lobes[i], i, step, migration);
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
