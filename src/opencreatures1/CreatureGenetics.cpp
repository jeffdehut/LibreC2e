#include "CreatureGenetics.h"

#include "CreatureSprites.h"
#include "CreatureBiochemistry.h"
#include "../../third_party/librecreatures/C1Palette.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "PathManager.h"
#include "ViewportManager.h"
#include "common/Exception.h"
#include "common/Random.h"
#include "common/endianlove.h"
#include "common/io/FileReader.h"
#include "common/io/FileWriter.h"
#include "common/io/SpanReader.h"
#include "fileformats/attFile.h"
#include "fileformats/genomeFile.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "fileformats/sfc/CInstinct.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <map>
#include <fmt/core.h>

// --- monikers and genome files ---

int32_t moniker_to_integer(const std::string& moniker) {
	uint32_t value = 0;
	for (size_t i = 0; i < 4 && i < moniker.size(); ++i) {
		value |= static_cast<uint32_t>(static_cast<uint8_t>(moniker[i])) << (8 * i);
	}
	return static_cast<int32_t>(value);
}

std::string register_moniker_text(const std::string& moniker) {
	return moniker.empty() ? "0" : fmt::format("{:x}", static_cast<uint32_t>(moniker_to_integer(moniker)));
}

bool is_register_moniker_text(const std::string& text) {
	// every moniker character is printable, so a moniker's hex is always eight digits
	return text == "0" || (text.size() == 8 && std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));
}

std::string register_moniker(const std::string& text) {
	if (!is_register_moniker_text(text)) {
		return text;
	}
	return text == "0" ? std::string() : moniker_from_integer(static_cast<int32_t>(std::stoul(text, nullptr, 16)));
}

std::string moniker_from_integer(int32_t value) {
	std::string result;
	for (size_t i = 0; i < 4; ++i) {
		char c = static_cast<char>((static_cast<uint32_t>(value) >> (8 * i)) & 0xff);
		if (c) {
			result += c;
		}
	}
	return result;
}

std::shared_ptr<genomeFile> load_genome(const std::string& moniker) {
	auto path = g_engine_context.paths->find_path(PATH_TYPE_GENETICS, moniker + ".gen");
	if (path.empty()) {
		return nullptr;
	}
	FileReader in(path);
	auto genome = std::make_shared<genomeFile>();
	in >> *genome;
	return genome;
}

std::vector<std::vector<uint8_t>> genome_gene_bytes(const std::string& moniker) {
	auto path = g_engine_context.paths->find_path(PATH_TYPE_GENETICS, moniker + ".gen");
	if (path.empty()) {
		return {};
	}
	FileReader in(path);
	auto data = in.read_to_end();
	// a Creatures 1 genome is "gene" records, then "gend"
	std::vector<std::vector<uint8_t>> genes;
	auto starts_with = [&](size_t pos, const char* tag) {
		return pos + 4 <= data.size() && std::equal(tag, tag + 4, data.begin() + static_cast<ptrdiff_t>(pos));
	};
	size_t pos = 0;
	while (pos < data.size() && starts_with(pos, "gene")) {
		size_t next = pos + 4;
		while (next < data.size() && !starts_with(next, "gene") && !starts_with(next, "gend")) {
			next++;
		}
		genes.emplace_back(data.begin() + static_cast<ptrdiff_t>(pos), data.begin() + static_cast<ptrdiff_t>(next));
		pos = next;
	}
	return genes;
}

void save_genome_bytes(const std::string& moniker, const std::vector<std::vector<uint8_t>>& genes) {
	auto out = g_engine_context.paths->create_file(PATH_TYPE_GENETICS, moniker + ".gen");
	for (const auto& g : genes) {
		out.write(reinterpret_cast<const char*>(g.data()), g.size());
	}
	out.write("gend", 4);
	out.close(); // A pregnancy must not reference a genome whose write failed.
}

std::string new_moniker() {
	for (int attempt = 0; attempt < 1000; ++attempt) {
		std::string m;
		m += static_cast<char>('0' + rand_int32(0, 9));
		for (int i = 0; i < 3; ++i) {
			m += static_cast<char>('A' + rand_int32(0, 25));
		}
		bool used = !g_engine_context.paths->find_path(PATH_TYPE_GENETICS, m + ".gen").empty();
		for (auto* obj : *g_engine_context.objects) {
			if (auto* c = obj->as_creature(); c && c->moniker == m) {
				used = true;
			}
		}
		if (!used) {
			return m;
		}
	}
	throw Exception("Couldn't find an unused moniker");
}

// --- child genomes (Q21) ---

