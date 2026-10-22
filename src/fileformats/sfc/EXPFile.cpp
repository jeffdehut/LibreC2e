#include "EXPFile.h"

#include "Body.h"
#include "CBiochemistry.h"
#include "CBrain.h"
#include "CGallery.h"
#include "CGenome.h"
#include "CInstinct.h"
#include "Creature.h"
#include "Limb.h"
#include "MFCReader.h"
#include "MFCWriter.h"
#include "SimpleObject.h"
#include "common/io/FileReader.h"
#include "common/io/FileWriter.h"

namespace sfc {
EXPFile read_exp_v1_file(const std::string& path) {
	FileReader in(path);
	return read_exp_v1_file(in);
}

EXPFile read_exp_v1_file(Reader& in) {
	// set up types
	MFCReader reader(in);
	reader.register_class<CGalleryV1>("CGallery", 1);
	reader.register_class<CreatureV1>("Creature", 1);
	reader.register_class<BodyV1>("Body", 1);
	reader.register_class<LimbV1>("Limb", 1);
	reader.register_class<CBrainV1>("CBrain", 1);
	reader.register_class<CBiochemistryV1>("CBiochemistry", 1);
	reader.register_class<CInstinctV1>("CInstinct", 1);
	reader.register_class<CGenomeV1>("CGenome", 1);

	// read file
	EXPFile exp;
	reader(exp.creature);
	if (!exp.creature) throw Exception("The export has no creature record");
	if (!in.has_data_left()) {
		// we do this stupid conditional because some EXP files floating around
		// just totally skip the ending genome. in that case, just treat it as
		// null I guess?
		exp.genome = nullptr;
		exp.child_genome = nullptr;
	} else {
		reader(exp.genome);
		if (exp.creature->zygote.size()) {
			reader(exp.child_genome);
		}
	}
	return exp;
}

void write_exp_v1_file(const std::string& path, const EXPFile& exp) {
	FileWriter out(path);
	write_exp_v1_file(out, exp);
	out.close();
}

void write_exp_v1_file(Writer& out, const EXPFile& exp) {
	MFCWriter writer(out);
	writer.register_class<CGalleryV1>("CGallery", 1);
	writer.register_class<CreatureV1>("Creature", 1);
	writer.register_class<BodyV1>("Body", 1);
	writer.register_class<LimbV1>("Limb", 1);
	writer.register_class<CBrainV1>("CBrain", 1);
	writer.register_class<CBiochemistryV1>("CBiochemistry", 1);
	writer.register_class<CInstinctV1>("CInstinct", 1);
	writer.register_class<CGenomeV1>("CGenome", 1);
	writer.register_class<SimpleObjectV1>("SimpleObject", 1);
	writer.register_class<EntityV1>("Entity", 1);

	writer(exp.creature);
	writer(exp.genome);
	if (exp.creature->zygote.size()) {
		writer(exp.child_genome);
	}
}

} // namespace sfc
