#include "MacroCommands.h"
#include "../../third_party/librecreatures/C1Arithmetic.h"

#include "C1ControlledSound.h"
#include "C1SoundManager.h"
#include "ImageManager.h"
#include "Macro.h"
#include "MacroContext.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "MessageManager.h"
#include "PointerManager.h"
#include "Scriptorium.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "common/Random.h"
#include "objects/Blackboard.h"
#include "objects/Object.h"
#include "objects/ObjectManager.h"
#include "fileformats/sfc/WinRect.h"
#include "objects/CompoundObject.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "CreatureBrain.h"
#include "CreatureGenetics.h"
#include "objects/Creature.h"
#include "objects/SimpleObject.h"
#include "objects/Vehicle.h"

#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <vector>

void Command_ADDV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t right = ctx.read_int(m);

	int32_t left;
	if (!ctx.try_get_variable(m, varname, &left)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}
	ctx.set_variable(m, varname, openc1::librecreatures::add(left, right));
	ctx.read_command_separator(m);
}

void Command_ANIM(MacroContext& ctx, Macro& m) {
	if (m.script[m.ip] == '[') {
		// The learning computer and cloud butterflies have scripts that go
		// `anim[0123]` with no whitespace separating the ANIM command and
		// the string argument. If we're followed directly by the string
		// bracket, skip trying to parse a separator.
	} else {
		ctx.read_arg_separator(m);
	}

	std::string anim_string = ctx.read_bracket_string(m);
	if (auto* creature = ctx.get_targ(m)->as_creature()) {
		// creature animations are two-digit pose numbers
		creature->set_pose_animation(anim_string);
		ctx.read_command_separator(m);
		return;
	}
	auto* r = ctx.get_targ_part(m);
	r->set_animation(0, anim_string);

	ctx.read_command_separator(m);
}

void Command_BASE(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t new_base = ctx.read_int(m);

	auto* r = ctx.get_targ_part(m);
	// TODO: I think we both set the new base and reset the pose to 0?
	r->set_base(new_base);
	r->set_pose(0);

	ctx.read_command_separator(m);
}

void Command_BBDcolon(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);

	auto targ = ctx.get_targ(m);
	auto bbd = targ->as_blackboard();

	Token subcommand = ctx.read_token(m);
	ctx.read_arg_separator(m);

	if (subcommand == Token("word")) {
		int32_t index = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t id = ctx.read_int(m);
		ctx.read_arg_separator(m);
		// BBD: WORD slot vocabulary_entry [text] (batch 4, Q19)
		std::string word;
		if (m.script.c_str()[m.ip] == '[') {
			word = ctx.read_bracket_string(m);
		} else {
			Token t = ctx.read_token(m);
			word.assign(t.data.begin(), t.data.end());
		}
		if (index < 0 || index >= 16 || !bbd) {
			throw Exception(fmt::format("BBD: WORD slot {} out of range", index));
		}
		bbd->words[static_cast<size_t>(index)].value = static_cast<uint32_t>(id);
		bbd->words[static_cast<size_t>(index)].text = word.substr(0, Blackboard::MAX_WORD);

	} else if (subcommand == Token("show")) {
		int32_t enable = ctx.read_int(m);
		if (enable == 1) {
			bbd->blackboard_show_word(targ->obv0);
		} else if (enable == 0) {
			bbd->blackboard_hide_word();
		} else {
			throw Exception(fmt::format("BBD: SHOW {} invalid argument", enable));
		}

	} else if (subcommand == Token("emit")) {
		int32_t volume = ctx.read_int(m);
		if (volume > 0) {
			bbd->blackboard_emit_earshot(targ->obv0);
		} else if (volume == 0) {
			bbd->blackboard_emit_eyesight(targ->obv0);
		} else {
			throw Exception(fmt::format("BBD: EMIT {} invalid argument", volume));
		}

	} else if (subcommand == Token("edit")) {
		int32_t enable = ctx.read_int(m);
		if (enable == 1) {
			bbd->blackboard_enable_edit();
		} else if (enable == 0) {
			bbd->blackboard_disable_edit();
		} else {
			throw Exception(fmt::format("BBD: EDIT {} invalid argument", enable));
		}

	} else {
		throw UnknownMacroCommand(fmt::format("unknown command bbd: {:?}", subcommand));
	}
	ctx.read_command_separator(m);
}

void Command_DIVV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t divisor = ctx.read_int(m);

	int32_t dividend;
	if (!ctx.try_get_variable(m, varname, &dividend)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}

	// LibreCreatures correction F060: never execute an overflowing signed divide.
	if (auto result = openc1::librecreatures::divide(dividend, divisor)) {
		ctx.set_variable(m, varname, *result);
	}

	ctx.read_command_separator(m);
}

void Command_MODV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t divisor = ctx.read_int(m);

	int32_t dividend;
	if (!ctx.try_get_variable(m, varname, &dividend)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}

	// LibreCreatures correction F060: never execute an overflowing signed remainder.
	if (auto result = openc1::librecreatures::remainder(dividend, divisor)) {
		ctx.set_variable(m, varname, *result);
	}

	ctx.read_command_separator(m);
}


// Scan script syntax without interpreting words inside bracketed text. Both comma
// and space command separators occur in shipped C1 scripts.
static Token next_syntax_token(MacroContext& ctx, Macro& m) {
    while (m.ip < m.script.size()) {
        if (m.script[m.ip] == '[') {
            ctx.read_bracket_string(m);
            continue;
        }
        const size_t pos = m.ip;
        if ((pos == 0 || m.script[pos - 1] == ',' || m.script[pos - 1] == ' ')
            && pos + 4 <= m.script.size()
            && (pos + 4 == m.script.size() || m.script[pos + 4] == ' ' || m.script[pos + 4] == ',')) {
            return ctx.read_token(m);
        }
        ++m.ip;
    }
    throw Exception("Unexpected end of script while scanning control flow");
}

void Command_DOIF(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	bool cond = ctx.read_condition(m);
	ctx.instructions_left_this_tick++;
	// if condition is true, just go to next command. ELSE will be skipped when
	// we hit it later
	if (cond) {
		if (ctx.debug) {
			printf("DOIF evaluted to true\n");
		}
		ctx.read_command_separator(m);
		return;
	}
	if (ctx.debug) {
		printf("DOIF evaluted to false\n");
	}
	// otherwise, go find the next ELSE/ENDI
	// TODO: hacky and probably wrong! get a better parser
	size_t level = 1;
	while (true) {
		Token command = next_syntax_token(ctx, m);
		if (command == Token("doif")) {
			level += 1;
		} else if (command == Token("else") && level == 1) {
			// cool, skip to this else
			ctx.read_command_separator(m);
			return;
		} else if (command == Token("endi")) {
			level -= 1;
			if (level == 0) {
				// cool, skip to this endi
				ctx.read_command_separator(m);
				return;
			}
		} else {
            // Continue after this token; bracketed arguments are skipped atomically.
        }
	}
}



void Command_DPAS(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	ctx.get_targ(m)->vehicle_drop_passengers();
}

void Command_ELSE(MacroContext& ctx, Macro& m) {
	// TODO: hacky and probably wrong! get a better parser
	ctx.instructions_left_this_tick++;
	size_t level = 1;
	while (true) {
		Token command = next_syntax_token(ctx, m);
		if (command == Token("doif")) {
			level += 1;
		} else if (command == Token("endi")) {
			level -= 1;
			if (level == 0) {
				ctx.read_command_separator(m);
				if (ctx.debug) {
					printf("ELSE jumped to %i\n", m.ip);
				}
				break;
			}
		} else {
            // Continue after this token; bracketed arguments are skipped atomically.
        }
	}
}

void Command_ENDI(MacroContext& ctx, Macro& m) {
	// I think we just move on?
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
}

// ENUM and RTAR: a matching classifier (0 matches anything), not hidden under the world
static bool enumerable(Object* obj, int32_t family, int32_t genus, int32_t species) {
	if ((obj->family != family && family != 0) || (obj->genus != genus && genus != 0) || (obj->species != species && species != 0)) {
		return false;
	}
	if (!obj->as_creature() && !obj->get_part(0)) {
		return true;
	}
	return obj->get_bbox().y <= numeric_cast<float>(CREATURES1_WORLD_HEIGHT);
}