namespace {

// gene record bytes: "gene" type subtype id generation switch-on flags data...
constexpr size_t GENE_TYPE = 4, GENE_SUBTYPE = 5, GENE_ID = 6, GENE_GENERATION = 7, GENE_SWITCHON = 8, GENE_FLAGS = 9;

bool same_gene(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
	return a.size() > GENE_ID && b.size() > GENE_ID && a[GENE_TYPE] == b[GENE_TYPE] && a[GENE_SUBTYPE] == b[GENE_SUBTYPE] && a[GENE_ID] == b[GENE_ID];
}

// The genes for breeding, pregnancy and laying, kept intact from parent to child (not in the
// original, which lets them mutate like any other; Jeff's choice, so lines don't die out
// through broken fertility): the biochemistry genes that use the sex hormones (63 oestrogen,
// 64 testosterone, 65 gonadotrophin, 66 progesterone), the receptors and emitters on the
// reproductive loci (organ 1, tissue 2: fertility, conception, pregnancy), and the receptor
// that lays the egg (involuntary action 1, batch 7). They never mutate, double or go missing.
constexpr size_t GENE_DATA = GENE_FLAGS + 1;

bool sex_hormone(uint8_t chemical) {
	return chemical >= 63 && chemical <= 66;
}

bool reproductive_gene(const std::vector<uint8_t>& g) {
	if (g.size() <= GENE_DATA || g[GENE_TYPE] != 1) {
		return false; // only biochemistry genes
	}
	const uint8_t* d = g.data() + GENE_DATA;
	const size_t n = g.size() - GENE_DATA;
	switch (g[GENE_SUBTYPE]) {
		case 0: // a receptor, 1 an emitter: organ, tissue, locus, chemical, ...
		case 1:
			if (n < 4) {
				return false;
			}
			return sex_hormone(d[3]) || (d[0] == 1 && d[1] == 2) || (g[GENE_SUBTYPE] == 0 && d[0] == 1 && d[1] == 4 && d[2] == 1);
		case 2: // a reaction: four amounts and chemicals, then its rate
			for (size_t i = 1; i < n && i < 8; i += 2) {
				if (sex_hormone(d[i])) {
					return true;
				}
			}
			return false;
		case 4: // an initial concentration: chemical, amount
			return sex_hormone(d[0]);
		default:
			return false;
	}
}

// the half-lives gene's bytes for the sex hormones (its other chemicals can mutate)
bool hormone_half_life(const std::vector<uint8_t>& g, size_t at) {
	return g.size() > GENE_DATA && g[GENE_TYPE] == 1 && g[GENE_SUBTYPE] == 3 && at >= GENE_DATA && sex_hormone(static_cast<uint8_t>(std::min<size_t>(at - GENE_DATA, 255)));
}

// As LibreCreatures copies a gene from a parent (checked against the original): the first eight
// bytes ("gene", family, subtype, number, generation) and the flags byte as they are; the
// switch-on stage and every byte of the gene's own data, each with a 1 in 1601 chance of a
// mutation if the gene can mutate. A mutation flips one bit: bit 0 half the time, each higher
// bit half as often (and now and then none). A pigment gene's "gext" marker never mutates, or
// the colours it carries would be lost for good (LibreCreatures' fix 7); its two values can.
std::vector<uint8_t> copy_with_mutation(const std::vector<uint8_t>& g) {
	std::vector<uint8_t> out = g;
	if (g.size() <= GENE_FLAGS || !(g[GENE_FLAGS] & 1) || reproductive_gene(g)) {
		return out; // not mutable (or kept intact)
	}
	const bool pigment_extension = g.size() >= 18 && g[GENE_TYPE] == 2 && g[GENE_SUBTYPE] == 6 &&
		std::equal(g.begin() + 12, g.begin() + 16, "gext");
	for (size_t i = GENE_SWITCHON; i < g.size(); ++i) {
		if (i == GENE_FLAGS || (pigment_extension && i >= 12 && i < 16) || hormone_half_life(g, i)) {
			continue;
		}
		if (rand_int32(0, 1600) != 0) {
			continue;
		}
		uint8_t mask = 1;
		for (int distance = 0; distance < 8; ++distance) {
			if (rand_int32(0, 1)) {
				break;
			}
			mask = static_cast<uint8_t>(mask << 1);
		}
		out[i] ^= mask;
	}
	return out;
}

} // namespace

