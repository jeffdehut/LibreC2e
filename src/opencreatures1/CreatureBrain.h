#pragma once

#include <array>
#include <stdint.h>
#include <vector>

namespace sfc {
struct CBrainV1;
struct NeuronV1;
} // namespace sfc

// The Creatures 1 brain, running on the brain saved in the world file (lobes, neurons and
// their dendrites). Behaviour follows the clean-room specs in `Clean Specs/BATCH1-ANSWERS.md`
// (questions 13-19 and 22).
//
// Lobes in a Creatures 1 brain:
//   0 perception (copies of the perceptible lobes among 1-5), 1 drives, 2 stimulus source,
//   3 verbs, 4 nouns, 5 general senses, 6 decisions, 7 attention, 8 concepts
enum CreatureBrainLobe {
	LOBE_PERCEPTION = 0,
	LOBE_DRIVE = 1,
	LOBE_STIMULUS_SOURCE = 2,
	LOBE_VERB = 3,
	LOBE_NOUN = 4,
	LOBE_GENERAL_SENSE = 5,
	LOBE_DECISION = 6,
	LOBE_ATTENTION = 7,
	LOBE_CONCEPT = 8,
};

// Not saved: where each lobe's "whole set" dendrite migration (flag 2) has got to.
struct BrainMigrationState {
	std::vector<std::array<uint32_t, 2>> next_neuron;
};

// A neuron of a lobe, or nullptr if the lobe or neuron doesn't exist.
sfc::NeuronV1* brain_neuron(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron);

// Inputs from the world: set a neuron's state, or add to it (capped at 255).
void brain_set_input(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron, uint8_t value);
void brain_add_input(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron, int32_t value);
// Sets every neuron of a lobe, or one neuron, to zero (state and output).
void brain_clear_lobe(sfc::CBrainV1& brain, uint32_t lobe);
void brain_clear_neuron(sfc::CBrainV1& brain, uint32_t lobe, uint32_t neuron);

// One brain step. `step` is the creature's brain-step counter, which also times decay.
void brain_tick(sfc::CBrainV1& brain, uint32_t step, BrainMigrationState& migration);

// The neuron with the highest output (ties to the lower number), or -1 if none fire.
int32_t brain_winner(const sfc::CBrainV1& brain, uint32_t lobe);
