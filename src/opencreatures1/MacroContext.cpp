#include "MacroContext.h"

#include "Macro.h"
#include "common/Ascii.h"
#include "objects/ObjectManager.h"

std::string format_as(ShortToken i) {
	return std::string(i.data.begin(), i.data.end());
}

std::string format_as(Token i) {
	return std::string(i.data.begin(), i.data.end());
}


int32_t object_to_integer(ObjectHandle handle) {
	if (!g_engine_context.objects->try_get(handle)) {
		return 0;
	}
	return static_cast<int32_t>(handle.to_integral() + 1);
}

ObjectHandle integer_to_object(int32_t value) {
	if (value == 0) {
		return {};
	}
	return ObjectHandle::from_integral(static_cast<uint32_t>(value) - 1);
}

ShortToken MacroContext::read_short_token(Macro& m) {
	// TODO: assert allowed letters
	ShortToken token;
	for (size_t i = 0; i < 2; ++i) {
		if (m.ip < m.script.size()) {
			token.data[i] = m.script[m.ip++];
		} else {
			token.data[i] = '\0';
		}
	}
	return token;
}

Token MacroContext::read_token(Macro& m) {
	// TODO: assert allowed letters
	Token token;
	for (size_t i = 0; i < 4; ++i) {
		if (m.ip < m.script.size()) {
			token.data[i] = m.script[m.ip++];
		} else {
			token.data[i] = '\0';
		}
	}
	return token;
}

std::string MacroContext::read_filename_token(Macro& m) {
	// TODO: assert allowed letters
	auto result = m.script.substr(m.ip, 4);
	m.ip += 4;
	return result;
}

Object* MacroContext::get_ownr(const Macro& m) const {
	auto* ownr = g_engine_context.objects->try_get(m.ownr);
	if (!ownr) {
		throw Exception("ownr is not a valid agent reference");
	}
	return ownr;
}

Object* MacroContext::maybe_get_ownr(const Macro& m) const {
	return g_engine_context.objects->try_get(m.ownr);
}

Object* MacroContext::get_targ(const Macro& m) const {
	auto* targ = g_engine_context.objects->try_get(m.targ);
	if (!targ) {
		throw Exception("targ is not a valid agent reference");
	}
	return targ;
}

void MacroContext::set_targ(Macro& m, ObjectHandle new_targ) {
	m.targ = new_targ;
}

DullPart* MacroContext::get_targ_part(const Macro& m) const {
	auto* part = get_targ(m)->get_part(m.part);
	if (!part) {
		throw Exception("part is not a valid part reference");
	}
	return part;
}

namespace {
bool script_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
}
void MacroContext::read_command_separator(Macro& m) {
    if (m.ip > m.script.size()) throw Exception("Script position exceeds text");
    if (m.ip == m.script.size()) return;
    if (!script_space(m.script[m.ip]) && m.script[m.ip] != ',')
        throw Exception(fmt::format("Expected command separator at {:?}", m.script.substr(m.ip)));
    while (m.ip < m.script.size() && script_space(m.script[m.ip])) ++m.ip;
    if (m.ip < m.script.size() && m.script[m.ip] == ',') ++m.ip;
    while (m.ip < m.script.size() && script_space(m.script[m.ip])) ++m.ip;
}

void MacroContext::read_arg_separator(Macro& m) {
    if (m.ip >= m.script.size() || !script_space(m.script[m.ip]))
        throw Exception(fmt::format("Expected whitespace before next argument at {:?}", m.script.substr(std::min<size_t>(m.ip, m.script.size()))));
    while (m.ip < m.script.size() && script_space(m.script[m.ip])) ++m.ip;
}

std::string MacroContext::read_bracket_string(Macro& m) {
	const char* s = m.script.c_str();
	uint32_t& p = m.ip;

	if (s[p] != '[') {
		throw Exception(fmt::format("Expected '[' but got {:?}", s[p]));
	}
	p++;
	std::string value;
	while (!(s[p] == ']' || s[p] == '\0')) {
		value += s[p];
		p++;
	}
	if (s[p] == '\0') {
		throw Exception("Unexpected end of input when parsing bracket-string");
	}
	p++;
	return value;
}

