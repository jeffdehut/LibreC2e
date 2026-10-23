#include "EngineContext.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "Wolfling.h"
#include "WorldState.h"
#include "WorldSystems.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

class WolflingWorld : public testing::Test {
protected:
	fs::path folder;
	Wolfling::Settings rules;
	void SetUp() override {
		g_engine_context.reset();
		folder = fs::temp_directory_path() / ("opencreatures1-wolfling-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(folder);
		std::ofstream(folder / "Creatures.exe"); // identifies only this isolated test folder
		g_engine_context.paths->set_main_directory(folder);
		g_engine_context.world->tick = 1000;
		rules.food = Wolfling::FOOD_NONE;
		rules.population_floor = 0;
		rules.names_by_generation = false;
		rules.snapshot_hours = 0;
		open("A.sfc");
	}
	void TearDown() override {
		open("unused.sfc");
		fs::remove_all(folder);
		set_world_file_name("World.openc1.sfc");
		g_engine_context.reset();
	}
	void open(const std::string& name, bool fresh = false) {
		set_world_file_name(name);
		Wolfling::world_changed(fresh);
	}
};

TEST_F(WolflingWorld, SwitchingAndReopeningWorldsRestoresOnlyTheirOwnRun) {
	Wolfling::start(rules);
	ASSERT_TRUE(Wolfling::hand_locked());
	open("B.sfc");
	EXPECT_FALSE(Wolfling::active());
	EXPECT_FALSE(g_engine_context.pointer->m_locked);
	EXPECT_TRUE(Wolfling::kit_allowed("Health"));
	auto feral = rules;
	feral.variant = Wolfling::VARIANT_FERAL;
	feral.snapshot_hours = 12;
	Wolfling::start(feral);
	open("A.sfc");
	EXPECT_TRUE(Wolfling::active());
	EXPECT_TRUE(g_engine_context.pointer->m_locked);
	EXPECT_FALSE(Wolfling::kit_allowed("Health"));
	EXPECT_EQ(Wolfling::settings().snapshot_hours, 0);
	Wolfling::stop();
	open("B.sfc");
	EXPECT_TRUE(Wolfling::active());
	EXPECT_FALSE(Wolfling::hand_locked());
	EXPECT_EQ(Wolfling::settings().snapshot_hours, 12);
	// Reset the engine as on startup, keeping only the files written by the previous run.
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(folder);
	open("A.sfc");
	EXPECT_FALSE(Wolfling::active());
	open("B.sfc");
	EXPECT_TRUE(Wolfling::active());
	EXPECT_EQ(Wolfling::settings().variant, Wolfling::VARIANT_FERAL);
}

TEST_F(WolflingWorld, SaveAsCopiesRunWithoutChangingTheOriginalWorld) {
	Wolfling::start(rules);
	set_world_file_name("Copy.sfc");
	save_world(get_world_file_name());
	EXPECT_TRUE(fs::exists(folder / "Worlds" / "Copy" / "Copy.sfc")); // saved in its own folder
	open("Copy.sfc");
	ASSERT_TRUE(Wolfling::active());
	Wolfling::stop();
	open("A.sfc");
	EXPECT_TRUE(Wolfling::active());
	open("Copy.sfc");
	EXPECT_FALSE(Wolfling::active());
}

TEST_F(WolflingWorld, NewWorldDoesNotInheritRunFromReusedSaveName) {
	Wolfling::start(rules);
	// New World finalizes the chosen save name before resetting its run.
	open("A.sfc", true);
	EXPECT_FALSE(Wolfling::active());
	EXPECT_FALSE(g_engine_context.pointer->m_locked);
	Wolfling::save_for_world(get_world_file_name());
	open("A.sfc");
	EXPECT_FALSE(Wolfling::active());
}

TEST_F(WolflingWorld, UnidentifiedLegacyRunCannotEnableOtherWorlds) {
	std::ofstream(folder / "Wolfling.openc1.txt") << "active 1\nsettings 0 0 0 0 0\n";
	open("World.openc1.sfc");
	EXPECT_FALSE(Wolfling::active());
	open("New World.sfc");
	EXPECT_FALSE(Wolfling::active());
	EXPECT_TRUE(fs::exists(folder / "Wolfling.openc1.txt"));
}