void Command_ENUM(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t family = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t genus = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t species = ctx.read_int(m);
	ctx.read_command_separator(m);

	if (family < 0 || family > 255 || genus < 0 || genus > 255 || species < 0 || species > 255) {
		throw Exception(fmt::format("Invalid arguments to ENUM {} {} {}", family, genus, species));
	}

	if (m.enum_result.size()) {
		throw Exception("Nested calls to ENUM are not implemented");
	}

	// in the object list's order, skipping objects hidden under the world (batch 5, Q3); the
	// results are taken from the back
	for (auto* obj : *g_engine_context.objects) {
		if (enumerable(obj, family, genus, species)) {
			m.enum_result.push_back(obj->uid);
		}
	}
	std::reverse(m.enum_result.begin(), m.enum_result.end());

	// if we have results, then run the block
	if (m.enum_result.size()) {
		m.stack.push_back(static_cast<int32_t>(m.ip)); // where to jump back to
		m.targ = m.enum_result.back();
		m.enum_result.pop_back();
		return;
	}

	// otherwise, if no objects, go find the next NEXT
	// TODO: hacky and probably wrong! get a better parser
	size_t level = 1;
	while (true) {
		Token command = next_syntax_token(ctx, m);
		if (command == Token("enum")) {
			level += 1;
		} else if (command == Token("next")) {
			level -= 1;
			if (level == 0) {
				// cool, skip past this NEXT
				ctx.read_command_separator(m);
				return;
			}
		} else {
            // Continue after this token; bracketed arguments are skipped atomically.
        }
	}
}

void Command_EVER(MacroContext& ctx, Macro& m) {
	// at the end of the loop, go back to start
	m.ip = numeric_cast<uint32_t>(m.stack.back());
	ctx.instructions_left_this_tick++;
}

void Command_FADE(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);

	ctx.get_targ(m)->current_sound.fade_out();
}

void Command_GPAS(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	ctx.get_targ(m)->vehicle_grab_passengers();
}

void Command_INST(MacroContext& ctx, Macro& m) {
	m.inst = true;
	ctx.read_command_separator(m);
}

void Command_LOOP(MacroContext& ctx, Macro& m) {
	ctx.read_command_separator(m);
	m.stack.push_back(numeric_cast<int32_t>(m.ip));
	ctx.instructions_left_this_tick++;
}

void Command_MESG(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token subcommand = ctx.read_token(m);
	if (subcommand == Token("writ")) {
		ctx.read_arg_separator(m);
		ObjectHandle to = ctx.read_object(m);
		ctx.read_arg_separator(m);
		int32_t message = ctx.read_int(m);
		ctx.read_command_separator(m);

		g_engine_context.messages->mesg_writ(m.ownr, to, MessageNumber(message));

	} else {
		throw Exception(fmt::format("Unknown command 'mesg {:?}'", subcommand));
	}
}

void Command_MULV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t multiplier = ctx.read_int(m);

	int32_t multiplicand;
	if (!ctx.try_get_variable(m, varname, &multiplicand)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}

	ctx.set_variable(m, varname, openc1::librecreatures::multiply(multiplicand, multiplier));

	ctx.read_command_separator(m);
}

void Command_MVBY(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t xdiff = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t ydiff = ctx.read_int(m);

	auto* obj = ctx.get_targ(m);
	obj->add_position(xdiff, ydiff);

	if (ctx.debug) {
		fmt::print("did a mvby  x={} y={} cls=({}, {}, {}) spr={}!\n",
			xdiff, ydiff, ctx.get_targ(m)->family, ctx.get_targ(m)->genus,
			ctx.get_targ(m)->species, ctx.get_targ_part(m)->gallery().name);
	}

	ctx.read_command_separator(m);
}

void Command_MVTO(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t x = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t y = ctx.read_int(m);

	auto* obj = ctx.get_targ(m);
    if (obj->movement_status == MOVEMENT_INVEHICLE) {
        auto* carrier = g_engine_context.objects->try_get(obj->carrier);
        if (carrier && carrier->as_vehicle()) {
            const auto vehicle = carrier->get_bbox();
            const auto& cabin = carrier->as_vehicle()->cabin;
            const Rect2f bounds{vehicle.x + cabin.x, vehicle.y + cabin.y, float(cabin.width), float(cabin.height)};
            if (!bounds.has_point(float(x), float(y))) {
                obj->carrier = {};
                obj->movement_status = MOVEMENT_AUTONOMOUS;
                if (auto* c = obj->as_creature()) {
                    c->move_ground_to(float(x), float(y));
                    c->update_limits();
                }
            }
        }
    }
	if (auto* creature = obj->as_creature()) {
		// MVTO moves a creature's ground point, then makes the same checks as walking against
		// its unchanged limits (batch 5, Q2)
		creature->move_ground_to(numeric_cast<float>(x), numeric_cast<float>(y));
		creature->settle_on_limits();
		ctx.read_command_separator(m);
		return;
	}

	obj->set_position(x, y);

	// if (ctx.debug) {
	// 	fmt::print("did a mvby  x={} y={} cls=({}, {}, {}) spr={}!\n",
	// 		xdiff, ydiff, ctx.get_targ(m)->family, ctx.get_targ(m)->genus, ctx.get_targ(m)->species, ctx.get_targ_part(m)->sprite.getName());
	// }

	ctx.read_command_separator(m);
}

void Command_NEGV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);

	int32_t value;
	if (!ctx.try_get_variable(m, varname, &value)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}

	ctx.set_variable(m, varname, openc1::librecreatures::negate(value));

	ctx.read_command_separator(m);
}

void Command_NEXT(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);

	while (m.enum_result.size()) {
		ObjectHandle obj = m.enum_result.back();
		m.enum_result.pop_back();
		if (g_engine_context.objects->try_get(obj)) {
			m.targ = obj;
			m.ip = static_cast<uint32_t>(m.stack.back());
			return;
		}
	}

	// done with the ENUM: TARG goes back to OWNR, which the shipped scripts rely on (the honey
	// pot's drop script ends its "enum 2 8 1 ... next" with "setv obv0 0" to refill itself)
	m.stack.pop_back();
	m.targ = m.ownr;
}

void Command_OVER(MacroContext& ctx, Macro& m) {
	auto* creature = ctx.get_targ(m)->as_creature();
	if (creature ? creature->has_pose_animation() : ctx.get_targ_part(m)->has_animation()) {
		m.ip -= 4;
	} else {
		if (ctx.debug) {
			printf("animation over!\n");
		}
		ctx.read_command_separator(m);
	}
}

void Command_PART(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t part = ctx.read_int(m);
	ctx.read_command_separator(m);

	m.part = part;
}

void Command_POSE(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t pose = ctx.read_int(m);
	ctx.read_command_separator(m);

	if (auto* creature = ctx.get_targ(m)->as_creature()) {
		creature->pose_animation.clear();
		creature->set_pose_number(pose);
		return;
	}

	auto* part = ctx.get_targ_part(m);
	part->clear_animation();
	part->set_pose(pose);
	if (!part->gallery().has_frame(part->frame())) {
		fmt::print("WARN [POSE] {} has no image for pose {} ({} images), in script {} {} {} {}\n",
			format_as(*ctx.get_targ(m)), pose, part->gallery().size(), m.classifier[0], m.classifier[1], m.classifier[2], m.event);
	}
}

void Command_REPE(MacroContext& ctx, Macro& m) {
	// done with this iteration of loop, go back to start if needed
	auto& iterations = m.stack[m.stack.size() - 1];
	auto& loop_start = m.stack[m.stack.size() - 2];
	if (ctx.debug) {
		printf("repe info iter %i start %i\n", iterations, loop_start);
	}
	iterations--;
	if (iterations > 0) {
		m.ip = numeric_cast<uint32_t>(loop_start);
	} else {
		m.stack.pop_back(); // iterations
		m.stack.pop_back(); // loop start
		if (ctx.debug) {
			printf("Done with REPE!\n");
		}
		ctx.read_command_separator(m);
	}
	ctx.instructions_left_this_tick++;
}

void Command_REPS(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t iterations = ctx.read_int(m);
	if (iterations <= 0) {
		throw Exception(fmt::format("REPS called with bad value: {}", iterations));
	}
	ctx.read_command_separator(m);
	m.stack.push_back(numeric_cast<int32_t>(m.ip));
	m.stack.push_back(iterations);
	ctx.instructions_left_this_tick++;
}

void Command_RNDV(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t low = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t high = ctx.read_int(m);

	// Some scripts give the bounds in reverse order, e.g. the bees' "rndv var1 -1 -5"
	if (low > high) {
		std::swap(low, high);
	}
	int32_t generated = rand_int32(low, high);
	if (ctx.debug) {
		fmt::print("RNDV info {:?} {} {}\n", varname, low, high);
		fmt::print("RNDV generated {}\n", generated);
	}
	ctx.set_variable(m, varname, generated);

	ctx.read_command_separator(m);

	ctx.instructions_left_this_tick++;
}

void Command_SETV(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t value = ctx.read_int(m);

	ctx.set_variable(m, varname, value);

	ctx.read_command_separator(m);
	ctx.instructions_left_this_tick++;
}

void Command_SNDC(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	std::string sound_name = ctx.read_filename_token(m);
	ctx.read_command_separator(m);

	auto* targ = ctx.get_targ(m);
	C1ControlledSound sound = g_engine_context.sounds->play_controlled_sound(sound_name, targ->get_bbox());
	if (!sound) {
		return;
	}
	targ->current_sound.fade_out();
	targ->current_sound = std::move(sound);
}