int32_t MacroContext::read_int(Macro& m) {
	const char* s = m.script.c_str();
	uint32_t& p = m.ip;

	bool negative = false;
	if (s[p] == '-') {
		negative = true;
		p++;
	}
	if (!is_ascii_digit(s[p])) {
		int32_t variable_value;
		Token tok = read_token(m);
		if (try_get_variable(m, tok, &variable_value)) {
			return variable_value;
		}
		auto it = integerrv_funcs.find(tok);
		if (it != integerrv_funcs.end()) {
			if (debug) {
				fmt::print("read_int calling func {}\n", tok);
			}
			return it->second(*this, m);
		}
		// TODO: is this okay? it would be nice to keep strong-typing of agents
		// vs. integers, although Creatures 1 itself seems to just keep everything
		// as an integer
		auto agent_it = agentrv_funcs.find(tok);
		if (agent_it != agentrv_funcs.end()) {
			if (debug) {
				fmt::print("read_int calling agent func {}\n", tok);
			}
			return object_to_integer(agent_it->second(*this, m));
		}
		throw Exception(fmt::format("Expected integer but got {}", s + p - 4));
	}
	int32_t value = 0;
	while (is_ascii_digit(s[p])) {
		value = value * 10 + (s[p] - '0');
		p++;
	}
	if (negative) {
		value *= -1;
	}
	if (debug) {
		fmt::print("read_int found integer literal\n");
	}
	return value;
}

bool MacroContext::read_condition(Macro& m) {
	int32_t left = read_int(m);
	read_arg_separator(m);
	ShortToken comparison = read_short_token(m);
	read_arg_separator(m);
	int32_t right = read_int(m);

	if (comparison == ShortToken("gt")) {
		return left > right;
	} else if (comparison == ShortToken("ge")) {
		return left >= right;
	} else if (comparison == ShortToken("lt")) {
		return left < right;
	} else if (comparison == ShortToken("le")) {
		return left <= right;
	} else if (comparison == ShortToken("eq")) {
		return left == right;
	} else if (comparison == ShortToken("ne")) {
		return left != right;
	} else {
		throw Exception(fmt::format("Unknown comparison operator {}", comparison));
	}
}

ObjectHandle MacroContext::read_object(Macro& m) {
	const char* s = m.script.c_str();
	uint32_t& p = m.ip;

	Token tok = read_token(m);
	auto it = agentrv_funcs.find(tok);
	if (it == agentrv_funcs.end()) {
		throw Exception(fmt::format("Expected object, but got {:?}", s + p - 4));
	}
	return it->second(*this, m);
};

bool MacroContext::try_get_variable(const Macro& m, Token varname, int32_t* value) const {
	if (varname == Token("var0")) {
		*value = m.vars[0];
	} else if (varname == Token("var1")) {
		*value = m.vars[1];
	} else if (varname == Token("var2")) {
		*value = m.vars[2];
	} else if (varname == Token("var3")) {
		*value = m.vars[3];
	} else if (varname == Token("var4")) {
		*value = m.vars[4];
	} else if (varname == Token("var5")) {
		*value = m.vars[5];
	} else if (varname == Token("var6")) {
		*value = m.vars[6];
	} else if (varname == Token("var7")) {
		*value = m.vars[7];
	} else if (varname == Token("var8")) {
		*value = m.vars[8];
	} else if (varname == Token("var9")) {
		*value = m.vars[9];
	} else if (varname == Token("obv0")) {
		*value = get_targ(m)->obv0;
	} else if (varname == Token("obv1")) {
		*value = get_targ(m)->obv1;
	} else if (varname == Token("obv2")) {
		*value = get_targ(m)->obv2;
	} else if (lvalue_funcs.count(varname) && integerrv_funcs.count(varname)) {
		// settable values such as ATTR, ACTV or XVEC can be changed by ADDV, SUBV and the
		// rest, e.g. the eggs' "subv attr 8"; none of them take arguments
		auto& self = const_cast<MacroContext&>(*this);
		*value = integerrv_funcs.find(varname)->second(self, const_cast<Macro&>(m));
	} else {
		return false;
	}
	if (debug) {
		fmt::print("{:?} -> {}\n", varname, *value);
	}
	return true;
}

void MacroContext::set_variable(Macro& m, Token varname, int32_t value) {
	if (varname == Token("var0")) {
		m.vars[0] = value;
	} else if (varname == Token("var1")) {
		m.vars[1] = value;
	} else if (varname == Token("var2")) {
		m.vars[2] = value;
	} else if (varname == Token("var3")) {
		m.vars[3] = value;
	} else if (varname == Token("var4")) {
		m.vars[4] = value;
	} else if (varname == Token("var5")) {
		m.vars[5] = value;
	} else if (varname == Token("var6")) {
		m.vars[6] = value;
	} else if (varname == Token("var7")) {
		m.vars[7] = value;
	} else if (varname == Token("var8")) {
		m.vars[8] = value;
	} else if (varname == Token("var9")) {
		m.vars[9] = value;
	} else if (varname == Token("obv0")) {
		get_targ(m)->obv0 = value;
	} else if (varname == Token("obv1")) {
		get_targ(m)->obv1 = value;
	} else if (varname == Token("obv2")) {
		get_targ(m)->obv2 = value;
	} else {
		auto it = lvalue_funcs.find(varname);
		if (it != lvalue_funcs.end()) {
			it->second(*this, m, value);
			return;
		}
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}
};

