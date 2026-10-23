#pragma once

#include "objects/ObjectHandle.h"

#include <array>
#include <memory>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

class genomeFile;
struct Creature;
namespace sfc {
struct CBrainV1;
}

// Creatures 1 genetics, following the clean-room specs in Clean Specs/BATCH2-ANSWERS.md
// (questions 17-21).

// Monikers as script integers: four characters, little-endian (TOKN).
int32_t moniker_to_integer(const std::string& moniker);
std::string moniker_from_integer(int32_t value);

// Genomes live in Genetics/<moniker>.gen.
std::shared_ptr<genomeFile> load_genome(const std::string& moniker);
// Raw gene records ("gene" ... ) as stored in a Creatures 1 genome file.
std::vector<std::vector<uint8_t>> genome_gene_bytes(const std::string& moniker);
void save_genome_bytes(const std::string& moniker, const std::vector<std::vector<uint8_t>>& genes);

// A new random moniker (a digit and three letters) that no creature or genome uses yet.
std::string new_moniker();

// Makes a child genome from two parents (b may be empty for a straight copy) and saves it.
// Returns the child's moniker, or an empty string if a parent's genome is missing (Q21).
std::string make_child_genome(const std::string& a, const std::string& b);
// The parents of a genome make_child_genome made since the program started (mother, then
// father, the father empty for a copy), or nullptr: the egg archive files eggs by them.
const std::pair<std::string, std::string>* genome_parents(const std::string& child);

// Whether a gene applies to a creature of this sex (flag 8 males only, 16 females only).
bool gene_applies(uint8_t flags, uint8_t sex);

// Builds a newborn's brain from its lobe genes (Q19).
std::shared_ptr<sfc::CBrainV1> build_brain(genomeFile& genome, uint8_t sex);

// Expresses the genes for a life stage: biochemistry, stimuli, poses, gaits and instincts
// that switch on at this stage, and the appearance from every appearance gene (Q17, Q20).
void express_genes(Creature& creature, genomeFile& genome, int32_t stage);
// puts back the receptors and emitters the creature's genes switch on by its life stage, if
// missing (worlds saved before a gene-expression fix)
void restore_missing_genes(Creature& c, genomeFile& genome);
// gives a creature that has never learned about food the memory that food and plants lower
// hunger (not in the original; newborns starved otherwise)
void seed_food_memory(Creature& c);

// The four appearance breeds (head, body, legs, arms) from a genome, or -1 where there's no gene.
std::array<int32_t, 4> appearance_breeds(genomeFile& genome, uint8_t sex);

// Builds or rebuilds the creature's body for its sex, age and breeds: the joined sprite file
// and each part's body data from Body Data/*.att.
void build_body(Creature& creature, const std::array<int32_t, 4>& breeds);

// NEW: CREA (Q18): a new creature from a genome; sex 0 picks one to balance the population.
ObjectHandle create_creature(const std::string& moniker, int32_t sex);

// Averaged C1 RGB, hue rotation and colour swap controls (128 is neutral).
std::array<uint8_t, 5> creature_palette_controls(genomeFile& genome, uint8_t sex);