void Command_SNDE(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	std::string sound_name = ctx.read_filename_token(m);
	ctx.read_command_separator(m);

	// each object has one current sound, and a new one replaces it (batch 3, Q24)
	auto* targ = ctx.get_targ(m);
	C1ControlledSound sound = g_engine_context.sounds->play_controlled_sound(sound_name, targ->get_bbox());
	if (!sound) {
		return;
	}
	targ->current_sound.fade_out();
	targ->current_sound = std::move(sound);
}

void Command_SNDL(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	std::string sound_name = ctx.read_filename_token(m);
	ctx.read_command_separator(m);

	auto* targ = ctx.get_targ(m);
	C1ControlledSound sound = g_engine_context.sounds->play_controlled_sound(sound_name, targ->get_bbox(), true);
	if (!sound) {
		return;
	}

	targ->current_sound.fade_out();
	targ->current_sound = std::move(sound);
}

static Object::StimulusScope read_stimulus_scope(Token subcommand, const char* command) {
	if (subcommand == Token("writ")) {
		return Object::STIM_WRIT;
	}
	if (subcommand == Token("shou")) {
		return Object::STIM_SHOU;
	}
	if (subcommand == Token("sign")) {
		return Object::STIM_SIGN;
	}
	if (subcommand == Token("tact")) {
		return Object::STIM_TACT;
	}
	throw UnknownMacroCommand(fmt::format("Unknown command '{} {:?}'", command, subcommand));
}

void Command_STIM(MacroContext& ctx, Macro& m) {
	// STIM WRIT creature, or STIM SHOU/SIGN/TACT from TARG, followed by significance,
	// input neuron, intensity, flags and four chemical/amount pairs
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token subcommand = ctx.read_token(m);
	if (subcommand == Token("from")) {
		// The Creatures 1 learning computer calls STIM FROM, which doesn't seem to be a
		// real command. Ignore it.
		for (int i = 0; i < 12; ++i) {
			ctx.read_arg_separator(m);
			ctx.read_int(m);
		}
		ctx.read_command_separator(m);
		return;
	}
	auto scope = read_stimulus_scope(subcommand, "stim");
	Object* writ_target = nullptr;
	if (scope == Object::STIM_WRIT) {
		ctx.read_arg_separator(m);
		writ_target = g_engine_context.objects->try_get(ctx.read_object(m));
	}
	std::array<int32_t, 12> v;
	for (auto& i : v) {
		ctx.read_arg_separator(m);
		i = ctx.read_int(m);
	}
	ctx.read_command_separator(m);

	auto byte = [](int32_t i) { return static_cast<uint8_t>(i); };
	sfc::StimulusV1 stimulus{byte(v[0]), byte(v[1]), byte(v[2]), byte(v[3]), byte(v[4]), byte(v[5]), byte(v[6]), byte(v[7]), byte(v[8]), byte(v[9]), byte(v[10]), byte(v[11])};
	// stimuli come from the script's owner
	auto* source = ctx.maybe_get_ownr(m);
	if (!source) {
		source = ctx.get_targ(m);
	}
	source->stimulate(scope, writ_target, stimulus);
}

void Command_STMpound(MacroContext& ctx, Macro& m) {
	// STM# WRIT creature number, or STM# SHOU/SIGN/TACT number: each creature's own stimulus
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	auto scope = read_stimulus_scope(ctx.read_token(m), "stm#");
	Object* writ_target = nullptr;
	if (scope == Object::STIM_WRIT) {
		ctx.read_arg_separator(m);
		writ_target = g_engine_context.objects->try_get(ctx.read_object(m));
	}
	ctx.read_arg_separator(m);
	int32_t number = ctx.read_int(m);
	ctx.read_command_separator(m);

	auto* source = ctx.maybe_get_ownr(m);
	if (!source) {
		source = ctx.get_targ(m);
	}
	source->stimulate_numbered(scope, writ_target, number);
}

void Command_STPC(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	auto* targ = ctx.get_targ(m);
	if (targ->current_sound) {
		targ->current_sound.stop();
	}
	ctx.read_command_separator(m);
}

void Command_TARG(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	ObjectHandle new_targ = ctx.read_object(m);
	ctx.set_targ(m, new_targ);
	ctx.read_command_separator(m);
}

void Command_TELE(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t x = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t y = ctx.read_int(m);
	ctx.read_command_separator(m);

	ctx.get_targ(m)->vehicle_teleport_passengers(numeric_cast<float>(x), numeric_cast<float>(y));
}

void Command_SUBV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t subtrahend = ctx.read_int(m);

	int32_t minuend;
	if (!ctx.try_get_variable(m, varname, &minuend)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}

	ctx.set_variable(m, varname, openc1::librecreatures::subtract(minuend, subtrahend));

	ctx.read_command_separator(m);
}


void Command_TICK(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t tick_value = ctx.read_int(m);
	ctx.get_targ(m)->tick_value = tick_value;
	// TODO: should this be reset?
	ctx.get_targ(m)->ticks_since_last_tick_event = 0;

	ctx.read_command_separator(m);
}

void Command_WAIT(MacroContext& ctx, Macro& m) {
	auto old_ip = m.ip - 4;

	ctx.read_arg_separator(m);
	int32_t wait_value = ctx.read_int(m);
	if (ctx.debug) {
		printf("wait %i\n", wait_value);
	}
	if (m.wait == 0) {
		m.wait = wait_value;
		m.ip = old_ip;
	} else {
		m.wait--;
		if (m.wait == 0) {
			if (ctx.debug) {
				printf("finished waiting!\n");
			}
			ctx.read_command_separator(m);
		} else {
			m.ip = old_ip;
		}
	}
}

void Command_UNTL(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	bool cond = ctx.read_condition(m);
	if (cond) {
		m.stack.pop_back();
		ctx.read_command_separator(m);
	} else {
		m.ip = numeric_cast<uint32_t>(m.stack.back());
	}
	ctx.instructions_left_this_tick++;
}

ObjectHandle AgentRV__IT_(MacroContext& ctx, Macro& m) {
	// for creatures, the object of attention when the script started
	return m._it_;
}

ObjectHandle AgentRV_FROM(MacroContext&, Macro& macro) {
	return macro.from;
}

ObjectHandle AgentRV_OWNR(MacroContext&, Macro& macro) {
	return macro.ownr;
}

ObjectHandle AgentRV_PNTR(MacroContext&, Macro&) {
	return g_engine_context.pointer->m_pointer_tool;
}

ObjectHandle AgentRV_TARG(MacroContext&, Macro& macro) {
	return macro.targ;
}

int32_t IntegerRV_ACTV(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return static_cast<int32_t>(targ->actv);
}

int32_t IntegerRV_HGHT(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().height);
}

int32_t IntegerRV_LIMB(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return targ->limit.bottom();
}

int32_t IntegerRV_LIML(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return targ->limit.x;
}

int32_t IntegerRV_LIMR(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return targ->limit.right();
}

int32_t IntegerRV_LIMT(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return targ->limit.y;
}

int32_t IntegerRV_POSB(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().bottom());
}

int32_t IntegerRV_POSE(MacroContext& ctx, Macro& m) {
	if (auto* creature = ctx.get_targ(m)->as_creature()) {
		return creature->pose_number;
	}
	return ctx.get_targ_part(m)->pose();
}

int32_t IntegerRV_POSL(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().x);
}

int32_t IntegerRV_POSR(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().right());
}

int32_t IntegerRV_POST(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().y);
}

int32_t IntegerRV_TOTL(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	auto family = ctx.read_int(m);
	ctx.read_arg_separator(m);
	auto genus = ctx.read_int(m);
	ctx.read_arg_separator(m);
	auto species = ctx.read_int(m);

	return g_engine_context.objects->count_classifier(family, genus, species);
}

int32_t IntegerRV_WDTH(MacroContext& ctx, Macro& m) {
	return numeric_cast<int32_t>(ctx.get_targ(m)->get_bbox().width);
}

int32_t IntegerRV_XVEC(MacroContext& ctx, Macro& m) {
	Object* o = ctx.get_targ(m);
	auto* veh = o->as_vehicle();
	if (!veh) {
		throw_exception("Called XVEC on non-vehicle object: {:?}", *o);
	}

	return numeric_cast<int32_t>(veh->xvel * 256);
}

int32_t IntegerRV_YVEC(MacroContext& ctx, Macro& m) {
	Object* o = ctx.get_targ(m);
	auto* veh = o->as_vehicle();
	if (!veh) {
		throw_exception("Called YVEC on non-vehicle object: {:?}", *o);
	}
	return numeric_cast<int32_t>(veh->yvel * 256);
}

int32_t IntegerRV_BUMP(MacroContext& ctx, Macro& m) {
	// what the vehicle has hit (batch 3, Q19)
	Object* o = ctx.get_targ(m);
	auto* veh = o->as_vehicle();
	if (!veh) {
		throw_exception("Called BUMP on non-vehicle object: {:?}", *o);
	}
	return static_cast<int32_t>(veh->bump);
}