static std::string cross_genomes(const std::string& a, const std::string& b) {
	auto pa = genome_gene_bytes(a);
	if (pa.empty()) {
		return {};
	}
	auto pb = b.empty() ? std::vector<std::vector<uint8_t>>{} : genome_gene_bytes(b);
	std::vector<std::vector<uint8_t>> child;
	if (pb.empty()) {
		child = pa; // a straight copy, as it is
	} else {
		// As LibreCreatures has it (checked against the original): from a random parent, runs of
		// 10 to 100 genes; then, where the other parent has the next gene (its first one like
		// it), the copying crosses over to carry on from there, unless the next gene is like
		// the one just copied (a duplicated gene). One crossover in 81 duplicates the next gene
		// instead (if it can be: copied once with its generation one higher, then again from
		// the other parent) or leaves it out (if it can be).
		std::array<std::vector<std::vector<uint8_t>>*, 2> parents{&pa, &pb};
		std::array<size_t, 2> pos{0, 0};
		size_t active = static_cast<size_t>(rand_int32(0, 1)) == 0 ? 1 : 0; // 0 the mother, 1 the father
		std::array<uint8_t, 3> copied{};
		const auto identity = [](const std::vector<uint8_t>& g) {
			return std::array<uint8_t, 3>{g.size() > GENE_TYPE ? g[GENE_TYPE] : uint8_t(0), g.size() > GENE_SUBTYPE ? g[GENE_SUBTYPE] : uint8_t(0),
				g.size() > GENE_ID ? g[GENE_ID] : uint8_t(0)};
		};
		// (a cap the original hasn't: duplications can make a genome grow, but never without end)
		const size_t most = 4 * std::max(pa.size(), pb.size()) + 64;
		while (child.size() < most) {
			const size_t run = static_cast<size_t>(rand_int32(10, 100));
			auto& from = *parents[active];
			bool done = false;
			for (size_t n = 0; n < run; ++n) {
				if (pos[active] >= from.size()) {
					done = true;
					break;
				}
				copied = identity(from[pos[active]]);
				child.push_back(copy_with_mutation(from[pos[active]++]));
			}
			if (done || pos[active] >= from.size()) {
				break;
			}
			const auto next = identity(from[pos[active]]);
			const size_t other = 1 - active;
			auto& theirs = *parents[other];
			const auto match = std::find_if(theirs.begin(), theirs.end(), [&](const auto& g) { return identity(g) == next; });
			if (match == theirs.end() || next == copied) {
				continue; // another run from the same parent
			}
			pos[other] = static_cast<size_t>(match - theirs.begin());
			if (rand_int32(0, 80) == 0) {
				if (rand_int32(0, 1) == 0) {
					auto& g = from[pos[active]];
					if (g.size() > GENE_FLAGS && (g[GENE_FLAGS] & 2) && !reproductive_gene(g)) {
						g[GENE_GENERATION]++;
						child.push_back(copy_with_mutation(g));
						pos[active]++;
					}
				} else if (theirs[pos[other]].size() > GENE_FLAGS && (theirs[pos[other]][GENE_FLAGS] & 4) && !reproductive_gene(theirs[pos[other]])) {
					pos[other]++; // left out
				}
			}
			active = other;
		}
	}
	// Batch 6 Q9: a child's genus gene records the actual two parents, not
	// the grandparents copied with whichever genome supplied this record.
	for (auto& record : child) {
		constexpr size_t MOTHER = GENE_FLAGS + 2, FATHER = MOTHER + 4;
		if (record.size() < FATHER + 4 || record[GENE_TYPE] != 2 || record[GENE_SUBTYPE] != 1) continue;
		for (size_t i = 0; i < 4; ++i) {
			record[MOTHER + i] = i < a.size() ? static_cast<uint8_t>(a[i]) : 0;
			record[FATHER + i] = i < b.size() ? static_cast<uint8_t>(b[i]) : 0;
		}
		break;
	}
	std::string moniker = new_moniker();
	save_genome_bytes(moniker, child);
	return moniker;
}

// --- expressing genes (Q17, Q19, Q20) ---

bool gene_applies(uint8_t flags, uint8_t sex) {
	if ((flags & 8) && sex != 1) {
		return false;
	}
	if ((flags & 16) && sex != 2) {
		return false;
	}
	return true;
}

static bool applies(const gene& g, uint8_t sex) {
	return gene_applies(g.header.flags(), sex);
}

