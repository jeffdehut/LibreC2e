#include "Macro.h"
#include "PathManager.h"
#include <chrono>
#include <filesystem>
#include <tuple>
#include <vector>
#include <fmt/core.h>

#include "MacroCommands.h"
#include "MacroContext.h"
#include "MacroManager.h"
#include "EngineContext.h"
#include "WorldState.h"
#include "objects/ObjectManager.h"
#include "objects/SimpleObject.h"

#include <gtest/gtest.h>

TEST(Macro, QueuingScriptsDoesNotInvalidateTheRunningScript) {
	g_engine_context.reset();
	auto owner = g_engine_context.objects->add<SimpleObject>();
	auto& macros = *g_engine_context.macros;
	macros.ctx.command_funcs[Token("spwn")] = [](MacroContext& ctx, Macro& m) {
		ctx.read_command_separator(m);
		// Force growth while the interpreter still holds the parent macro by reference.
		for (int i = 0; i < 100; ++i) {
			g_engine_context.macros->add(Macro{"endm"});
		}
	};
	Macro parent{"spwn,setv obv0 42,endm"};
	parent.inst = true;
	parent.ownr = parent.targ = owner;
	macros.add(parent);
	macros.tick();
	EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 42);
	macros.tick();
	EXPECT_EQ(macros.count(), 0u);
	g_engine_context.reset();
}

TEST(Macro, UnexpectedEndOfMacro) {
	MacroContext ctx;
	Macro m;
	EXPECT_THROW(ctx.tick_macro(m), UnexpectedEndOfMacro);
}

TEST(Macro, UnknownCommand) {
	MacroContext ctx;
	Macro m{"foob"};

	EXPECT_THROW(ctx.tick_macro(m), UnknownMacroCommand);
}

TEST(Macro, VarsStartAtZero) {
	Macro m;
	for (auto var : m.vars) {
		EXPECT_EQ(var, 0);
	}
}

TEST(Macro, FullSavedStackRemainsReadableAndCopyable) {
	Macro m;
	for (int32_t i = 0; i < 20; ++i) m.stack.push_back(i);
	// Twenty entries is valid in both the engine and the original SFC macro record.
	EXPECT_NO_THROW({
		const Macro copy = m;
		EXPECT_EQ(copy.stack[19], 19);
	});
	EXPECT_THROW(m.stack.push_back(20), std::length_error);
}

TEST(Macro, Math) {
	MacroContext ctx;
	MacroCommands::install_math_commands(ctx);

	Macro m{"addv var0 5,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], 5);

	m = Macro{"subv var1 3,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[1], -3);

	m = Macro{"rndv var5 8 24,endm"};
	ctx.tick_macro(m);
	EXPECT_GE(m.vars[5], 8);
	EXPECT_LE(m.vars[5], 24);

	// bounds in reverse order, like the bees' "rndv var1 -1 -5"
	for (int i = 0; i < 100; ++i) {
		m = Macro{"rndv var1 -1 -5,endm"};
		ctx.tick_macro(m);
		EXPECT_GE(m.vars[1], -5);
		EXPECT_LE(m.vars[1], -1);
	}
}
TEST(Macro, BitwiseMath) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);

	Macro m{"setv var0 12,andv var0 10,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], 8);

	m = Macro{"setv var0 12,orrv var0 3,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], 15);
}

static void run_to_end(MacroContext& ctx, Macro& m) {
	for (int i = 0; i < 100 && !m.destroy_as_soon_as_possible; ++i) {
		ctx.tick_macro(m);
	}
}

TEST(Macro, Subroutines) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);

	Macro m{"setv var0 1,gsub frut,addv var0 10,stop,subr frut,addv var0 100,retn"};
	run_to_end(ctx, m);
	EXPECT_EQ(m.vars[0], 111);
	EXPECT_TRUE(m.subroutine_label.empty());
}

TEST(Macro, ReachingSubrEndsScript) {
	// Like the coconut tree, whose main body runs into "subr frut".
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);

	Macro m{"setv var0 1,subr frut,addv var0 100,retn,endm"};
	run_to_end(ctx, m);
	EXPECT_EQ(m.vars[0], 1);
}

TEST(Macro, ClasPacksFamilyGenusSpecies) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);

	auto handle = g_engine_context.objects->add<SimpleObject>();
	Macro m{"setv clas 33950208,setv var0 fmly,setv var1 gnus,setv var2 spcs,setv var3 clas,endm"};
	m.targ = handle;
	run_to_end(ctx, m);
	// the coconut tree's coconuts: family 2, genus 6, species 10
	EXPECT_EQ(m.vars[0], 2);
	EXPECT_EQ(m.vars[1], 6);
	EXPECT_EQ(m.vars[2], 10);
	EXPECT_EQ(m.vars[3], 33950208);
	g_engine_context.objects->destroy_later(handle);
	g_engine_context.objects->destroy_pending();
}