int32_t IntegerRV_WIND(MacroContext&, Macro&) {
	// The wind comes from a table indexed by the world's period setting (batch 3, Q17), but
	// the table's values aren't known. Both shipped worlds have period 0.
	// TODO: the wind table; check WIND in the original
	return 0;
}

int32_t IntegerRV_SCOR(MacroContext& ctx, Macro& m) {
	// the five score counters (batch 3, Q30)
	ctx.read_arg_separator(m);
	int32_t n = ctx.read_int(m);
	const auto& world = *g_engine_context.world;
	switch (n) {
		case 0: return static_cast<int32_t>(world.hatchery_eggs);
		case 1: return static_cast<int32_t>(world.natural_eggs);
		case 2: return static_cast<int32_t>(world.dead_norns);
		case 3: return static_cast<int32_t>(world.live_norns);
		case 4: return static_cast<int32_t>(world.breeders_score);
	}
	throw Exception(fmt::format("SCOR called with invalid counter {}", n));
}

// The ground level: one height for each 32-pixel strip of the world (batch 3, Q16).
constexpr int32_t GROUND_LEVEL_STRIP_WIDTH = 32;

int32_t IntegerRV_GRND(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t n = ctx.read_int(m);
	const auto& ground = g_engine_context.map->get_groundlevel();
	if (n < 0 || static_cast<size_t>(n) >= ground.size()) {
		throw Exception(fmt::format("GRND called with invalid strip {}", n));
	}
	return static_cast<int32_t>(ground[static_cast<size_t>(n)]);
}

int32_t IntegerRV_GNDpound(MacroContext&, Macro&) {
	return static_cast<int32_t>(g_engine_context.map->get_groundlevel().size());
}

int32_t IntegerRV_GNDW(MacroContext&, Macro&) {
	return GROUND_LEVEL_STRIP_WIDTH;
}

void LValue_ACTV(const MacroContext& ctx, const Macro& m, int32_t value) {
	auto new_actv = ActiveFlag(value);
	Object* targ = ctx.get_targ(m);

	switch (new_actv) {
		case ACTV_INACTIVE:
			targ->actv = ACTV_INACTIVE;
			return;
		case ACTV_ACTIVE1:
			targ->actv = ACTV_ACTIVE1;
			return;
		case ACTV_ACTIVE2:
			targ->actv = ACTV_ACTIVE2;
			return;
	}

	throw_exception("Unknown active state {}", new_actv);
}

void LValue_XVEC(const MacroContext& ctx, const Macro& m, int32_t value) {
	Object* o = ctx.get_targ(m);
	auto* veh = o->as_vehicle();
	if (!veh) {
		throw_exception("Called XVEC on non-vehicle object: {:?}", *o);
	}
	veh->xvel = value / 256.f;
}

void LValue_YVEC(const MacroContext& ctx, const Macro& m, int32_t value) {
	Object* o = ctx.get_targ(m);
	auto* veh = o->as_vehicle();
	if (!veh) {
		throw_exception("Called YVEC on non-vehicle object: {:?}", *o);
	}
	veh->yvel = value / 256.f;
}


// Bitwise maths

void Command_ANDV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t right = ctx.read_int(m);

	int32_t left;
	if (!ctx.try_get_variable(m, varname, &left)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}
	ctx.set_variable(m, varname, left & right);
	ctx.read_command_separator(m);
}

void Command_ORRV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token varname = ctx.read_token(m);
	ctx.read_arg_separator(m);
	int32_t right = ctx.read_int(m);

	int32_t left;
	if (!ctx.try_get_variable(m, varname, &left)) {
		throw Exception(fmt::format("Unknown variable {:?}", varname));
	}
	ctx.set_variable(m, varname, left | right);
	ctx.read_command_separator(m);
}

// Subroutines. Creatures 1 macros only store one return address, so GSUB can't nest.

void Command_GSUB(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	std::string label = ctx.read_filename_token(m);
	ctx.read_command_separator(m);

	if (!m.subroutine_label.empty()) {
		throw Exception(fmt::format("GSUB {:?} called while already in subroutine {:?}", label, m.subroutine_label));
	}

	const auto return_address = m.ip;
    m.ip = 0;
    bool found = false;
    try {
        while (m.ip < m.script.size()) {
            if (next_syntax_token(ctx, m) != Token("subr")) continue;
            ctx.read_arg_separator(m);
            if (ctx.read_filename_token(m) != label) continue;
            if (m.ip < m.script.size() && m.script[m.ip] != ',' && m.script[m.ip] != ' ') continue;
            found = true;
            break;
        }
    } catch (const Exception&) {
        // The missing label is reported below with its name.
    }
    if (!found) {
        m.ip = return_address;
        throw Exception(fmt::format("GSUB couldn't find SUBR {:?}", label));
    }
    const auto target_address = m.ip;
    m.ip = return_address;
	m.subroutine_label = label;
	m.subroutine_address = m.ip;
	m.ip = target_address;
	ctx.read_command_separator(m);
}

void Command_SUBR(MacroContext&, Macro& m) {
	// Reaching a SUBR without a GSUB ends the script, like STOP. E.g. the coconut tree's
	// script runs into "subr frut" after its main body.
	m.ip -= 4;
	m.destroy_as_soon_as_possible = true;
}

void Command_RETN(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	if (m.subroutine_label.empty()) {
		throw Exception("RETN called outside of a subroutine");
	}
	m.ip = m.subroutine_address;
	m.subroutine_label.clear();
	m.subroutine_address = 0;
}

// Scripts

void Command_SCRX(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t family = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t genus = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t species = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t eventno = ctx.read_int(m);
	ctx.read_command_separator(m);

	g_engine_context.scriptorium->remove(family, genus, species, eventno);
}

// The version reported by the GOG release ("Creatures Build 47" in Version.txt). Scripts
// only check it against small numbers, e.g. "vrsn 2".
constexpr int32_t CREATURES1_ENGINE_VERSION = 47;

void Command_VRSN(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t required = ctx.read_int(m);
	if (CREATURES1_ENGINE_VERSION < required) {
		m.destroy_as_soon_as_possible = true;
		return;
	}
	ctx.read_command_separator(m);
}

// Debugging output

void Command_DBUG(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t value = ctx.read_int(m);
	ctx.read_command_separator(m);
	fmt::print("DBUG {}\n", value);
}

void Command_DBGM(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	std::string message = ctx.read_bracket_string(m);
	ctx.read_command_separator(m);
	fmt::print("DBGM {}\n", message);
}

void Command_DBGV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t value = ctx.read_int(m);
	ctx.read_command_separator(m);
	fmt::print("DBGV {}\n", value);
}

// Objects