std::shared_ptr<sfc::CBrainV1> build_brain(genomeFile& genome, uint8_t sex) {
	// lobe genes that switch on at stage 0: generation 0 first, then duplicates
	std::vector<oldBrainLobeGene*> genes;
	for (int pass = 0; pass < 2; ++pass) {
		for (auto& g : genome.genes) {
			auto* lobe = dynamic_cast<oldBrainLobeGene*>(g.get());
			if (lobe && lobe->header.switchontime == 0 && applies(*lobe, sex) && ((lobe->header.generation == 0) == (pass == 0))) {
				genes.push_back(lobe);
			}
		}
	}
	auto brain = std::make_shared<sfc::CBrainV1>();
	if (genes.empty()) {
		return brain;
	}
	static const uint32_t MINIMUM[8] = {0, 16, 40, 16, 40, 32, 16, 40};
	auto wrap = [](int32_t v, int32_t lo, int32_t hi) { return lo + ((v - lo) % (hi - lo + 1) + (hi - lo + 1)) % (hi - lo + 1); };
	const uint32_t lobe_count = static_cast<uint32_t>(genes.size());

	// As LibreCreatures reads a lobe's genes (its src/c1/brain/rules.cpp and lobe.cpp, which
	// match the original's): a rule's bytes are folded into its 30 tokens and end with two END
	// tokens; a range whose maximum is below its minimum wraps into the range from the minimum up.
	auto copy_rule = [](std::array<uint8_t, 10>& out, const uint8_t* rule) {
		out.fill(0);
		for (size_t i = 0; i < 8; ++i) {
			out[i] = static_cast<uint8_t>(rule[i] % 30);
		}
	};
	auto range_max = [](uint8_t minimum, uint8_t maximum) {
		return maximum >= minimum ? maximum : static_cast<uint8_t>(minimum + maximum % (256 - minimum));
	};
	auto dendrite_type = [&](const oldDendriteInfo& d) {
		sfc::DendriteTypeV1 t{};
		t.source_lobe = static_cast<uint32_t>(d.srclobe) % lobe_count;
		t.min = d.min;
		t.max = range_max(d.min, d.max);
		t.spread = static_cast<uint8_t>(wrap(d.spread, 0, 3));
		t.fanout = static_cast<uint8_t>(wrap(d.fanout, 0, 8));
		t.min_ltw = d.minLTW;
		t.max_ltw = range_max(d.minLTW, d.maxLTW);
		t.min_strength = d.minstr;
		t.max_strength = range_max(d.minstr, d.maxstr);
		t.migration = static_cast<uint8_t>(wrap(d.migrateflag, 0, 2));
		t.relax_suscept = d.relaxsuscept;
		t.relax_stw = d.relaxSTW;
		t.ltw_gain_rate = d.LTWgainrate;
		t.gain_rate = d.strgain;
		t.lose_rate = d.strloss;
		copy_rule(t.gain_svrule, d.strgainrule);
		copy_rule(t.lose_svrule, d.strlossrule);
		copy_rule(t.suscept_svrule, d.susceptrule);
		copy_rule(t.reinforce_svrule, d.relaxrule);
		return t;
	};

	for (size_t i = 0; i < genes.size(); ++i) {
		auto& g = *genes[i];
		sfc::LobeV1 l{};
		l.x = static_cast<uint32_t>(wrap(g.x, 0, 63));
		l.y = static_cast<uint32_t>(wrap(g.y, 0, 63));
		// a size of 1 to 63 is kept; any other byte n gives n % 63 + 1
		const auto extent = [](int32_t v) { return static_cast<uint32_t>(v >= 1 && v <= 63 ? v : v % 63 + 1); };
		l.width = extent(g.width);
		l.height = extent(g.height);
		if (l.x + l.width > 63) {
			l.x = 64 - l.width;
		}
		if (l.y + l.height > 63) {
			l.y = 64 - l.height;
		}
		if (i >= 1 && i < 8) {
			while (l.width * l.height < MINIMUM[i]) {
				l.width++;
			}
		}
		// only lobes 1-7 (those with a smallest size) are held to 32 x 32
		if (i >= 1 && i < 8 && l.width * l.height > 1024) {
			l.width = l.height = 32;
		}
		l.perceptible = static_cast<uint32_t>(wrap(g.perceptflag, 0, 2));
		l.threshold = g.nominalthreshold;
		l.leakage = g.leakagerate;
		l.reststate = g.reststate;
		l.inputgain = g.inputgain;
		copy_rule(l.svrule, g.staterule);
		l.flags = g.flags;
		l.d0_type = dendrite_type(g.dendrite1);
		l.d1_type = dendrite_type(g.dendrite2);
		l.num_neurons = l.width * l.height;
		brain->lobes.push_back(l);
	}

	// neurons, then their dendrites wired to the source lobes
	for (auto& l : brain->lobes) {
		for (uint32_t k = 0; k < l.num_neurons; ++k) {
			sfc::NeuronV1 n{};
			n.x = static_cast<uint8_t>(k % l.width);
			n.y = static_cast<uint8_t>(k / l.width);
			brain->neurons.push_back(n);
		}
	}
	size_t first = 0;
	for (auto& l : brain->lobes) {
		uint32_t dendrites = 0;
		for (int type = 0; type < 2; ++type) {
			const auto& t = type == 0 ? l.d0_type : l.d1_type;
			const auto& src = brain->lobes[t.source_lobe];
			const uint32_t m = src.num_neurons;
			if (!m || t.max < t.min) {
				continue;
			}
			const int32_t r = t.max - t.min;
			for (uint32_t k = 0; k < l.num_neurons; ++k) {
				int32_t a = rand_int32(0, r), b = rand_int32(0, r);
				int32_t extra = 0;
				switch (t.spread) {
					case 0: extra = a; break;
					case 1: extra = (a + b) / 2; break;
					case 2: extra = std::abs(a + b - r); break;
					default: extra = r - std::abs(a + b - r); break;
				}
				const int32_t count = t.min + extra;
				const uint32_t cell = k * m / l.num_neurons;
				const int32_t cx = static_cast<int32_t>(cell % src.width), cy = static_cast<int32_t>(cell / src.width);
				auto& n = brain->neurons[first + k];
				auto& list = type == 0 ? n.dendrites0 : n.dendrites1;
				for (int32_t d = 0; d < count; ++d) {
					int32_t x = cx, y = cy;
					if (d > 0) {
						x = std::clamp(cx + rand_int32(-t.fanout, t.fanout), 0, static_cast<int32_t>(src.width) - 1);
						y = std::clamp(cy + rand_int32(-t.fanout, t.fanout), 0, static_cast<int32_t>(src.height) - 1);
					}
					sfc::DendriteV1 den{};
					den.source_index = static_cast<uint32_t>(y) * src.width + static_cast<uint32_t>(x);
					den.home_x = static_cast<uint8_t>(x);
					den.home_y = static_cast<uint8_t>(y);
					den.ltw = den.stw = rand_uint8(std::min(t.min_ltw, t.max_ltw), std::max(t.min_ltw, t.max_ltw));
					den.strength = rand_uint8(std::min(t.min_strength, t.max_strength), std::max(t.min_strength, t.max_strength));
					list.push_back(den);
				}
				dendrites += static_cast<uint32_t>(count);
			}
		}
		l.num_dendrites = dendrites;
		first += l.num_neurons;
	}
	return brain;
}

