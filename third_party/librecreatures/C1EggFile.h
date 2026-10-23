// Adapted from h4rm0n1c/LibreCreatures (GNU GPL version 3).
// 88f9c46 (neorender), src/c1/creatures/egg.hpp, egg.cpp and genome.cpp (Genome::serialize).
// See NOTICE.md and LICENSE.
//
// An egg as a file (not in the 1996 game): LibreCreatures' File > Export Held Egg and Import
// Egg write and read these, and so does this engine, so the two can trade eggs. It holds what
// makes the egg that egg: its classifier (any egg, family 2 genus 5), the sex it hatches as
// (obv1: 1 male, 2 female, 0 chosen at hatching) and the baby's genome, whose name is the
// baby's moniker (obv0). Everything else is a fresh hatchery egg's when it's imported.
//
// The layout is fixed (all little-endian): a "CEgg" class record, the version, classifier,
// sex, the genome's byte count and CRC-32, then a "CGenome" class record with the genome's
// byte count, its four-character name, sex, life stage and the genome file itself.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace openc1::librecreatures {

struct EggFile {
    static constexpr std::uint32_t VERSION = 2;
    static constexpr std::uint32_t EGG_CLASSIFIER = 0x02050000u; // 2 5 x
    static constexpr std::uint32_t CLASSIFIER_MASK = 0xffff0000u;
    static constexpr std::size_t MAX_GENOME_BYTES = 1u << 20; // far above a Norn's ~8 KB
    static constexpr std::size_t HEADER_BYTES = 56;

    std::uint32_t classifier = 0x02050200u;
    std::uint32_t sex = 0;
    std::string moniker; // four characters, the genome's file name
    std::vector<std::uint8_t> genome; // the .gen file, "gene" ... "gend"

    static bool is_egg_classifier(std::uint32_t classifier) {
        return (classifier & CLASSIFIER_MASK) == EGG_CLASSIFIER;
    }

    // the standard CRC-32, as zlib computes it
    static std::uint32_t crc32(const std::uint8_t* bytes, std::size_t count) {
        std::uint32_t crc = 0xffffffffu;
        for (std::size_t i = 0; i < count; ++i) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
            }
        }
        return ~crc;
    }

    static bool is_moniker_character(std::uint8_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    }

    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        const auto u32 = [&out](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        };
        const std::uint8_t egg_class[] = {0xff, 0xff, 0x01, 0x00, 0x04, 0x00, 'C', 'E', 'g', 'g'};
        const std::uint8_t genome_class[] = {0xff, 0xff, 0x01, 0x00, 0x07, 0x00, 'C', 'G', 'e', 'n', 'o', 'm', 'e'};
        out.insert(out.end(), std::begin(egg_class), std::end(egg_class));
        u32(VERSION);
        u32(classifier);
        u32(sex);
        u32(static_cast<std::uint32_t>(genome.size()));
        u32(crc32(genome.data(), genome.size()));
        out.insert(out.end(), std::begin(genome_class), std::end(genome_class));
        u32(static_cast<std::uint32_t>(genome.size()));
        for (std::size_t i = 0; i < 4; ++i) {
            out.push_back(i < moniker.size() ? static_cast<std::uint8_t>(moniker[i]) : 0);
        }
        u32(sex == 2 ? 2 : 1); // the genome's own sex: male unless the egg is female
        out.push_back(0); // life stage zero
        out.insert(out.end(), genome.begin(), genome.end());
        return out;
    }

    // Reads a whole file, checking every byte that matters first (people will rename a
    // video to .egg): its size, both class records, the version, an egg classifier, a sex,
    // sizes that agree with each other and the file, a moniker that's a safe file name,
    // genes that start "gene", and the CRC. False, and nothing changed, if it isn't one.
    bool read(const std::uint8_t* data, std::size_t size) {
        const auto u32 = [data](std::size_t at) {
            return static_cast<std::uint32_t>(data[at]) | (static_cast<std::uint32_t>(data[at + 1]) << 8) |
                   (static_cast<std::uint32_t>(data[at + 2]) << 16) | (static_cast<std::uint32_t>(data[at + 3]) << 24);
        };
        if (data == nullptr || size < HEADER_BYTES + 8 || size > HEADER_BYTES + MAX_GENOME_BYTES) return false;
        const std::uint8_t egg_class[] = {0xff, 0xff, 0x01, 0x00, 0x04, 0x00, 'C', 'E', 'g', 'g'};
        const std::uint8_t genome_class[] = {0xff, 0xff, 0x01, 0x00, 0x07, 0x00, 'C', 'G', 'e', 'n', 'o', 'm', 'e'};
        if (std::memcmp(data, egg_class, sizeof egg_class) != 0 || u32(10) != VERSION || !is_egg_classifier(u32(14))) return false;
        if (u32(18) > 2) return false;
        const std::uint32_t genome_size = u32(22);
        if (genome_size != size - HEADER_BYTES || std::memcmp(data + 30, genome_class, sizeof genome_class) != 0 || u32(43) != genome_size) {
            return false;
        }
        for (std::size_t i = 0; i < 4; ++i) {
            if (!is_moniker_character(data[47 + i])) return false;
        }
        const std::uint8_t* genes = data + HEADER_BYTES;
        if (std::memcmp(genes, "gene", 4) != 0 || crc32(genes, genome_size) != u32(26)) return false;
        classifier = u32(14);
        sex = u32(18);
        moniker.assign(reinterpret_cast<const char*>(data + 47), 4);
        genome.assign(genes, genes + genome_size);
        return true;
    }
};

} // namespace openc1::librecreatures
