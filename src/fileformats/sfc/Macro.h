#pragma once

#include "fileformats/MFCObject.h"

#include <array>
#include <stdint.h>
#include <string>

namespace sfc {

struct ObjectV1;

struct MacroV1 : MFCObject {
	uint32_t selfdestruct;
	uint32_t inst;
	std::string script;
	uint32_t ip;
	std::array<int32_t, 20> stack;
	uint32_t sp;
	std::array<int32_t, 10> vars;
	ObjectV1* ownr = nullptr;
	ObjectV1* from = nullptr;
	ObjectV1* exec = nullptr;
	ObjectV1* targ = nullptr;
	ObjectV1* _it_ = nullptr;
	int32_t part;
	std::string subroutine_label;
	uint32_t subroutine_address;
	int32_t wait;

	template <typename Archive>
	void serialize(Archive& ar) {
		ar(selfdestruct);
		ar(inst);
		// the script's buffer size, which the original makes its length + 6 (as LibreCreatures
		// reads and writes it); ignored when reading
		uint32_t buffer_size = (uint32_t)script.size() + 6;
		ar(buffer_size);
		ar.ascii_mfcstring(script);
		ar(ip);
		for (auto& s : stack) {
			ar(s);
		}
		ar(sp);
		for (auto& v : vars) {
			ar(v);
		}
		ar(ownr);
		ar(from);
		ar(exec);
		ar(targ);
		ar(_it_);
		ar(part);
		ar.ascii_dword(subroutine_label);
		ar(subroutine_address);
		ar(wait);
	}
};

} // namespace sfc