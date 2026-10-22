#pragma once

#include <memory>
#include <string>

class Reader;
class Writer;

namespace sfc {

struct CreatureV1;
struct CGenomeV1;

struct EXPFile {
	std::shared_ptr<CreatureV1> creature;
	std::shared_ptr<CGenomeV1> genome;
	std::shared_ptr<CGenomeV1> child_genome;
};

EXPFile read_exp_v1_file(Reader& in);
EXPFile read_exp_v1_file(const std::string& path);
// writes the creature, its genome and, if it's pregnant, its child's genome
void write_exp_v1_file(Writer& out, const EXPFile&);
void write_exp_v1_file(const std::string& path, const EXPFile&);

} // namespace sfc