std::array<int32_t, 4> appearance_breeds(genomeFile& genome, uint8_t sex) {
	std::array<int32_t, 4> breeds{-1, -1, -1, -1};
	for (auto& g : genome.genes) {
		auto* a = dynamic_cast<creatureAppearanceGene*>(g.get());
		if (a && applies(*a, sex) && a->part < 4) {
			breeds[a->part] = a->variant;
		}
	}
	return breeds;
}

void seed_food_memory(Creature& c) {
	// Not in the original: a creature that has never learned what food does knows that food
	// (category 6) and plants (category 4) lower hunger, as if it had eaten once. Newborns
	// have no learned averages, so hunger-driven recall never steered them to food and, left
	// alone, they starved beside it (batch 7 Q3, batch 8).
	constexpr size_t HUNGER_DOWN = 35 - 33;
	for (auto [category, amount] : {std::pair<size_t, uint8_t>{6, 100}, {4, 50}}) {
		auto& learned = c.learned_chemicals[category];
		if (std::all_of(learned.begin(), learned.end(), [](uint8_t v) { return v == 0; })) {
			learned[HUNGER_DOWN] = amount;
		}
	}
}

// A biochemistry gene's organ byte: 0 the brain, 1 the body; others fold onto those, as the
// original reads them
uint8_t gene_organ(uint8_t organ) {
	return organ < 2 ? organ : static_cast<uint8_t>(organ & 1);
}

void restore_missing_genes(Creature& c, genomeFile& genome) {
	// Worlds saved before the shared-locus fix lost an emitter or receptor whenever another on
	// the same locus switched on (for example a female's oestrogen emitter at stage 2, so she
	// was never fertile). Puts back every receptor and emitter that should be switched on by the
	// creature's life stage and isn't there; chemical levels are left as they are.
	if (!c.biochemistry) {
		return;
	}
	auto& bio = *c.biochemistry;
	int restored = 0;
	for (auto& gp : genome.genes) {
		gene& g = *gp;
		if (!applies(g, c.sex) || g.header.switchontime > c.age) {
			continue;
		}
		if (auto* r = dynamic_cast<bioReceptorGene*>(&g)) {
			// a receptor replaces any other on its locus, so one there of any chemical counts
			const uint8_t organ = gene_organ(r->organ);
			const bool there = std::any_of(bio.receptors.begin(), bio.receptors.end(), [&](auto& x) { return gene_organ(x.organ) == organ && x.tissue == r->tissue && x.locus == r->locus; });
			if (!there && bio.receptors.size() < 128) {
				bio.receptors.push_back({organ, r->tissue, r->locus, r->chemical, r->threshold, r->nominal, r->gain, static_cast<uint8_t>((r->inverted ? 1 : 0) | (r->digital ? 2 : 0))});
				restored++;
			}
		} else if (auto* e = dynamic_cast<bioEmitterGene*>(&g)) {
			const bool there = std::any_of(bio.emitters.begin(), bio.emitters.end(), [&](auto& x) { return x.organ == e->organ && x.tissue == e->tissue && x.locus == e->locus && x.chemical == e->chemical; });
			if (!there && bio.emitters.size() < 128) {
				bio.emitters.push_back({e->organ, e->tissue, e->locus, e->chemical, e->threshold, e->rate, e->gain, static_cast<uint8_t>((e->clear ? 1 : 0) | (e->digital ? 2 : 0) | (e->invert ? 4 : 0))});
				restored++;
			}
		}
	}
	if (restored > 0) {
		fmt::print("INFO [Genetics] {} ({}): put back {} emitters and receptors its genes switch on by now\n", c.history_name, c.moniker, restored);
	}
}

