#include "CreatureBiochemistry.h"

#include "Biochemistry.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"

#include <algorithm>
#include <fmt/core.h>

static sfc::LobeV1* lobe_for_tissue(sfc::CBrainV1* brain, uint8_t tissue, size_t* first_neuron) {
	if (!brain || tissue >= brain->lobes.size()) {
		return nullptr;
	}
	size_t first = 0;
	for (size_t i = 0; i < tissue; ++i) {
		first += brain->lobes[i].num_neurons;
	}
	*first_neuron = first;
	return &brain->lobes[tissue];
}

static uint8_t* brain_locus(sfc::CBrainV1* brain, bool receptor, uint8_t tissue, uint8_t locus) {
	size_t first_neuron = 0;
	sfc::LobeV1* lobe = lobe_for_tissue(brain, tissue, &first_neuron);
	if (!lobe) {
		return nullptr;
	}
	auto neuron = [&](size_t cell) -> sfc::NeuronV1* {
		if (cell >= lobe->num_neurons || first_neuron + cell >= brain->neurons.size()) {
			return nullptr;
		}
		return &brain->neurons[first_neuron + cell];
	};

	if (receptor) {
		switch (locus) {
			case 0: return &lobe->threshold;
			case 1: return &lobe->leakage;
			case 2: return &lobe->reststate;
			default: break;
		}
		if (locus < 13) {
			// Source-informed C1 locus mapping from LibreCreatures lobe.cpp.
			// Chemistry can regulate both connection types' learning/pruning rates.
			auto& type = locus < 8 ? lobe->d0_type : lobe->d1_type;
			switch ((locus - 3) % 5) {
				case 0: return &type.relax_suscept;
				case 1: return &type.relax_stw;
				case 2: return &type.ltw_gain_rate;
				case 3: return &type.gain_rate;
				case 4: return &type.lose_rate;
			}
		}
		if (locus < 17) {
			return &lobe->chemicals[locus - 13];
		}
		if (locus > 32) return nullptr;
		auto* n = neuron(locus - 17u);
		return n ? &n->state : nullptr;
	}

	switch (locus) {
		case 0: return &lobe->activity;
		case 1:
		case 2:
			return &lobe->loose_dendrites[locus - 1];
		default: {
			if (locus > 18) return nullptr;
			auto* n = neuron(locus - 3u);
			return n ? &n->output : nullptr;
		}
	}
}

uint8_t* creature_locus(CreatureLoci& loci, sfc::CBrainV1* brain, uint8_t& dead, bool receptor, uint8_t organ, uint8_t tissue, uint8_t locus) {
	if (organ == 0) {
		return brain_locus(brain, receptor, tissue, locus);
	}
	if (organ != 1) {
		return nullptr;
	}
	switch (tissue) {
		case 0: // somatic
			if (receptor) {
				return locus < loci.life_stage.size() ? &loci.life_stage[locus] : nullptr;
			}
			return locus == 0 ? &loci.muscle_energy : nullptr;
		case 1: // circulatory
			return locus < loci.floating.size() ? &loci.floating[locus] : nullptr;
		case 2: // reproductive
			if (locus == 0) {
				return receptor ? &loci.fertility : &loci.fertile_output;
			}
			if (locus == 1) {
				return receptor ? &loci.conception_chance : &loci.pregnant_output;
			}
			return nullptr;
		case 3: // immune
			return locus == 0 ? &dead : nullptr;
		case 4: // sensorimotor
			if (receptor) {
				if (locus < 8) {
					return &loci.involuntary[locus];
				}
				return locus < 16 ? &loci.gait[locus - 8u] : nullptr;
			}
			return locus < loci.senses.size() ? &loci.senses[locus] : nullptr;
		case 5: // drive levels
			return locus < loci.drives.size() ? &loci.drives[locus] : nullptr;
		default:
			return nullptr;
	}
}