void Command_NEWcolon(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token subcommand = ctx.read_token(m);

	if (subcommand == Token("simp")) {
		ctx.read_arg_separator(m);
		std::string sprite = ctx.read_filename_token(m);
		ctx.read_arg_separator(m);
		int32_t image_count = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t first_image = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t plane = ctx.read_int(m);
		ctx.read_arg_separator(m);
		ctx.read_int(m); // clone: not implemented, objects never share sprite data anyway
		ctx.read_command_separator(m);

		const auto& gallery = g_engine_context.images->get_image(sprite, first_image, image_count, ImageManager::IMAGE_SPR);
		auto handle = g_engine_context.objects->add<SimpleObject>();
		auto* obj = static_cast<SimpleObject*>(g_engine_context.objects->try_get(handle));
		obj->family = obj->genus = obj->species = 0;
		obj->movement_status = MOVEMENT_AUTONOMOUS;
		obj->attr = 0;
		obj->limit = Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
		obj->actv = ACTV_INACTIVE;
		obj->tick_value = 0;
		obj->ticks_since_last_tick_event = 0;
		obj->obv0 = obj->obv1 = obj->obv2 = 0;
		obj->z_order = plane;
		obj->click_bhvr = {-1, -1, -1};
		obj->touch_bhvr = 0;
		obj->part = DullPart(gallery, 0, 0, 0, plane);

        obj->unplaced = true;
        m.pending_creation = handle;
		m.targ = handle;
		m.part = 0;

	} else if (subcommand == Token("comp") || subcommand == Token("vhcl")) {
		// NEW: COMP sprite image_count first_image clone, NEW: VHCL sprite image_count first_image
		ctx.read_arg_separator(m);
		std::string sprite = ctx.read_filename_token(m);
		ctx.read_arg_separator(m);
		int32_t image_count = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t first_image = ctx.read_int(m);
		if (subcommand == Token("comp")) {
			ctx.read_arg_separator(m);
			ctx.read_int(m); // clone: not implemented
		}
		ctx.read_command_separator(m);

		const auto& gallery = g_engine_context.images->get_image(sprite, first_image, image_count, ImageManager::IMAGE_SPR);
		ObjectHandle handle = subcommand == Token("vhcl") ? g_engine_context.objects->add<Vehicle>() : g_engine_context.objects->add<CompoundObject>();
		auto* obj = static_cast<CompoundObject*>(g_engine_context.objects->try_get(handle));
		obj->family = obj->genus = obj->species = 0;
		obj->movement_status = MOVEMENT_AUTONOMOUS;
		obj->attr = 0;
		obj->limit = Rect2i{0, 0, CREATURES1_WORLD_WIDTH, CREATURES1_WORLD_HEIGHT};
		obj->actv = ACTV_INACTIVE;
		obj->tick_value = 0;
		obj->ticks_since_last_tick_event = 0;
		obj->obv0 = obj->obv1 = obj->obv2 = 0;
		obj->hotspots.fill(Rect2i{-1, -1, 0, 0});
		obj->functions_to_hotspots.fill(-1);
		obj->new_part_gallery = gallery;
		if (auto* veh = obj->as_vehicle()) {
			veh->xvel = veh->yvel = 0;
			veh->cabin = {};
			veh->bump = 0;
		}

        obj->unplaced = true;
        m.pending_creation = handle;
		m.targ = handle;
		m.part = 0;

	} else if (subcommand == Token("part")) {
		// NEW: PART part x y first_image plane - x and y are relative to part 0
		ctx.read_arg_separator(m);
		int32_t partno = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t x = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t y = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t first_image = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t plane = ctx.read_int(m);
		ctx.read_command_separator(m);

		auto* comp = ctx.get_targ(m)->as_compound_object();
		if (!comp) {
			throw Exception("NEW: PART called on an object that isn't a compound object");
		}
		if (partno < 0 || partno > 9 || numeric_cast<size_t>(partno) > comp->parts.size()) {
			throw Exception(fmt::format("NEW: PART {} out of order (object has {} parts)", partno, comp->parts.size()));
		}
		float basex = comp->parts.empty() ? 0 : comp->parts[0].x();
		float basey = comp->parts.empty() ? 0 : comp->parts[0].y();
		CompoundPart part;
		static_cast<DullPart&>(part) = DullPart(comp->new_part_gallery, first_image, basex + x, basey + y, plane);
		part.relx = x;
		part.rely = y;
		if (numeric_cast<size_t>(partno) == comp->parts.size()) {
			comp->parts.push_back(std::move(part));
		} else {
			comp->parts[numeric_cast<size_t>(partno)] = std::move(part);
		}

	} else if (subcommand == Token("crea")) {
		// NEW: CREA genome_moniker sex: a creature from a genome (batch 2, Q18)
		ctx.read_arg_separator(m);
		int32_t genome = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t sex = ctx.read_int(m);
		ctx.read_command_separator(m);
		m.targ = create_creature(moniker_from_integer(genome), sex);

	} else if (subcommand == Token("gene")) {
		// NEW: GENE mum dad variable: a child genome (dad 0 for a straight copy) (batch 2, Q18, Q21)
		ctx.read_arg_separator(m);
		int32_t a = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t b = ctx.read_int(m);
		ctx.read_arg_separator(m);
		Token variable = ctx.read_token(m);
		ctx.read_command_separator(m);
		std::string child = make_child_genome(moniker_from_integer(a), b ? moniker_from_integer(b) : "");
		if (child.empty()) {
			throw Exception(fmt::format("NEW: GENE couldn't find genome {:?}", moniker_from_integer(a)));
		}
		ctx.set_variable(m, variable, moniker_to_integer(child));

	} else {
		throw UnknownMacroCommand(fmt::format("Unknown command 'new: {:?}'", subcommand));
	}
}

int32_t IntegerRV_TOKN(MacroContext& ctx, Macro& m) {
	// a four-letter word as an integer, e.g. a genome moniker
	ctx.read_arg_separator(m);
	return moniker_to_integer(ctx.read_filename_token(m));
}

void Command_MATE(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	if (auto* creature = ctx.get_targ(m)->as_creature()) {
		creature->mate();
	}
}

void Command_SPOT(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t spotno = ctx.read_int(m);
	sfc::WinRect rect;
	ctx.read_arg_separator(m);
	rect.left = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.top = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.right = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.bottom = ctx.read_int(m);
	ctx.read_command_separator(m);

	auto* comp = ctx.get_targ(m)->as_compound_object();
	if (!comp) {
		throw Exception("SPOT called on an object that isn't a compound object");
	}
	if (spotno < 0 || spotno >= numeric_cast<int32_t>(comp->hotspots.size())) {
		throw Exception(fmt::format("SPOT called with invalid hotspot {}", spotno));
	}
	comp->hotspots[numeric_cast<size_t>(spotno)] = rect;
}

void Command_KNOB(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t function = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t spotno = ctx.read_int(m);
	ctx.read_command_separator(m);

	auto* comp = ctx.get_targ(m)->as_compound_object();
	if (!comp) {
		throw Exception("KNOB called on an object that isn't a compound object");
	}
	if (function < 0 || function >= numeric_cast<int32_t>(comp->functions_to_hotspots.size())) {
		throw Exception(fmt::format("KNOB called with invalid function {}", function));
	}
	comp->functions_to_hotspots[numeric_cast<size_t>(function)] = spotno;
}

void Command_CABN(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	sfc::WinRect rect;
	ctx.read_arg_separator(m);
	rect.left = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.top = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.right = ctx.read_int(m);
	ctx.read_arg_separator(m);
	rect.bottom = ctx.read_int(m);
	ctx.read_command_separator(m);

	auto* veh = ctx.get_targ(m)->as_vehicle();
	if (!veh) {
		throw Exception("CABN called on an object that isn't a vehicle");
	}
	veh->cabin = rect;
}

void Command_PRLD(MacroContext& ctx, Macro& m) {
	// preloads an animation's sprites: only needed for speed on 1990s computers
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	ctx.read_bracket_string(m);
	ctx.read_command_separator(m);
}

void Command_KILL(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	ObjectHandle handle = ctx.read_object(m);
	ctx.read_command_separator(m);

	auto* obj = g_engine_context.objects->try_get(handle);
	if (!obj) {
		throw Exception("KILL called on an invalid object");
	}
	if (obj->as_pointer_tool()) {
		throw Exception("KILL can't destroy the pointer");
	}
	g_engine_context.macros->delete_macros_owned_by(handle);
	g_engine_context.objects->destroy_later(handle);
}

void Command_BHVR(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t click = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t touch = ctx.read_int(m);
	ctx.read_command_separator(m);

	auto* simp = ctx.get_targ(m)->as_simple_object();
	if (!simp) {
		throw Exception("BHVR called on an object that isn't a simple object");
	}
	// The message sent when clicked, indexed by the object's current ACTV. Matches the
	// click behaviours saved in the original game's worlds.
	switch (click) {
		case 0: simp->click_bhvr = {-1, -1, -1}; break; // clicks do nothing
		case 1: simp->click_bhvr = {MESSAGE_ACTIVATE1, -1, -1}; break; // monostable
		case 2: simp->click_bhvr = {MESSAGE_ACTIVATE1, MESSAGE_ACTIVATE1, -1}; break; // retriggerable
		case 3: simp->click_bhvr = {MESSAGE_ACTIVATE1, MESSAGE_DEACTIVATE, -1}; break; // toggle
		case 4: simp->click_bhvr = {MESSAGE_ACTIVATE1, MESSAGE_ACTIVATE2, MESSAGE_DEACTIVATE}; break; // cycle
		default: throw Exception(fmt::format("BHVR called with unknown click behaviour {}", click));
	}
	simp->touch_bhvr = numeric_cast<uint8_t>(touch);
}

void Command_RTAR(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t family = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t genus = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t species = ctx.read_int(m);
	ctx.read_command_separator(m);

	std::vector<ObjectHandle> matches;
	for (auto* obj : *g_engine_context.objects) {
		if (enumerable(obj, family, genus, species)) {
			matches.push_back(obj->uid);
		}
	}
	// no match makes TARG null (batch 5, Q3)
	m.targ = matches.empty() ? ObjectHandle{} : matches[numeric_cast<size_t>(rand_int32(0, numeric_cast<int32_t>(matches.size()) - 1))];
}

void Command_MCRT(MacroContext& ctx, Macro& m) {
	// For any object: freed from any carrier, its limits removed, moved (a creature's ground
	// point, an object's top-left corner), then made free with its room limits worked out
	// again. It isn't put on a floor, and MCRT doesn't end the script's turn (batch 5, Q2;
	// batch 6, Q1c).
	ctx.instructions_left_this_tick++;
	auto* targ = ctx.get_targ(m);
	targ->carrier = {};
	targ->movement_status = MOVEMENT_FLOATING;
	targ->limit = Rect2i{-100000, -100000, 200000, 200000};
	Command_MVTO(ctx, m);
	targ->movement_status = MOVEMENT_AUTONOMOUS;
	if (auto* creature = targ->as_creature()) {
		creature->update_limits();
		creature->pushed_by_limit = false;
	} else {
		auto bbox = targ->get_bbox();
		targ->limit = g_engine_context.map->limits_for(bbox.x, bbox.bottom(), targ->limit);
	}
}