void express_genes(Creature& c, genomeFile& genome, int32_t stage) {
	const uint8_t sex = c.sex;
	if (!c.biochemistry) {
		c.biochemistry = std::make_shared<sfc::CBiochemistryV1>();
	}
	auto& bio = *c.biochemistry;
	for (auto& gp : genome.genes) {
		gene& g = *gp;
		if (!applies(g, sex)) {
			continue;
		}
		const bool now = g.header.switchontime == stage;
		if (auto* genus = dynamic_cast<creatureGenusGene*>(&g)) {
			// appearance genes apply at every stage, whatever their switch-on value
			c.genus = static_cast<uint8_t>(genus->genus % 4 + 1);
			c.mother = genus->mum;
			c.father = genus->dad;
			continue;
		}
		if (!now) {
			continue;
		}
		if (auto* r = dynamic_cast<bioReceptorGene*>(&g)) {
			sfc::ReceptorV1 rec{gene_organ(r->organ), r->tissue, r->locus, r->chemical, r->threshold, r->nominal, r->gain, static_cast<uint8_t>((r->inverted ? 1 : 0) | (r->digital ? 2 : 0))};
			// As LibreCreatures loads them (checked against the original): a receptor replaces
			// any earlier one on the same locus, whatever its chemical; an emitter, only one
			// on the same locus for the same chemical (a female's reproductive locus 0 has an
			// oestrogen emitter and a sex drive one)
			auto* target = creature_locus(c.loci, c.brain.get(), c.dead, true, rec.organ, rec.tissue, rec.locus);
			auto same = std::find_if(bio.receptors.begin(), bio.receptors.end(), [&](auto& x) {
				auto* theirs = creature_locus(c.loci, c.brain.get(), c.dead, true, x.organ, x.tissue, x.locus);
				return target ? theirs == target : (gene_organ(x.organ) == rec.organ && x.tissue == rec.tissue && x.locus == rec.locus);
			});
			if (same != bio.receptors.end()) {
				*same = rec;
			} else if (bio.receptors.size() < 128) {
				bio.receptors.push_back(rec);
			} else {
				continue;
			}
			// Source-informed: newly expressed receptors establish their nominal
			// target before chemistry runs. Not the death signal, though: every shipped
			// genome's death receptor has a nominal of 128 (it reads 0 while glycogen is
			// above 8), so a newborn would die if a 128-tick check came before its first
			// chemistry step.
			if (auto* locus = creature_locus(c.loci, c.brain.get(), c.dead, true, rec.organ, rec.tissue, rec.locus)) {
				if (locus != &c.loci.death_signal) *locus = rec.nominal;
			}
		} else if (auto* e = dynamic_cast<bioEmitterGene*>(&g)) {
			sfc::EmitterV1 em{gene_organ(e->organ), e->tissue, e->locus, e->chemical, e->threshold, e->rate, e->gain, static_cast<uint8_t>((e->clear ? 1 : 0) | (e->digital ? 2 : 0) | (e->invert ? 4 : 0))};
			auto same = std::find_if(bio.emitters.begin(), bio.emitters.end(), [&](auto& x) { return x.organ == em.organ && x.tissue == em.tissue && x.locus == em.locus && x.chemical == em.chemical; });
			if (same != bio.emitters.end()) {
				*same = em;
			} else if (bio.emitters.size() < 128) {
				bio.emitters.push_back(em);
			}
		} else if (auto* rx = dynamic_cast<bioReactionGene*>(&g)) {
			if (bio.reactions.size() < 128) {
				bio.reactions.push_back({reaction_amount(rx->quantity[0]), rx->reactant[0], reaction_amount(rx->quantity[1]), rx->reactant[1], rx->rate,
					reaction_amount(rx->quantity[2]), rx->reactant[2], reaction_amount(rx->quantity[3]), rx->reactant[3]});
			}
		} else if (auto* h = dynamic_cast<bioHalfLivesGene*>(&g)) {
			for (size_t i = 0; i < 256; ++i) {
				bio.chemicals[i].decay = h->halflives[i];
			}
		} else if (auto* ic = dynamic_cast<bioInitialConcentrationGene*>(&g)) {
			bio.chemicals[ic->chemical].concentration = ic->quantity;
		} else if (auto* st = dynamic_cast<creatureStimulusGene*>(&g)) {
			if (st->stim < c.stimuli.size()) {
				uint8_t flags = static_cast<uint8_t>((st->modulate ? 1 : 0) | (st->addoffset ? 2 : 0) | (st->whenasleep ? 4 : 0));
				c.stimuli[st->stim] = {st->significance, st->sensoryneuron, st->intensity, flags, st->drives[0], st->amounts[0], st->drives[1], st->amounts[1], st->drives[2], st->amounts[2], st->drives[3], st->amounts[3]};
			}
		} else if (auto* p = dynamic_cast<creaturePoseGene*>(&g)) {
			if (p->poseno < c.poses.size()) {
				std::string pose(reinterpret_cast<char*>(p->pose), 15);
				for (auto& ch : pose) {
					if (!(ch == '?' || ch == '!' || ch == 'X' || (ch >= '0' && ch <= '9'))) {
						ch = 'X';
					}
				}
				c.poses[p->poseno] = pose;
			}
		} else if (auto* gait = dynamic_cast<creatureGaitGene*>(&g)) {
			if (gait->drive < c.gait_animations.size()) {
				std::string anim;
				for (uint8_t pose : gait->pose) {
					if (!pose) {
						break;
					}
					anim += fmt::format("{:02}", pose);
				}
				c.gait_animations[gait->drive] = anim.empty() ? "" : anim + "R";
			}
		} else if (auto* inst = dynamic_cast<creatureInstinctGene*>(&g)) {
			if (c.instincts.size() < 20) {
				auto queued = std::make_shared<sfc::CInstinctV1>();
				for (size_t i = 0; i < 3; ++i) {
					queued->dendrites[i].lobe = inst->lobes[i];
					queued->dendrites[i].cell = inst->neurons[i];
				}
				queued->motor_decision = inst->action;
				queued->reinforcement_chemical = inst->drive;
				queued->reinforcement_amount = inst->level;
				queued->phase = 0;
				c.instincts.push_back(queued);
			}
		}
	}

	const auto controls = creature_palette_controls(genome, sex);
	std::copy_n(controls.begin(), 3, c.pigments.begin());
	c.pigment_rotation = controls[3];
	c.pigment_swap = controls[4];
}