TEST(Macro, ScoreCountersFromDDE) {
	// batch 3, Q30: hatchery eggs, natural eggs, deaths, current creatures, creature-time
	g_engine_context.reset();
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	Macro m{"dde: hatc,dde: live,dde: live,dde: negg,dde: died,setv var0 scor 0,setv var1 scor 1,setv var2 scor 2,setv var3 scor 3,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], 1);
	EXPECT_EQ(m.vars[1], 1);
	EXPECT_EQ(m.vars[2], 1);
	EXPECT_EQ(m.vars[3], 1);
	g_engine_context.reset();
}

TEST(Macro, EventListKeepsTheFourMostRecent) {
	// batch 3, Q31
	g_engine_context.reset();
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	std::vector<ObjectHandle> objects;
	for (int i = 0; i < 5; ++i) {
		objects.push_back(g_engine_context.objects->add<SimpleObject>());
		Macro m{"evnt targ,endm"};
		m.ownr = m.targ = objects.back();
		ctx.tick_macro(m);
	}
	auto& events = g_engine_context.world->events;
	ASSERT_EQ(events.size(), 4u);
	EXPECT_EQ(events.front(), objects[1]);
	EXPECT_EQ(events.back(), objects[4]);
	g_engine_context.reset();
}

TEST(Macro, LibreCreaturesSignedDivisionBoundary) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	Macro m{"setv var0 -2147483648,divv var0 -1,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], INT32_MIN);
}

TEST(Macro, LibreCreaturesZeroDivisorKeepsValueAndContinues) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	Macro m{"setv var0 37,divv var0 0,setv var1 99,endm"};
	ctx.tick_macro(m);
	EXPECT_EQ(m.vars[0], 37);
	EXPECT_EQ(m.vars[1], 99);
}

TEST(Macro, LibreCreaturesSignedRemainderAndZeroDivisor) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	for (const auto& item : std::vector<std::tuple<int32_t, int32_t, int32_t>>{{-2147483647 - 1, -1, 0}, {-7, 3, -1}, {7, -3, 1}, {37, 0, 37}}) {
		Macro m{fmt::format("setv var0 {},modv var0 {},endm", std::get<0>(item), std::get<1>(item))};
		EXPECT_NO_THROW(ctx.tick_macro(m));
		EXPECT_EQ(m.vars[0], std::get<2>(item));
	}
}

TEST(Macro, LibreCreaturesArithmeticWrapsAt32Bits) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	for (const auto& item : std::vector<std::pair<std::string, int32_t>>{
		{"setv var0 2147483647,addv var0 1,endm", INT32_MIN},
		{"setv var0 -2147483648,subv var0 1,endm", INT32_MAX},
		{"setv var0 2147483647,mulv var0 2,endm", -2},
		{"setv var0 -2147483648,negv var0,endm", INT32_MIN}}) {
		Macro m{item.first};
		ctx.tick_macro(m);
		EXPECT_EQ(m.vars[0], item.second);
	}
}

TEST(Macro, CorrectedFullDomainRandomBoundsAlreadyWork) {
	MacroContext ctx;
	MacroCommands::install_default_commands(ctx);
	for (int i = 0; i < 256; ++i) {
		Macro full{"rndv var0 -2147483648 2147483647,endm"};
		EXPECT_NO_THROW(ctx.tick_macro(full));
		Macro reversed{"rndv var0 2147483647 -2147483648,endm"};
		EXPECT_NO_THROW(ctx.tick_macro(reversed));
	}
}

TEST(Macro, MissingArtCreationDoesNotLeaveAnObject) {
	g_engine_context.reset();
	const auto root = std::filesystem::temp_directory_path() / ("openc1-new-art-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root / "Images");
	g_engine_context.paths->set_main_directory(root);
	const auto owner = g_engine_context.objects->add<SimpleObject>();
	for (const std::string& command : {"new: simp none 1 0 100 0,endm", "new: comp none 1 0 0,endm", "new: vhcl none 1 0,endm"}) {
		Macro m{command};
		m.targ = m.ownr = owner;
		m.part = 3;
		EXPECT_THROW(g_engine_context.macros->ctx.tick_macro(m), std::exception);
		EXPECT_EQ(g_engine_context.objects->count_classifier(0, 0, 0), 1);
		EXPECT_EQ(m.targ, owner);
		EXPECT_EQ(m.part, 3);
	}
	g_engine_context.objects->tick();
	g_engine_context.reset();
	std::filesystem::remove_all(root);
}

TEST(Macro, CorrectedCompletedScriptDoesNotDelayAdjacentScripts) {
	auto run = [](int ending_position) {
		g_engine_context.reset();
		const auto a = g_engine_context.objects->add<SimpleObject>();
		const auto b = g_engine_context.objects->add<SimpleObject>();
		for (int position = 0; position < 3; ++position) {
			if (position == ending_position) g_engine_context.macros->add(Macro{"endm"});
			if (position < 2) {
				Macro waiting{"wait 2,setv obv0 17,endm"};
				waiting.targ = waiting.ownr = position == 0 ? a : b;
				g_engine_context.macros->add(waiting);
			}
		}
		std::vector<std::pair<int32_t, int32_t>> observations;
		for (int tick = 0; tick < 5; ++tick) {
			g_engine_context.macros->tick();
			observations.emplace_back(g_engine_context.objects->try_get(a)->obv0, g_engine_context.objects->try_get(b)->obv0);
		}
		g_engine_context.reset();
		return observations;
	};
	const auto normal = run(-1);
	ASSERT_EQ(normal.back(), (std::pair<int32_t, int32_t>{17, 17}));
	for (int position : {0, 1, 2}) EXPECT_EQ(run(position), normal);
}