static Creature* get_targ_creature(MacroContext& ctx, Macro& m, const char* command) {
	auto* creature = ctx.get_targ(m)->as_creature();
	if (!creature) {
		throw Exception(fmt::format("{} called on an object that isn't a creature", command));
	}
	return creature;
}

void Command_APPR(MacroContext& ctx, Macro& m) {
	// Walks towards the target point and waits, checking each tick, until the ground point is
	// within 55 px of it horizontally. Never gives up; the script is replaced by a new decision
	// or attention change instead (spec Q10).
	auto old_ip = m.ip - 4;
	auto* creature = get_targ_creature(ctx, m, "APPR");
	if (creature->has_target && std::abs(creature->ground_x() - creature->target_x) < 55) {
		creature->pose_animation.clear();
		ctx.read_command_separator(m);
		return;
	}
	if (!creature->has_pose_animation()) {
		creature->walk();
	}
	m.ip = old_ip;
}

void Command_AIMcolon(MacroContext& ctx, Macro& m) {
	// which function (0 activate 1, 1 activate 2, 2 deactivate) of _IT_ to aim at
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t action = ctx.read_int(m);
	ctx.read_command_separator(m);
	auto* creature = get_targ_creature(ctx, m, "AIM:");
	creature->aim_action = action;
	creature->update_target_point();
}

void Command_IMPT(MacroContext& ctx, Macro& m) {
	// the current action's importance: each brain step adds twice this to the current decision (spec Q7)
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t importance = ctx.read_int(m);
	ctx.read_command_separator(m);
	get_targ_creature(ctx, m, "IMPT")->importance = importance;
}

void Command_TOUC(MacroContext& ctx, Macro& m) {
	// Reaches for the target point and waits until the reaching pose is done (spec Q11).
	auto old_ip = m.ip - 4;
	auto* creature = get_targ_creature(ctx, m, "TOUC");
	auto* it = g_engine_context.objects->try_get(creature->attention);
	const float dx = creature->has_target ? std::abs(creature->target_x - creature->ground_x()) : 1e9f;
	if (!it || dx >= 55 || creature->target_y > creature->ground_y()) {
		// failed: stop the script and forget the decision
		if (std::getenv("OPENCREATURES1_TRACE_MEALS") && m.event == 17 && m.classifier[0] == 2 && m.classifier[1] == 6) {
			fmt::print("TRACE [Meal] {} touc failed: {}, dx {:.0f}, target y {:.0f} vs ground y {:.0f}\n", creature->moniker, it ? "in reach?" : "no _IT_", dx, creature->target_y, creature->ground_y());
		}
		if (creature->brain) {
			brain_clear_neuron(*creature->brain, LOBE_DECISION, static_cast<uint32_t>(creature->current_decision));
		}
		m.ip = old_ip;
		m.destroy_as_soon_as_possible = true;
		return;
	}
	if (it->carrier == creature->uid) {
		ctx.read_command_separator(m);
		return;
	}
	// reach pose = height level + 4 * reach level, each step 18 px
	const int32_t height = std::clamp(numeric_cast<int32_t>((creature->ground_y() - creature->target_y) / 18), 0, 3);
	const int32_t reach = std::clamp(numeric_cast<int32_t>(dx / 18), 0, 2);
	const int32_t pose = height + 4 * reach;
	if (creature->pose_number != pose || creature->has_pose_animation()) {
		creature->pose_animation.clear();
		creature->set_pose_number(pose);
	}
	if (creature->pose_reached()) {
		ctx.read_command_separator(m);
		return;
	}
	m.ip = old_ip;
}

void Command_SAYN(MacroContext& ctx, Macro& m) {
	// the creature says its strongest need out loud (batch 2, Q23)
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	get_targ_creature(ctx, m, "SAYN")->say_need();
}

void Command_DONE(MacroContext& ctx, Macro& m) {
	// marks the action finished; the script carries on (spec Q8)
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	get_targ_creature(ctx, m, "DONE")->done();
}

void Command_LTCY(MacroContext& ctx, Macro& m) {
	// sets involuntary action n's latency to a random number of brain steps (spec Q24)
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t action = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t min = ctx.read_int(m);
	ctx.read_arg_separator(m);
	int32_t max = ctx.read_int(m);
	ctx.read_command_separator(m);
	auto* creature = get_targ_creature(ctx, m, "LTCY");
	if (action < 0 || action >= 8) {
		throw Exception(fmt::format("LTCY called with invalid involuntary action {}", action));
	}
	creature->involuntary_latency[static_cast<size_t>(action)] = rand_int32(std::min(min, max), std::max(min, max));
}

void Command_DROP(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	auto* targ = ctx.get_targ(m);
	if (auto* creature = targ->as_creature()) {
		creature->drop_carried_objects();
		return;
	}
	for (auto* obj : *g_engine_context.objects) {
		if (obj->carrier == targ->uid) {
			g_engine_context.messages->mesg_writ(targ->uid, obj->uid, MESSAGE_DROP);
		}
	}
}

void Command_ASLP(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t asleep = ctx.read_int(m);
	ctx.read_command_separator(m);
	auto* creature = get_targ_creature(ctx, m, "ASLP");
	creature->asleep = asleep ? 1 : 0;
	creature->eyes_open = asleep ? 0 : 1;
}

void Command_DREA(MacroContext& ctx, Macro& m) {
	// dreaming while asleep, when the brain sorts out what it learned
	// TODO: the brain isn't implemented yet
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	int32_t dreaming = ctx.read_int(m);
	ctx.read_command_separator(m);
	get_targ_creature(ctx, m, "DREA")->dreaming = numeric_cast<uint32_t>(dreaming);
}

void Command_SNEZ(MacroContext& ctx, Macro& m) {
	// sneezing and coughing spread bacteria (batch 2, Q15)
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	get_targ_creature(ctx, m, "SNEZ")->sneeze();
}

int32_t IntegerRV_CHEM(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t chemical = ctx.read_int(m);
	auto* creature = get_targ_creature(ctx, m, "CHEM");
	if (chemical < 0 || chemical > 255 || !creature->biochemistry) {
		throw Exception(fmt::format("CHEM called with invalid chemical {}", chemical));
	}
	return creature->biochemistry->chemicals[static_cast<size_t>(chemical)].concentration;
}

int32_t IntegerRV_DRIV(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	int32_t drive = ctx.read_int(m);
	auto* creature = get_targ_creature(ctx, m, "DRIV");
	if (drive < 0 || drive >= numeric_cast<int32_t>(creature->loci.drives.size())) {
		throw Exception(fmt::format("DRIV called with invalid drive {}", drive));
	}
	return creature->loci.drives[static_cast<size_t>(drive)];
}

int32_t IntegerRV_DRVexclamation(MacroContext& ctx, Macro& m) {
	return creature_highest_drive(get_targ_creature(ctx, m, "DRV!")->loci);
}

int32_t IntegerRV_DEAD(MacroContext& ctx, Macro& m) {
	return get_targ_creature(ctx, m, "DEAD")->dead;
}

int32_t IntegerRV_ASLP(MacroContext& ctx, Macro& m) {
	return get_targ_creature(ctx, m, "ASLP")->asleep;
}

int32_t IntegerRV_CAGE(MacroContext& ctx, Macro& m) {
	// TARG's life stage (0 baby ... 6 senile); the kiss script needs 2 or more (batch 7, Q4)
	return get_targ_creature(ctx, m, "CAGE")->age;
}

int32_t IntegerRV_BABY(MacroContext& ctx, Macro& m) {
	// the moniker of the genome the creature is carrying (batch 2, Q9)
	auto* creature = get_targ_creature(ctx, m, "BABY");
	return creature->zygote.empty() ? 0 : moniker_to_integer(creature->zygote);
}

void LValue_BABY(const MacroContext& ctx, const Macro& m, int32_t value) {
	auto* creature = ctx.get_targ(m)->as_creature();
	if (!creature) {
		throw Exception("BABY set on an object that isn't a creature");
	}
	creature->zygote = value ? moniker_from_integer(value) : "";
}

void Command_WALK(MacroContext& ctx, Macro& m) {
	// starts the creature's walking animation
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);
	auto* creature = ctx.get_targ(m)->as_creature();
	if (!creature) {
		throw Exception("WALK called on an object that isn't a creature");
	}
	creature->walk();
}

void Command_SLIM(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);

	// Makes TARG free (movs 0) and works out its room limits, by the same room lookup as
	// everything else (batch 5, Q1). It isn't moved.
	auto* targ = ctx.get_targ(m);
	targ->movement_status = MOVEMENT_AUTONOMOUS;
	auto bbox = targ->get_bbox();
	targ->limit = g_engine_context.map->limits_for(bbox.x, bbox.bottom(), targ->limit);
}