std::array<uint8_t, 5> creature_palette_controls(genomeFile& genome, uint8_t sex) {
	std::array<int32_t, 3> sum{}, count{};
	int32_t rotation = 0, swap = 0, bleeds = 0;
	for (auto& gp : genome.genes) {
		if (!applies(*gp, sex)) {
			continue;
		}
		if (auto* p = dynamic_cast<creaturePigmentGene*>(gp.get()); p && p->color < 3) {
			sum[p->color] += p->amount;
			count[p->color]++;
			if (p->has_extension) { rotation += p->rotation; swap += p->swap; ++bleeds; }
		} else if (auto* b = dynamic_cast<creaturePigmentBleedGene*>(gp.get())) {
			rotation += b->rotation;
			swap += b->swap;
			bleeds++;
		}
	}
	std::array<uint8_t, 5> controls{};
	for (size_t i = 0; i < 3; ++i) controls[i] = static_cast<uint8_t>(count[i] ? sum[i] / count[i] : 128);
	controls[3] = static_cast<uint8_t>(bleeds ? rotation / bleeds : 128);
	controls[4] = static_cast<uint8_t>(bleeds ? swap / bleeds : 128);
	return controls;
}

// --- bodies ---

void build_body(Creature& c, const std::array<int32_t, 4>& breeds) {
	static const char LETTERS[12] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L'};
	static const int REGION[12] = {0, 1, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3};
	int sex_digit = (c.sex == 2 ? 4 : 0) + (c.genus == 2 ? 1 : 0);
	std::vector<std::string> names;
	for (size_t i = 0; i < 12; ++i) {
		// TODO: how the original picks sprites when one is missing isn't in the specs; try lower
		// breeds, then male sprites, as openc2e does
		std::string name;
		for (int breed = std::max(0, breeds[REGION[i]]); breed >= 0 && name.empty(); --breed) {
			name = creature_body_part_sprite(LETTERS[i], sex_digit, c.age, breed);
		}
        if (name.empty() && (sex_digit & 4)) {
            for (int breed = std::max(0, breeds[REGION[i]]); breed >= 0 && name.empty(); --breed)
                name = creature_body_part_sprite(LETTERS[i], sex_digit & ~4, c.age, breed);
        }
		if (name.empty()) {
			throw Exception(fmt::format("No body-part sprite for creature {} part {}", c.moniker, LETTERS[i]));
		}
		names.push_back(name);
	}
	// The original keeps each creature's sprites in Images/<moniker>.spr (batch 3, Q27). When
	// it writes them isn't known; here it's whenever the body is built, so a world saved
	// here has the sprite files the original needs to load it.
	const auto remap = openc1::librecreatures::build_creature_palette_remap(
		g_engine_context.images->default_palette(), c.pigments[0], c.pigments[1], c.pigments[2], c.pigment_rotation, c.pigment_swap);
	g_engine_context.images->add_joined_spr_image(c.moniker, names, c.moniker, &remap);
	c.sprite_parts = names;
	int32_t total = 0;
	std::vector<int32_t> bases;
	for (auto& n : names) {
		bases.push_back(total);
		auto path = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, n + ".spr");
		FileReader in(path);
		total += read16le(in);
	}
	const auto& gallery = g_engine_context.images->get_image(c.moniker, 0, total, ImageManager::IMAGE_SPR);

	auto att = [&](size_t i) {
		auto path = g_engine_context.paths->find_path(PATH_TYPE_BODY_DATA, names[i] + ".att");
		if (path.empty()) {
			throw Exception(fmt::format("Couldn't find body data {}.att", names[i]));
		}
		FileReader in(path);
		return ReadAttFile(in);
	};
	if (!c.body) {
		c.body = std::make_unique<Body>();
	}
	static_cast<DullPart&>(*c.body) = DullPart(gallery, bases[1], 0, 0, numeric_cast<int32_t>(c.z_order));
	auto body_att = att(1);
	for (size_t frame = 0; frame < 10; ++frame) {
		for (size_t p = 0; p < 6; ++p) {
			c.body->body_data[p][frame] = {static_cast<int8_t>(body_att.attachments[frame][p * 2]), static_cast<int8_t>(body_att.attachments[frame][p * 2 + 1])};
		}
	}
	auto make_chain = [&](std::unique_ptr<Limb>& root, std::initializer_list<size_t> parts) {
		std::unique_ptr<Limb>* slot = &root;
		for (size_t i : parts) {
			if (!*slot) {
				*slot = std::make_unique<Limb>();
			}
			auto next = std::move((*slot)->next_limb);
			static_cast<DullPart&>(**slot) = DullPart(gallery, bases[i], 0, 0, numeric_cast<int32_t>(c.z_order));
			(*slot)->next_limb = std::move(next);
			auto a = att(i);
			for (size_t frame = 0; frame < 10; ++frame) {
				(*slot)->limb_data[frame] = {static_cast<uint8_t>(a.attachments[frame][0]), static_cast<uint8_t>(a.attachments[frame][1]), static_cast<uint8_t>(a.attachments[frame][2]), static_cast<uint8_t>(a.attachments[frame][3])};
			}
			slot = &(*slot)->next_limb;
		}
	};
	make_chain(c.head, {0});
	make_chain(c.left_thigh, {2, 3, 4});
	make_chain(c.right_thigh, {5, 6, 7});
	make_chain(c.left_arm, {8, 9});
	make_chain(c.right_arm, {10, 11});
	c.breeds = breeds;
	c.update_frames();
	c.reposition();
}