static void add_chemical(sfc::CBiochemistryV1& b, uint8_t chem, int32_t amount) {
	if (chem == 0) {
		return; // chemical 0 is "none"
	}
	b.chemicals[chem].concentration = static_cast<uint8_t>(std::clamp(b.chemicals[chem].concentration + amount, 0, 255));
}

void creature_biochemistry_tick(sfc::CBiochemistryV1& b, CreatureLoci& loci, sfc::CBrainV1* brain, uint8_t& dead, uint32_t biotick) {
	// emitters: effect bit 1 clears the locus after reading, 2 is digital, 4 inverts
	for (const auto& e : b.emitters) {
		if (e.rate != 0 && biotick % e.rate != 0) {
			continue;
		}
		uint8_t* locus = creature_locus(loci, brain, dead, false, e.organ, e.tissue, e.locus);
		if (!locus) {
			continue;
		}
		int32_t value = *locus;
		if (e.effect & 4) {
			value = 255 - value;
		}
		// Source-informed C1 admission: a clear-on-emission source is only cleared
		// after an eligible emission, never merely because its period was examined.
		if (value == 0 || value <= e.threshold) continue;
		if (e.effect & 2) {
			// Batch 7 Q4: an inactive pregnancy/fertility locus at zero must
			// not emit hormones when the digital threshold is zero.
			if (value > e.threshold) {
				add_chemical(b, e.chemical, e.gain);
			}
		} else {
			// gain x level / 256, rounded down (batch 7, Q1d)
			add_chemical(b, e.chemical, std::clamp((value - e.threshold) * e.gain / 256, 0, 255));
		}
		if (e.effect & 1) *locus = 0;
	}

	// receptors: effect bit 1 inverts, 2 is digital
	for (const auto& r : b.receptors) {
		uint8_t* locus = creature_locus(loci, brain, dead, true, r.organ, r.tissue, r.locus);
		if (!locus || r.chemical == 0) {
			continue;
		}
		// Source-informed C1 receptor arithmetic: only positive excess contributes,
		// analogue gain divides by 256; below threshold the nominal value remains.
		const int32_t level = b.chemicals[r.chemical].concentration;
		const int32_t excess = std::max(0, level - r.threshold);
		int32_t value = excess == 0 ? 0 : (r.effect & 2) ? r.gain : excess * r.gain / 256;
		value = (r.effect & 1) ? r.nominal - value : r.nominal + value;
		value = std::clamp(value, 0, 255);
		// the "die if non-zero" locus never brings a creature back to life
		if (locus == &dead && value == 0) {
			continue;
		}
		if (locus == &dead && !dead) {
			// say which chemical killed it (for the wolfling run's causes of death)
			fmt::print("INFO [Death] the death receptor fired: chemical {} at {} (threshold {}, effect {})\n", r.chemical, level, r.threshold, r.effect);
		}
		*locus = static_cast<uint8_t>(value);
	}

	// reactions and decay share their rate rules with the existing Biochemistry code
	ChemicalStates chemicals;
	for (size_t i = 0; i < chemicals.size(); ++i) {
		chemicals[i] = {b.chemicals[i].concentration, b.chemicals[i].decay};
	}
	for (const auto& r : b.reactions) {
		ChemicalReaction rxn{r.r1_amount, r.r1_chem, r.r2_amount, r.r2_chem, r.rate, r.p1_amount, r.p1_chem, r.p2_amount, r.p2_chem};
		run_chemical_reaction(chemicals, rxn, biotick);
	}
	// chemical 0 is "none" and must stay empty
	chemicals[0].concentration = 0;
	for (size_t i = 0; i < chemicals.size(); ++i) {
		b.chemicals[i].concentration = decay_chemical(chemicals[i].concentration, chemicals[i].decay, biotick);
	}
}

int32_t creature_highest_drive(const CreatureLoci& loci) {
	return static_cast<int32_t>(std::max_element(loci.drives.begin(), loci.drives.end()) - loci.drives.begin());
}
