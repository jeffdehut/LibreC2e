#pragma once

#include <array>
#include <stdint.h>

namespace sfc {
struct CBiochemistryV1;
struct CBrainV1;
} // namespace sfc

// Creatures 1 biochemistry: each biotick, emitters turn locus values into chemicals,
// receptors turn chemical levels into locus values, reactions convert chemicals, and every
// chemical decays at its own rate.
//
// Loci are addressed by (organ, tissue, locus), as shown in the Genetics Kit:
//   organ 0 (brain): tissue is the lobe number
//   organ 1 (creature): tissues 0 somatic, 1 circulatory, 2 reproductive, 3 immune,
//     4 sensorimotor, 5 drive levels
// This follows openc2e's existing Creatures 1 support.

// Locus values that live on the creature rather than in its brain. Receptors set most of
// them every biotick, so they don't need saving.
struct CreatureLoci {
	std::array<uint8_t, 16> drives{}; // drive levels 0-15
	std::array<uint8_t, 8> involuntary{}; // sensorimotor receptors 0-7
	std::array<uint8_t, 8> gait{}; // sensorimotor receptors 8-15
	std::array<uint8_t, 6> senses{0, 0, 0, 0, 128, 0}; // sensorimotor emitters 0-5; light starts at 128
	std::array<uint8_t, 7> life_stage{}; // somatic receptors 0-6
	uint8_t muscle_energy = 0; // somatic emitter 0
	std::array<uint8_t, 9> floating{}; // circulatory 0-8
	// reproductive loci (batch 2, Q21): receptors set fertility and the chance of conceiving;
	// emitters read 255 while fertile or pregnant
	uint8_t fertility = 0; // receptor 0
	uint8_t conception_chance = 0; // receptor 1
	uint8_t fertile_output = 0; // emitter 0
	uint8_t pregnant_output = 0; // emitter 1
};

// The locus a receptor writes or an emitter reads, or nullptr if there isn't one. `dead` is
// the creature's dead flag (immune locus 0).
uint8_t* creature_locus(CreatureLoci& loci, sfc::CBrainV1* brain, uint8_t& dead, bool receptor, uint8_t organ, uint8_t tissue, uint8_t locus);

void creature_biochemistry_tick(sfc::CBiochemistryV1& biochemistry, CreatureLoci& loci, sfc::CBrainV1* brain, uint8_t& dead, uint32_t biotick);

// Index of the strongest drive (DRV! in scripts).
int32_t creature_highest_drive(const CreatureLoci& loci);