ObjectHandle create_creature(const std::string& moniker, int32_t sex) {
	auto genome = load_genome(moniker);
	if (!genome) {
		throw Exception(fmt::format("NEW: CREA couldn't find genome {:?}", moniker));
	}
	if (sex == 0) {
		// balance the population: female with chance males / all living creatures (Q18)
		int32_t males = 0, living = 0;
		for (auto* obj : *g_engine_context.objects) {
			if (auto* other = obj->as_creature(); other && !other->dead) {
				living++;
				males += other->sex == 1;
			}
		}
		sex = (living ? rand_int32(0, living - 1) < males : rand_int32(0, 1) == 0) ? 2 : 1;
	}
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.moniker = moniker;
	c.family = 4;
	c.genus = 1;
	c.sex = static_cast<uint8_t>(sex);
	c.species = c.sex; // a creature's species is its sex (Q20)
	c.attr = 70; // as saved creatures have: wall-bound, activateable, mouseable
	// no limits (movs 2) until a script runs SLIM or MCRT (batch 5, Q1)
	c.movement_status = MOVEMENT_FLOATING;
	c.actv = ACTV_INACTIVE;
	c.limit = Rect2i{-100000, -100000, 200000, 200000};
	c.tick_value = c.timer_countdown = 0;
	c.obv0 = c.obv1 = c.obv2 = 0;
	// facing right with its ground point at (0, 0) until a script moves it (batch 3, Q38)
	c.direction = 2;
	c.downfoot_right = false;
	c.footx = c.footy = 0;
	c.choose_plane();
	c.expression = 1;
	c.eyes_open = 1;
	c.asleep = 0;
	c.age = 0;
	c.biotick = 0;
	c.dead = 0;
	c.age_ticks = 0;
	seed_food_memory(c);
	c.dreaming = 0;
	c.history_moniker = moniker;
	c.history_name = "<Un Named>";
	for (auto& p : c.object_positions) {
		p = {-1, -1};
	}
	c.genome = genome;

	c.brain = build_brain(*genome, c.sex); // brain receptor targets must exist during expression
	express_genes(c, *genome, 0);
	c.initialize_vocabulary(); // genus is now known, including Grendel word banks
	build_body(c, appearance_breeds(*genome, c.sex));
	c.load_voice();
	c.history_moms_moniker = c.mother;
	c.history_dads_moniker = c.father;
	// born now, written as the original writes it ("17:18 Oct 28 1997"; LibreCreatures)
	{
		static const char* const MONTHS[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
		const std::time_t now = std::time(nullptr);
		if (const std::tm* t = std::localtime(&now)) {
			c.history_birthday = fmt::format("{:02}:{:02} {} {:02} {}", t->tm_hour, t->tm_min, MONTHS[t->tm_mon % 12], t->tm_mday, t->tm_year + 1900);
		}
	}
	fmt::print("INFO [Life] Created creature {}: sex {}, mother {}, father {}\n", c.moniker, c.sex, c.mother, c.father);
	c.init_pose_values_from_frames();
	if (!c.poses[0].empty()) {
		c.set_pose_number(0);
	}
	return handle;
}

namespace {
std::map<std::string, std::pair<std::string, std::string>> s_genome_parents;
}

std::string make_child_genome(const std::string& a, const std::string& b) {
	auto child = cross_genomes(a, b);
	if (!child.empty()) {
		s_genome_parents[child] = {a, b};
	}
	return child;
}

const std::pair<std::string, std::string>* genome_parents(const std::string& child) {
	auto it = s_genome_parents.find(child);
	return it == s_genome_parents.end() ? nullptr : &it->second;
}