void Command_DDEcolon(MacroContext& ctx, Macro& m) {
	// DDE: commands talk to the kits. The kits are part of the engine here, so only the
	// commands world scripts use matter: the score counters (batch 3, Q12 and Q30).
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token subcommand = ctx.read_token(m);
	auto& world = *g_engine_context.world;
	if (subcommand == Token("hatc")) {
		world.hatchery_eggs++;
	} else if (subcommand == Token("negg")) {
		world.natural_eggs++;
		if (auto* egg = g_engine_context.objects->try_get(m.targ); egg && egg->family == 2 && egg->genus == 5 && egg->species == 2) {
			auto* owner = g_engine_context.objects->try_get(m.ownr);
			auto* mother = owner ? owner->as_creature() : nullptr;
			fmt::print("INFO [Life] Laid egg {}: mother {}, world tick {}\n", moniker_from_integer(egg->obv0), mother ? mother->moniker : "unknown", world.tick);
		}
	} else if (subcommand == Token("died")) {
		world.dead_norns++;
		if (world.live_norns > 0) {
			world.live_norns--;
		}
	} else if (subcommand == Token("live")) {
		world.live_norns++;
	} else {
		throw UnknownMacroCommand(fmt::format("Unknown command 'dde: {:?}'", subcommand));
	}
	ctx.read_command_separator(m);
}

void Command_EDIT(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_command_separator(m);

	auto* targ = ctx.get_targ(m);
    targ->unplaced = false;
	targ->movement_status = MOVEMENT_MOUSEDRIVEN;
	targ->carrier = g_engine_context.pointer->m_pointer_tool;
	g_engine_context.pointer->m_edited_object = targ->uid;
}

// Event objects, kept in the world file

void Command_EVNT(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	ObjectHandle handle = ctx.read_object(m);
	ctx.read_command_separator(m);

	// the 4 most recent events, shown on the status bar (batch 3, Q31)
	auto& events = g_engine_context.world->events;
	if (std::find(events.begin(), events.end(), handle) == events.end()) {
		events.push_back(handle);
		if (events.size() > MAX_EVENTS) {
			events.erase(events.begin());
		}
	}
}

void Command_RMEV(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	ObjectHandle handle = ctx.read_object(m);
	ctx.read_command_separator(m);

	auto& events = g_engine_context.world->events;
	events.erase(std::remove(events.begin(), events.end(), handle), events.end());
	// a dead creature goes on death row, which is how the Funeral Kit learns of deaths
	// (batch 3, Q31)
	auto* obj = g_engine_context.objects->try_get(handle);
	if (auto* creature = obj ? obj->as_creature() : nullptr; creature && creature->dead) {
		auto& death_row = g_engine_context.world->death_row;
		if (death_row.size() < MAX_DEATH_ROW) {
			death_row.push_back(handle);
		}
	}
}

// System

void Command_SYScolon(MacroContext& ctx, Macro& m) {
	ctx.instructions_left_this_tick++;
	ctx.read_arg_separator(m);
	Token subcommand = ctx.read_token(m);

	if (subcommand == Token("cmra")) {
		ctx.read_arg_separator(m);
		int32_t x = ctx.read_int(m);
		ctx.read_arg_separator(m);
		int32_t y = ctx.read_int(m);
		ctx.read_command_separator(m);
		g_engine_context.viewport->set_scroll_position(x, y);

	} else if (subcommand == Token("camt")) {
		// centre the camera on TARG
		ctx.read_command_separator(m);
		auto bbox = ctx.get_targ(m)->get_bbox();
		const auto& camera = g_engine_context.viewport->get_main_camera_rect();
		g_engine_context.viewport->set_scroll_position(
			numeric_cast<int32_t>(bbox.x + bbox.width / 2) - camera.width / 2,
			numeric_cast<int32_t>(bbox.y + bbox.height / 2) - camera.height / 2);

	} else if (subcommand == Token("wtop")) {
		// bring the game window to the front: the applets used this, nothing to do here
		ctx.read_command_separator(m);

	} else {
		throw UnknownMacroCommand(fmt::format("Unknown command 'sys: {:?}'", subcommand));
	}
}

// Values

// CARR, like OBJP, is the script owner's: the egg's hatching script uses "targ carr" to find
// its incubator again while TARG is the newborn creature.
ObjectHandle AgentRV_CARR(MacroContext& ctx, Macro& m) {
	return ctx.get_ownr(m)->carrier;
}

ObjectHandle AgentRV_NORN(MacroContext&, Macro&) {
	return g_engine_context.world->current_norn;
}

// OBJP belongs to the script's owner: the egg scripts run "new: crea ..., setv objp targ"
// with TARG set to the new creature, then use OBJP to find it again later.
ObjectHandle AgentRV_OBJP(MacroContext& ctx, Macro& m) {
	return ctx.get_ownr(m)->objp;
}

int32_t IntegerRV_ATTR(MacroContext& ctx, Macro& m) {
	return ctx.get_targ(m)->attr;
}

int32_t IntegerRV_CLAS(MacroContext& ctx, Macro& m) {
	auto* targ = ctx.get_targ(m);
	return (targ->family << 24) | (targ->genus << 16) | (targ->species << 8);
}

int32_t IntegerRV_FMLY(MacroContext& ctx, Macro& m) {
	return ctx.get_targ(m)->family;
}

int32_t IntegerRV_GNUS(MacroContext& ctx, Macro& m) {
	return ctx.get_targ(m)->genus;
}

int32_t IntegerRV_MOVS(MacroContext& ctx, Macro& m) {
	return ctx.get_targ(m)->movement_status;
}

int32_t IntegerRV_SPCS(MacroContext& ctx, Macro& m) {
	return ctx.get_targ(m)->species;
}

int32_t IntegerRV_TOUC(MacroContext& ctx, Macro& m) {
	ctx.read_arg_separator(m);
	auto* first = g_engine_context.objects->try_get(ctx.read_object(m));
	ctx.read_arg_separator(m);
	auto* second = g_engine_context.objects->try_get(ctx.read_object(m));
	if (!first || !second) {
		throw Exception("TOUC called with an invalid object");
	}
	auto a = first->get_bbox();
	auto b = second->get_bbox();
	// the overlap must have area, and x wraps round the world (batch 5, Q3)
	const float wrap = numeric_cast<float>(CREATURES1_WORLD_WIDTH);
	bool touching = false;
	for (float shift : {0.0f, -wrap, wrap}) {
		touching = touching || (a.x + shift < b.right() && b.x < a.right() + shift && a.y < b.bottom() && b.y < a.bottom());
	}
	return touching ? 1 : 0;
}

void LValue_ATTR(const MacroContext& ctx, const Macro& m, int32_t value) {
	ctx.get_targ(m)->attr = numeric_cast<uint8_t>(value);
}

void LValue_CLAS(const MacroContext& ctx, const Macro& m, int32_t value) {
	// family, genus and species packed into the top three bytes
	auto* targ = ctx.get_targ(m);
	auto clas = static_cast<uint32_t>(value);
	targ->family = static_cast<uint8_t>(clas >> 24);
	targ->genus = static_cast<uint8_t>(clas >> 16);
	targ->species = static_cast<uint8_t>(clas >> 8);
}

void LValue_NORN(const MacroContext&, const Macro&, int32_t value) {
	g_engine_context.world->current_norn = integer_to_object(value);
}

void LValue_OBJP(const MacroContext& ctx, const Macro& m, int32_t value) {
	ctx.get_ownr(m)->objp = integer_to_object(value);
}

void MacroCommands::install_math_commands(MacroContext& ctx) {
	ctx.command_funcs[Token("addv")] = Command_ADDV;
	ctx.command_funcs[Token("divv")] = Command_DIVV;
	ctx.command_funcs[Token("mulv")] = Command_MULV;
	ctx.command_funcs[Token("negv")] = Command_NEGV;
	ctx.command_funcs[Token("subv")] = Command_SUBV;
	ctx.command_funcs[Token("andv")] = Command_ANDV;
	ctx.command_funcs[Token("orrv")] = Command_ORRV;
	ctx.command_funcs[Token("modv")] = Command_MODV;
	ctx.command_funcs[Token("rndv")] = Command_RNDV;
}