const Macro* g_running_macro = nullptr;

void MacroContext::tick_macro(Macro& m, bool handle_errors) {
	// remember which script is running, for crash reports
	struct Running {
		const Macro* previous;
		explicit Running(const Macro* m) : previous(g_running_macro) { g_running_macro = m; }
		~Running() { g_running_macro = previous; }
	} running(&m);
	debug = false;
	if (debug) {
		fmt::print("DEBUG cls=({}, {}, {}) uid={}\n", get_ownr(m)->family, get_ownr(m)->genus, get_ownr(m)->species, m.ownr);
		fmt::print("macro wait={} - {} !!ip!! {} - subroutine {} {}\n", m.wait, m.script.substr(0, m.ip), m.ip <= m.script.size() ? m.script.substr(m.ip) : "", m.subroutine_label, m.subroutine_address);
		fmt::print("stack ");
		// for (size_t i = 0;)
		for (auto s : m.stack) {
			fmt::print("{} ", s);
		}
		fmt::print("\n");
	}

	// we don't want to let inst run forever...
	instructions_left_this_tick = m.inst ? 100 : 1;

	// Some commands don't use up the tick's instruction budget, so a loop made only of
	// them would never end. Stop runaway scripts rather than freezing the game.
	constexpr int32_t RUNAWAY_COMMAND_LIMIT = 100000;
	int32_t commands_run_this_tick = 0;

	while (instructions_left_this_tick > 0 || m.pending_creation) {
		if (++commands_run_this_tick > RUNAWAY_COMMAND_LIMIT) {
			fmt::print("error: script ran {} commands in one tick, stopping it\n", RUNAWAY_COMMAND_LIMIT);
			auto* owner = maybe_get_ownr(m);
			fmt::print("DEBUG owner={}\nmacro {}!!ip!!{}\n\n", format_as(owner), m.script.substr(0, m.ip), m.ip <= m.script.size() ? m.script.substr(m.ip) : "");
            if (m.pending_creation) g_engine_context.objects->destroy_later(m.pending_creation);
            m.pending_creation = {};
			m.destroy_as_soon_as_possible = true;
			break;
		}
		if (m.destroy_as_soon_as_possible) {
			fmt::print("DEBUG [MacroManager] stopping script because destroy_as_soon_as_possible was set. this might not be correct!\n");
			break;
		}

		auto original_ip = m.ip;
		instructions_left_this_tick--;
		try {
			Token command = read_token(m);
			if (command == Token("endm") || command == Token("stop")) {
                if (auto* created = g_engine_context.objects->try_get(m.pending_creation)) created->unplaced = false;
                m.pending_creation = {};
				// remove from list, check selfdestruct
				m.destroy_as_soon_as_possible = true;
				m.ip = original_ip;
				break;
			}
			if (command == Token("\0\0\0\0")) {
				throw UnexpectedEndOfMacro("Expected 'endm' or 'stop'");
			}
			auto it = command_funcs.find(command);
			if (it == command_funcs.end()) {
				throw UnknownMacroCommand(fmt::format("Unknown command {:?}", command));
			}
			it->second(*this, m);
            if (auto* created = g_engine_context.objects->try_get(m.pending_creation); created && !created->unplaced) {
                m.pending_creation = {};
                if (m.stop_after_placement) m.destroy_as_soon_as_possible = true;
            }
		} catch (std::exception& e) {
			// TODO: rather than this handle_errors bit, we should probably throw
			// Exceptions that have all the information needed to build your own
			// error message (mostly the instruction-pointer where the error occurred)
			// and let the caller handle it themselves
            if (m.pending_creation) {
                g_engine_context.objects->destroy_later(m.pending_creation);
                m.pending_creation = {};
            }
			if (!handle_errors) {
				throw;
			}

			fmt::print("error: {}\n", e.what());
			auto* owner = maybe_get_ownr(m);
			if (owner) {
				fmt::print("DEBUG cls=({}, {}, {}) uid={}\n", owner->family, owner->genus, owner->species, m.ownr);
			} else {
				fmt::print("DEBUG badownr uid={}\n", m.ownr);
			}
			fmt::print(
				"macro {}!!ip!!{}\n",
				m.script.substr(0, m.ip),
				m.ip <= m.script.size() ? m.script.substr(m.ip) : "");
			fmt::print("\n");

			if (m.event >= 0) {
				error_messages.push_back(fmt::format("There's an error in a script: {}\n\nScript {} {} {} {}",
					e.what(), m.classifier[0], m.classifier[1], m.classifier[2], m.event));
			} else {
				error_messages.push_back(fmt::format("There's an error in a script: {}", e.what()));
			}

			// stop the script rather than retrying the failing command every tick
			m.ip = original_ip;
			m.destroy_as_soon_as_possible = true;
			return;
		}
	}
}