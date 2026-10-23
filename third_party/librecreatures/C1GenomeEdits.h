// Adapted from h4rm0n1c/LibreCreatures (GNU GPL version 3).
// b775a03, src/kits/c1kitlib/include/c1kit/genome_edits.hpp and hatchery.hpp
// (append_colour_genes). See NOTICE.md and LICENSE.
//
// The Hatchery Designer's whole-genome changes, made before an egg is laid. Each finds the
// genes it changes by what they do (chemicals, loci), not by gene number, so it works on any
// genome, and says how many genes it changed. LibreCreatures took them from its Libre Set,
// which took them from Creature Labs' Forest norns, Feliz Norns (MuppetBoy) and Jessica's
// Discover Albia.
//
// A genome here is its gene records as stored: "gene", then family, subtype, number,
// generation, switch-on stage and flags (one byte each), then the gene's own bytes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

namespace openc1::librecreatures {

using GeneRecord = std::vector<std::uint8_t>;

constexpr std::size_t GENE_HEADER = 10;
constexpr std::uint8_t GENE_BRAIN = 0, GENE_BIOCHEMISTRY = 1, GENE_CREATURE = 2;
constexpr std::uint8_t GENE_MUTABLE = 0x01, GENE_DUPLICABLE = 0x02, GENE_DELETABLE = 0x04;

inline std::uint8_t gene_family(const GeneRecord& g) { return g.size() > 4 ? g[4] : 0xff; }
inline std::uint8_t gene_subtype(const GeneRecord& g) { return g.size() > 5 ? g[5] : 0xff; }
inline std::uint8_t gene_number(const GeneRecord& g) { return g.size() > 6 ? g[6] : 0; }
inline std::uint8_t& gene_flags(GeneRecord& g) { return g[9]; }
inline std::size_t payload_size(const GeneRecord& g) { return g.size() > GENE_HEADER ? g.size() - GENE_HEADER : 0; }
inline std::uint8_t& payload(GeneRecord& g, std::size_t i) { return g[GENE_HEADER + i]; }
inline std::uint8_t payload(const GeneRecord& g, std::size_t i) { return g[GENE_HEADER + i]; }

inline bool is_receptor(const GeneRecord& g) { return gene_family(g) == GENE_BIOCHEMISTRY && gene_subtype(g) == 0 && payload_size(g) >= 8; }
inline bool is_emitter(const GeneRecord& g) { return gene_family(g) == GENE_BIOCHEMISTRY && gene_subtype(g) == 1 && payload_size(g) >= 8; }
inline bool is_reaction(const GeneRecord& g) { return gene_family(g) == GENE_BIOCHEMISTRY && gene_subtype(g) == 2 && payload_size(g) >= 9; }
inline bool is_initial_concentration(const GeneRecord& g) { return gene_family(g) == GENE_BIOCHEMISTRY && gene_subtype(g) == 4 && payload_size(g) >= 2; }

inline GeneRecord make_gene(std::uint8_t family, std::uint8_t subtype, std::uint8_t number, std::uint8_t flags, const std::vector<std::uint8_t>& bytes) {
    GeneRecord g{'g', 'e', 'n', 'e', family, subtype, number, 0, 0, flags};
    g.insert(g.end(), bytes.begin(), bytes.end());
    return g;
}

inline std::uint8_t next_gene_number(const std::vector<GeneRecord>& genes) {
    std::uint8_t next = 0;
    for (const auto& g : genes) {
        if (gene_number(g) >= next) next = static_cast<std::uint8_t>(gene_number(g) + 1);
    }
    return next;
}

// chemical numbers
constexpr std::uint8_t NEED_FOR_PLEASURE_INCREASE = 18, HUNGER_INCREASE = 19, TIREDNESS_INCREASE = 22,
    SLEEPINESS_INCREASE = 23, BOREDOM_INCREASE = 27, SEX_DRIVE_INCREASE = 29, REWARD = 49, PUNISHMENT = 50,
    LAST_REINFORCER = 55, AGEING = 56, GLUCOSE = 58, GLYCOGEN = 59, FIRST_ANTIBODY = 240;

// "Eat better". The reactions that raise Need for Pleasure, Hunger, Tiredness, Sleepiness,
// Boredom and Sex Drive (X Increase -> X + Punishment) punish whatever the creature is doing
// while the drive rises, eating included, so it learns to avoid what keeps it alive. Their
// Punishment goes; the other drives' raisers keep theirs.
inline int remove_drive_raise_punishment(std::vector<GeneRecord>& genes) {
    const std::uint8_t raisers[] = {NEED_FOR_PLEASURE_INCREASE, HUNGER_INCREASE, TIREDNESS_INCREASE,
                                    SLEEPINESS_INCREASE, BOREDOM_INCREASE, SEX_DRIVE_INCREASE};
    int changed = 0;
    for (auto& g : genes) {
        if (!is_reaction(g)) continue;
        bool raiser = false;
        for (const std::uint8_t chemical : raisers) {
            raiser = raiser || (payload(g, 1) == chemical && payload(g, 0) != 0) || (payload(g, 3) == chemical && payload(g, 2) != 0);
        }
        if (!raiser) continue;
        bool edited = false;
        for (const std::size_t product : {std::size_t{5}, std::size_t{7}}) {
            if (payload(g, product) == PUNISHMENT) {
                payload(g, product) = 0;
                payload(g, product - 1) = 0;
                edited = true;
            }
        }
        changed += edited ? 1 : 0;
    }
    return changed;
}

// "No death from old age". Removes the receptors that end life by age: Ageing on the last
// life stage, or on the death locus. The stock genomes have none; some later ones do.
inline int remove_old_age_death(std::vector<GeneRecord>& genes) {
    int removed = 0;
    for (std::size_t i = 0; i < genes.size();) {
        const auto& g = genes[i];
        if (is_receptor(g) && payload(g, 3) == AGEING && (payload(g, 0) & 1) == 1
            && ((payload(g, 1) == 0 && payload(g, 2) == 6) || (payload(g, 1) == 3 && payload(g, 2) == 0))) {
            genes.erase(genes.begin() + static_cast<std::ptrdiff_t>(i));
            ++removed;
        } else {
            ++i;
        }
    }
    return removed;
}

// "Lock learning genes". The brain lobes, every receptor and emitter wired to a brain locus,
// and every reaction that makes or uses a reinforcer (Reward to Punishment Echo) can no
// longer mutate, duplicate or be deleted, so offspring keep a brain that learns.
inline int lock_learning_genes(std::vector<GeneRecord>& genes) {
    constexpr std::uint8_t locks = GENE_MUTABLE | GENE_DUPLICABLE | GENE_DELETABLE;
    int changed = 0;
    for (auto& g : genes) {
        if (g.size() < GENE_HEADER) continue;
        bool learning = gene_family(g) == GENE_BRAIN;
        if ((is_receptor(g) || is_emitter(g)) && (payload(g, 0) & 1) == 0) learning = true;
        if (is_reaction(g)) {
            for (const std::size_t chemical : {1, 3, 5, 7}) {
                const std::uint8_t c = payload(g, chemical);
                learning = learning || (c >= REWARD && c <= LAST_REINFORCER);
            }
        }
        if (learning && (gene_flags(g) & locks) != 0) {
            gene_flags(g) = static_cast<std::uint8_t>(gene_flags(g) & ~locks);
            ++changed;
        }
    }
    return changed;
}

// "Hardier babies". A newborn starts with at least 160 Glycogen and 190 Glucose (stock: 122
// and 159), and with 130 of every antibody it has none of, so it starts immune to every
// antigen. New genes take the next free numbers.
inline int harden_newborns(std::vector<GeneRecord>& genes) {
    int changed = 0;
    bool antibody_present[8] = {};
    for (auto& g : genes) {
        if (!is_initial_concentration(g)) continue;
        const std::uint8_t chemical = payload(g, 0);
        const std::uint8_t floor = chemical == GLYCOGEN ? 160 : chemical == GLUCOSE ? 190 : 0;
        if (floor != 0 && payload(g, 1) < floor) {
            payload(g, 1) = floor;
            ++changed;
        }
        if (chemical >= FIRST_ANTIBODY && chemical < FIRST_ANTIBODY + 8) antibody_present[chemical - FIRST_ANTIBODY] = true;
    }
    std::uint8_t next = next_gene_number(genes);
    for (int antibody = 0; antibody < 8; ++antibody) {
        if (antibody_present[antibody]) continue;
        genes.push_back(make_gene(GENE_BIOCHEMISTRY, 4, next++, GENE_MUTABLE, {static_cast<std::uint8_t>(FIRST_ANTIBODY + antibody), 130}));
        ++changed;
    }
    return changed;
}

// The colour added: a tint per pigment channel (red, green, blue; 128 is none), and the
// pigment extension's hue rotation and colour swap (128 is none for both).
struct EggColours {
    std::uint8_t tint[3] = {0x80, 0x80, 0x80};
    std::uint8_t hue_rotation = 0x80;
    std::uint8_t colour_swap = 0x80;
};

// Pigment genes added per channel. A channel's pigment genes are averaged, so four beside a
// stock genome's few give the tint about half the say; each extension is averaged in turn into
// the one before, so the last ones (these) set the hue and swap.
constexpr int COLOUR_GENES_PER_CHANNEL = 4;

// Adds the pigment genes, each with its extension: a creature-family subtype-6 gene of channel
// and amount, then "gext", hue rotation and colour swap. They are mutable only: a duplicable
// gene can make crossover copy whole blocks again (genomes grow each generation), and a
// deletable one lets the colour drop out.
inline void append_colour_genes(std::vector<GeneRecord>& genes, const EggColours& colours) {
    std::uint8_t next = next_gene_number(genes);
    for (int copy = 0; copy < COLOUR_GENES_PER_CHANNEL; ++copy) {
        for (std::uint8_t channel = 0; channel < 3; ++channel) {
            genes.push_back(make_gene(GENE_CREATURE, 6, next++, GENE_MUTABLE,
                {channel, colours.tint[channel], 'g', 'e', 'x', 't', colours.hue_rotation, colours.colour_swap}));
        }
    }
}

struct DesignerChoices {
    bool eat_better = false;
    bool no_old_age_death = false;
    bool lock_learning = false;
    bool hardier_babies = false;
};

// Applies the chosen changes; returns what changed, for the player
inline std::string apply_designer_choices(std::vector<GeneRecord>& genes, const DesignerChoices& choices) {
    std::string text;
    const auto note = [&text](const char* what, int count) {
        text += std::string(text.empty() ? "" : "; ") + what + " " + std::to_string(count);
    };
    if (choices.eat_better) note("drive raisers no longer punishing:", remove_drive_raise_punishment(genes));
    if (choices.no_old_age_death) note("old-age death receptors removed:", remove_old_age_death(genes));
    if (choices.lock_learning) note("learning genes locked:", lock_learning_genes(genes));
    if (choices.hardier_babies) note("newborn genes hardened:", harden_newborns(genes));
    return text;
}

} // namespace openc1::librecreatures