void MacroCommands::install_default_commands(MacroContext& ctx) {
	install_math_commands(ctx);

	ctx.command_funcs[Token("anim")] = Command_ANIM;
	ctx.command_funcs[Token("snez")] = Command_SNEZ;
	ctx.command_funcs[Token("drea")] = Command_DREA;
	ctx.command_funcs[Token("aslp")] = Command_ASLP;
	ctx.command_funcs[Token("drop")] = Command_DROP;
	ctx.command_funcs[Token("ltcy")] = Command_LTCY;
	ctx.command_funcs[Token("mate")] = Command_MATE;
	// the shipped kiss script (0 0 0 event 17) spells it f**k (batch 7, Q4)
	ctx.command_funcs[Token("f**k")] = Command_MATE;
	ctx.command_funcs[Token("k**f")] = Command_MATE;
	ctx.command_funcs[Token("sayn")] = Command_SAYN;
	ctx.command_funcs[Token("touc")] = Command_TOUC;
	ctx.command_funcs[Token("impt")] = Command_IMPT;
	ctx.command_funcs[Token("aim:")] = Command_AIMcolon;
	ctx.command_funcs[Token("appr")] = Command_APPR;
	ctx.command_funcs[Token("base")] = Command_BASE;
	ctx.command_funcs[Token("bbd:")] = Command_BBDcolon;
	ctx.command_funcs[Token("bhvr")] = Command_BHVR;
	ctx.command_funcs[Token("cabn")] = Command_CABN;
	ctx.command_funcs[Token("dbgm")] = Command_DBGM;
	ctx.command_funcs[Token("dbgv")] = Command_DBGV;
	ctx.command_funcs[Token("dbug")] = Command_DBUG;
	ctx.command_funcs[Token("dde:")] = Command_DDEcolon;
	ctx.command_funcs[Token("dpas")] = Command_DPAS;
	ctx.command_funcs[Token("doif")] = Command_DOIF;
	ctx.command_funcs[Token("done")] = Command_DONE;
	ctx.command_funcs[Token("else")] = Command_ELSE;
	ctx.command_funcs[Token("edit")] = Command_EDIT;
	ctx.command_funcs[Token("endi")] = Command_ENDI;
	ctx.command_funcs[Token("enum")] = Command_ENUM;
	ctx.command_funcs[Token("ever")] = Command_EVER;
	ctx.command_funcs[Token("evnt")] = Command_EVNT;
	ctx.command_funcs[Token("fade")] = Command_FADE;
	ctx.command_funcs[Token("gpas")] = Command_GPAS;
	ctx.command_funcs[Token("gsub")] = Command_GSUB;
	ctx.command_funcs[Token("inst")] = Command_INST;
	ctx.command_funcs[Token("kill")] = Command_KILL;
	ctx.command_funcs[Token("knob")] = Command_KNOB;
	ctx.command_funcs[Token("loop")] = Command_LOOP;
	ctx.command_funcs[Token("mcrt")] = Command_MCRT;
	ctx.command_funcs[Token("mesg")] = Command_MESG;
	ctx.command_funcs[Token("mvby")] = Command_MVBY;
	ctx.command_funcs[Token("mvto")] = Command_MVTO;
	ctx.command_funcs[Token("new:")] = Command_NEWcolon;
	ctx.command_funcs[Token("next")] = Command_NEXT;
	ctx.command_funcs[Token("over")] = Command_OVER;
	ctx.command_funcs[Token("part")] = Command_PART;
	ctx.command_funcs[Token("pose")] = Command_POSE;
	ctx.command_funcs[Token("prld")] = Command_PRLD;
	ctx.command_funcs[Token("repe")] = Command_REPE;
	ctx.command_funcs[Token("reps")] = Command_REPS;
	ctx.command_funcs[Token("retn")] = Command_RETN;
	ctx.command_funcs[Token("rmev")] = Command_RMEV;
	ctx.command_funcs[Token("rtar")] = Command_RTAR;
	ctx.command_funcs[Token("scrx")] = Command_SCRX;
	ctx.command_funcs[Token("setv")] = Command_SETV;
	ctx.command_funcs[Token("slim")] = Command_SLIM;
	ctx.command_funcs[Token("sndc")] = Command_SNDC;
	ctx.command_funcs[Token("snde")] = Command_SNDE;
	ctx.command_funcs[Token("sndl")] = Command_SNDL;
	ctx.command_funcs[Token("spot")] = Command_SPOT;
	ctx.command_funcs[Token("stim")] = Command_STIM;
	ctx.command_funcs[Token("stm#")] = Command_STMpound;
	ctx.command_funcs[Token("stpc")] = Command_STPC;
	ctx.command_funcs[Token("subr")] = Command_SUBR;
	ctx.command_funcs[Token("sys:")] = Command_SYScolon;
	ctx.command_funcs[Token("targ")] = Command_TARG;
	ctx.command_funcs[Token("tele")] = Command_TELE;
	ctx.command_funcs[Token("tick")] = Command_TICK;
	ctx.command_funcs[Token("untl")] = Command_UNTL;
	ctx.command_funcs[Token("vrsn")] = Command_VRSN;
	ctx.command_funcs[Token("wait")] = Command_WAIT;
	ctx.command_funcs[Token("walk")] = Command_WALK;

	ctx.agentrv_funcs[Token("_it_")] = AgentRV__IT_;
	ctx.agentrv_funcs[Token("carr")] = AgentRV_CARR;
	ctx.agentrv_funcs[Token("from")] = AgentRV_FROM;
	ctx.agentrv_funcs[Token("norn")] = AgentRV_NORN;
	ctx.agentrv_funcs[Token("objp")] = AgentRV_OBJP;
	ctx.agentrv_funcs[Token("ownr")] = AgentRV_OWNR;
	ctx.agentrv_funcs[Token("pntr")] = AgentRV_PNTR;
	ctx.agentrv_funcs[Token("targ")] = AgentRV_TARG;

	ctx.integerrv_funcs[Token("actv")] = IntegerRV_ACTV;
	ctx.integerrv_funcs[Token("tokn")] = IntegerRV_TOKN;
	ctx.integerrv_funcs[Token("baby")] = IntegerRV_BABY;
	ctx.integerrv_funcs[Token("cage")] = IntegerRV_CAGE;
	ctx.integerrv_funcs[Token("aslp")] = IntegerRV_ASLP;
	ctx.integerrv_funcs[Token("dead")] = IntegerRV_DEAD;
	ctx.integerrv_funcs[Token("drv!")] = IntegerRV_DRVexclamation;
	ctx.integerrv_funcs[Token("driv")] = IntegerRV_DRIV;
	ctx.integerrv_funcs[Token("chem")] = IntegerRV_CHEM;
	ctx.integerrv_funcs[Token("attr")] = IntegerRV_ATTR;
	ctx.integerrv_funcs[Token("clas")] = IntegerRV_CLAS;
	ctx.integerrv_funcs[Token("fmly")] = IntegerRV_FMLY;
	ctx.integerrv_funcs[Token("gnus")] = IntegerRV_GNUS;
	ctx.integerrv_funcs[Token("hght")] = IntegerRV_HGHT;
	ctx.integerrv_funcs[Token("limb")] = IntegerRV_LIMB;
	ctx.integerrv_funcs[Token("liml")] = IntegerRV_LIML;
	ctx.integerrv_funcs[Token("limr")] = IntegerRV_LIMR;
	ctx.integerrv_funcs[Token("limt")] = IntegerRV_LIMT;
	ctx.integerrv_funcs[Token("movs")] = IntegerRV_MOVS;
	ctx.integerrv_funcs[Token("posb")] = IntegerRV_POSB;
	ctx.integerrv_funcs[Token("pose")] = IntegerRV_POSE;
	ctx.integerrv_funcs[Token("posl")] = IntegerRV_POSL;
	ctx.integerrv_funcs[Token("posr")] = IntegerRV_POSR;
	ctx.integerrv_funcs[Token("post")] = IntegerRV_POST;
	ctx.integerrv_funcs[Token("spcs")] = IntegerRV_SPCS;
	ctx.integerrv_funcs[Token("totl")] = IntegerRV_TOTL;
	ctx.integerrv_funcs[Token("touc")] = IntegerRV_TOUC;
	ctx.integerrv_funcs[Token("wdth")] = IntegerRV_WDTH;
	ctx.integerrv_funcs[Token("xvec")] = IntegerRV_XVEC;
	ctx.integerrv_funcs[Token("yvec")] = IntegerRV_YVEC;
	ctx.integerrv_funcs[Token("bump")] = IntegerRV_BUMP;
	ctx.integerrv_funcs[Token("wind")] = IntegerRV_WIND;
	ctx.integerrv_funcs[Token("scor")] = IntegerRV_SCOR;
	ctx.integerrv_funcs[Token("grnd")] = IntegerRV_GRND;
	ctx.integerrv_funcs[Token("gnd#")] = IntegerRV_GNDpound;
	ctx.integerrv_funcs[Token("gndw")] = IntegerRV_GNDW;

	ctx.lvalue_funcs[Token("actv")] = LValue_ACTV;
	ctx.lvalue_funcs[Token("attr")] = LValue_ATTR;
	ctx.lvalue_funcs[Token("baby")] = LValue_BABY;
	ctx.lvalue_funcs[Token("clas")] = LValue_CLAS;
	ctx.lvalue_funcs[Token("norn")] = LValue_NORN;
	ctx.lvalue_funcs[Token("objp")] = LValue_OBJP;
	ctx.lvalue_funcs[Token("xvec")] = LValue_XVEC;
	ctx.lvalue_funcs[Token("yvec")] = LValue_YVEC;
}