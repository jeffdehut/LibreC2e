#include "C1SoundManager.h"
#include "CreatureBiochemistry.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "MacroManager.h"
#include "MapManager.h"
#include "MessageManager.h"
#include "PathManager.h"
#include "SFCSerialization.h"
#include "Scriptorium.h"
#include "ViewportManager.h"
#include "WorldState.h"
#include "WorldSystems.h"
#include "common/backend/Backend.h"
#include "common/backend/NullBackend.h"
#include "fileformats/genomeFile.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "fileformats/sfc/SFCFile.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fmt/core.h>
#include <fstream>
#include <gtest/gtest.h>

TEST(Reproduction, PregnancyHormonesRequireAnActivePregnancyLocus) {
	// Batch 7 Q4: progesterone and gonadotrophin are emitted while pregnant,
	// not merely because a digital emitter's threshold happens to be zero.
	sfc::CBiochemistryV1 bio{};
	bio.emitters = {{1, 2, 1, 66, 0, 13, 4, 2}, {1, 2, 1, 65, 0, 40, 64, 2}};
	bio.chemicals[65].decay = bio.chemicals[66].decay = 255;
	CreatureLoci loci;
	uint8_t dead = 0;
	creature_biochemistry_tick(bio, loci, nullptr, dead, 0);
	EXPECT_EQ(bio.chemicals[66].concentration, 0);
	EXPECT_EQ(bio.chemicals[65].concentration, 0);
	loci.pregnant_output = 255;
	creature_biochemistry_tick(bio, loci, nullptr, dead, 520);
	EXPECT_EQ(bio.chemicals[66].concentration, 4);
	EXPECT_EQ(bio.chemicals[65].concentration, 64);
}

TEST(Reproduction, DigitalEmittersRespectTheirBoundaryAndInversion) {
	for (uint8_t threshold : {uint8_t(0), uint8_t(128), uint8_t(255)}) {
		for (bool invert : {false, true}) {
			for (int offset : {-1, 0, 1}) {
				int value = threshold + offset;
				if (value < 0 || value > 255) continue;
				sfc::CBiochemistryV1 bio{};
				bio.chemicals[63].decay = 255;
				bio.emitters = {{1, 2, 0, 63, threshold, 1, 6, static_cast<uint8_t>(invert ? 6 : 2)}};
				CreatureLoci loci;
				loci.fertile_output = static_cast<uint8_t>(invert ? 255 - value : value);
				uint8_t dead = 0;
				creature_biochemistry_tick(bio, loci, nullptr, dead, 0);
				EXPECT_EQ(bio.chemicals[63].concentration, value > threshold ? 6 : 0);
			}
		}
	}
}


class BreedingData : public testing::Test {
protected:
	std::filesystem::path data;
	Creature* mum = nullptr;
	Creature* dad = nullptr;
	void SetUp() override {
		const char* input = std::getenv("OPENC1_BREEDING_TEST_DATA");
		if (!input) GTEST_SKIP() << "Requires a marked scratch copy of original C1 data";
		data = std::filesystem::canonical(input);
		ASSERT_TRUE(std::filesystem::is_regular_file(data / ".breeding-test-scratch"));
		g_engine_context.reset();
		if (!get_backend()) set_backend(NullBackend::get_instance());
		g_engine_context.paths->set_main_directory(data);
		g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
		g_engine_context.images->load_default_palette();
		auto world = sfc::read_sfc_v1_file(data / "Eden.sfc");
		sfc_load_everything(world);
		set_autosave_enabled(false);
		for (auto* obj : *g_engine_context.objects) obj->tick_value = 0;
		// Original genomes/art/scripts are read only from a caller-created scratch copy.
		// Fixed parent names keep the test independent of random crossover/mutation.
		std::filesystem::copy_file(data / "Genetics/mum1.gen", data / "Genetics/tMum.gen", std::filesystem::copy_options::overwrite_existing);
		std::filesystem::copy_file(data / "Genetics/dad1.gen", data / "Genetics/tDad.gen", std::filesystem::copy_options::overwrite_existing);
		mum = g_engine_context.objects->try_get(create_creature("tMum", 2))->as_creature();
		dad = g_engine_context.objects->try_get(create_creature("tDad", 1))->as_creature();
		for (auto* c : {mum, dad}) {
			for (int stage : {1, 2}) { c->age = static_cast<uint8_t>(stage); express_genes(*c, *c->genome, stage); }
			build_body(*c, appearance_breeds(*c->genome, c->sex));
			c->movement_status = MOVEMENT_AUTONOMOUS;
			c->move_ground_to(c == mum ? 2660 : 2640, 927);
			c->update_limits();
			c->loci.senses[0] = 255; // the regular 16-tick constant emitter input
			c->biochemistry->chemicals[58].concentration = 246;
			c->biochemistry->chemicals[59].concentration = 197;
		}
	}
	void TearDown() override { g_engine_context.reset(); }
	void hormones(Creature& c) {
		creature_biochemistry_tick(*c.biochemistry, c.loci, c.brain.get(), c.dead, c.biotick++);
		c.update_fertility();
	}
	void tick() {
		g_engine_context.world->tick++;
		g_engine_context.objects->tick();
		g_engine_context.macros->tick();
		if (g_engine_context.world->tick % 4 == 0) g_engine_context.messages->tick();
		g_engine_context.objects->destroy_pending();
	}
};

TEST_F(BreedingData, OriginalFemaleGenomeCyclesWithoutFalsePregnancy) {
	bool fertile = false, returned = false;
	for (int step = 0; step < 3000; ++step) {
		hormones(*mum);
		fertile |= !mum->gamete.empty();
		returned |= fertile && mum->gamete.empty();
	}
	EXPECT_TRUE(fertile);
	EXPECT_TRUE(returned);
	EXPECT_EQ(mum->biochemistry->chemicals[66].concentration, 0);
	EXPECT_EQ(mum->biochemistry->chemicals[65].concentration, 0);
}

TEST_F(BreedingData, ConceptionRunsOriginalLayingAndHatchingScripts) {
	for (int step = 0; step < 5000 && (mum->gamete.empty() || dad->gamete.empty() || dad->biochemistry->chemicals[13].concentration <= 128); ++step) {
		hormones(*mum); hormones(*dad);
	}
	ASSERT_FALSE(mum->gamete.empty());
	ASSERT_FALSE(dad->gamete.empty());
	ASSERT_GT(dad->biochemistry->chemicals[13].concentration, 128);
	dad->attention = mum->uid;
	dad->update_target_point();
	// Run the real world's kiss script, including its age/sex/drive checks and
	// f**k command. Isolate that short script from competing brain decisions;
	// the ordinary creature/world loop resumes for pregnancy, laying and hatching.
	for (int attempt = 0; attempt < 256 && mum->zygote.empty(); ++attempt) {
		dad->update_fertility();
		dad->start_decision(1);
		for (int turn = 0; turn < 300 && mum->zygote.empty(); ++turn) {
			creature_tick_pose(*mum);
			creature_tick_pose(*dad);
			g_engine_context.macros->tick();
			g_engine_context.messages->tick();
		}
	}
	ASSERT_FALSE(mum->zygote.empty());
	const auto child = mum->zygote;
	auto genome = load_genome(child);
	ASSERT_TRUE(genome);
	bool parents_match = false;
	for (const auto& gene : genome->genes) if (auto* ancestry = dynamic_cast<creatureGenusGene*>(gene.get())) {
		parents_match = ancestry->mum == "tMum" && ancestry->dad == "tDad";
		break;
	}
	EXPECT_TRUE(parents_match);
	// Keep the pregnancy across an actual world-file round trip before continuing.
	save_world("BreedingPregnant.sfc");
	auto saved = sfc::read_sfc_v1_file(find_world("BreedingPregnant.sfc"));
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(data);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	sfc_load_everything(saved);
	mum = dad = nullptr;
	for (auto* obj : *g_engine_context.objects) if (auto* c = obj->as_creature()) {
		if (c->moniker == "tMum") mum = c;
		if (c->moniker == "tDad") dad = c;
	}
	ASSERT_NE(mum, nullptr);
	ASSERT_NE(dad, nullptr);
	ASSERT_EQ(mum->zygote, child);
	fmt::print("TEST breeding: conceived {} with tMum/tDad ancestry; pregnancy survived save/reload\n", child);
	ASSERT_TRUE(std::filesystem::is_regular_file(data / "Genetics" / (child + ".gen")));
	const auto before = g_engine_context.world->natural_eggs;
	bool saw_egg = false, hatched = false;
	for (int n = 0; n < 18000; ++n) {
		tick();
		for (auto* obj : *g_engine_context.objects) {
			if (obj->family == 2 && obj->genus == 5 && obj->species == 2 && obj->obv0 == moniker_to_integer(child) && !saw_egg) {
				saw_egg = true;
				fmt::print("TEST breeding: original laying script created egg {} at tick {}\n", child, g_engine_context.world->tick);
			}
			if (auto* c = obj->as_creature(); c && c->moniker == child) hatched = true;
		}
		if (hatched) break;
	}
	EXPECT_TRUE(saw_egg) << "progesterone=" << int(mum->biochemistry->chemicals[66].concentration) << " baby=" << mum->zygote;
	EXPECT_TRUE(mum->zygote.empty());
	EXPECT_EQ(g_engine_context.world->natural_eggs, before + 1);
	EXPECT_TRUE(hatched);
	if (hatched) fmt::print("TEST breeding: original egg script hatched {} at tick {}\n", child, g_engine_context.world->tick);
}

TEST_F(BreedingData, ExistingFalsePregnancyChemistryRecoversWithoutManualRepair) {
	mum->biochemistry->chemicals[65].concentration = 255;
	mum->biochemistry->chemicals[66].concentration = 255;
	bool became_fertile = false;
	for (int step = 0; step < 5000; ++step) {
		hormones(*mum);
		became_fertile |= !mum->gamete.empty();
	}
	EXPECT_TRUE(mum->zygote.empty());
	EXPECT_TRUE(became_fertile);
	EXPECT_EQ(mum->biochemistry->chemicals[65].concentration, 0);
}


TEST(Reproduction, ChildAndCopiedGenomesRecordTheirActualParents) {
	g_engine_context.reset();
	const auto data = std::filesystem::temp_directory_path() / ("openc1-pedigree-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(data / "Genetics");
	g_engine_context.paths->set_main_directory(data);
	// A synthetic, immutable C1 genus gene; no original genome assets required.
	const std::vector<std::vector<uint8_t>> genes{{'g','e','n','e',2,1,1,0,0,0,0,'O','L','D','M','O','L','D','D'}};
	save_genome_bytes("MUM1", genes);
	save_genome_bytes("DAD1", genes);
	for (const std::string& father : {std::string("DAD1"), std::string()}) {
		auto child = load_genome(make_child_genome("MUM1", father));
		ASSERT_TRUE(child);
		ASSERT_EQ(child->genes.size(), 1u);
		auto* genus = dynamic_cast<creatureGenusGene*>(child->genes[0].get());
		ASSERT_NE(genus, nullptr);
		EXPECT_EQ(genus->mum, "MUM1");
		EXPECT_EQ(genus->dad, father);
		EXPECT_EQ(genus->genus, 0);
	}
	EXPECT_EQ(genome_gene_bytes("MUM1"), genes);
	EXPECT_EQ(genome_gene_bytes("DAD1"), genes);
	g_engine_context.reset();
	std::filesystem::remove_all(data);
}

#include "fileformats/sfc/Macro.h"
TEST_F(BreedingData, InvalidSavedExecutionPositionsAreDiscardedWhileWorldLoads) {
    auto saved = sfc_dump_everything();
    saved.macros.clear();
    for (int n = 0; n < 4; ++n) {
        auto macro = std::make_shared<sfc::MacroV1>();
        macro->script = "wait 1,endm";
        if (n == 1) macro->ip = 100;
        if (n == 2) macro->sp = 21;
        if (n == 3) { macro->subroutine_label = "test"; macro->subroutine_address = 100; }
        saved.macros.push_back(macro);
    }
    g_engine_context.reset();
    g_engine_context.paths->set_main_directory(data);
    g_engine_context.images->load_default_palette();
    g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
    EXPECT_NO_THROW(sfc_load_everything(saved));
    EXPECT_EQ(g_engine_context.macros->count(), 1u);
    EXPECT_EQ(count_living_creatures(), 2);
}
TEST_F(BreedingData, MissingGeneratedSpriteIsRecoveredAndUnavailableBreedFallsBack) {
    // Preserve all essential source art; remove only this scratch-generated sprite.
    auto saved = sfc_dump_everything();
    const auto moniker = mum->moniker;
    const auto file = g_engine_context.paths->find_path(PATH_TYPE_IMAGE, moniker + ".spr");
    ASSERT_FALSE(file.empty()); std::filesystem::remove(file);
    g_engine_context.reset(); g_engine_context.paths->set_main_directory(data);
    g_engine_context.images->load_default_palette();
    g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
    EXPECT_NO_THROW(sfc_load_everything(saved));
    Creature* restored = nullptr;
    for (auto* obj : *g_engine_context.objects) if (auto* c = obj->as_creature(); c && c->moniker == moniker) restored = c;
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->body->gallery().has_frame(restored->body->frame()));
    EXPECT_NO_THROW(build_body(*restored, {9, 9, 9, 9}));
}

#include "fileformats/sfc/Creature.h"
TEST_F(BreedingData, OverlongSavedPosesAndGaitsRecoverWithoutLosingCreature) {
    auto saved = sfc_dump_everything();
    for (auto& obj : saved.objects) if (auto* c = dynamic_cast<sfc::CreatureV1*>(obj.get())) {
        c->current_pose = std::string(80, '0');
        c->poses[0] = std::string(80, '0');
        c->gait_animations[0] = std::string(80, '0') + "R";
    }
    g_engine_context.reset(); g_engine_context.paths->set_main_directory(data);
    g_engine_context.images->load_default_palette();
    g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
    EXPECT_NO_THROW(sfc_load_everything(saved));
    EXPECT_EQ(count_living_creatures(), 2);
    for (auto* obj : *g_engine_context.objects) if (auto* c = obj->as_creature()) {
        EXPECT_EQ(c->current_pose.size(), 15u); EXPECT_EQ(c->poses[0].size(), 15u);
        EXPECT_EQ(c->gait_animations[0].size(), 17u);
        c->loci.gait[0] = 255; EXPECT_NO_THROW(c->walk());
    }
}

TEST_F(BreedingData, QuestionRepliesCannotRecursivelyOverflowTheStack) {
	for (auto* c : {mum, dad}) {
		for (auto& word : c->vocabulary) word = {"unknown", "unknown", 255};
		const std::string answer = c == mum ? "ping" : "pong";
		const std::string question = c == mum ? "pong" : "ping";
		c->vocabulary[74] = {answer, answer, 255};
		c->vocabulary[75] = {question, question, 255};
		c->current_decision = 0;
		c->asleep = 0;
	}
	const auto before = g_engine_context.objects->count_classifier(0, 0, 0);
	for (int n = 0; n < 50; ++n) mum->say_what_doing();
	EXPECT_LE(g_engine_context.objects->count_classifier(0, 0, 0), before + 50);
	// A later independent broadcast must still work after the recursion guard resets.
	dad->say_what_doing();
	EXPECT_EQ(g_engine_context.objects->count_classifier(0, 0, 0), before + 51);
}

TEST_F(BreedingData, FreelyStandingCreatureRecoversFromMidAirAndStaleRoomLimits) {
	mum->attr |= ATTR_WALLBOUND;
	mum->movement_status = MOVEMENT_AUTONOMOUS;
	mum->move_ground_to(2660, 850);
	mum->limit = {2500, 0, 500, 50}; // stale limits from another level
	mum->asleep = 1; // sleepers need rescue too
	mum->check_standing_on_a_floor();
	EXPECT_EQ(mum->ground_y(), 927);
	EXPECT_EQ(mum->limit.bottom(), 927);
	mum->movement_status = MOVEMENT_CARRIED;
	mum->move_ground_to(2660, 850);
	mum->check_standing_on_a_floor();
	EXPECT_EQ(mum->ground_y(), 850);
	mum->movement_status = MOVEMENT_FLOATING;
	mum->check_standing_on_a_floor();
	EXPECT_EQ(mum->ground_y(), 850);
}

TEST_F(BreedingData, TaughtVocabularySurvivesWorldSaveAndReload) {
	mum->learn_action_words();
	const auto moniker = mum->moniker;
	save_world("Teaching Check.sfc");
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(data);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	sfc_load_everything(sfc::read_sfc_v1_file(find_world("Teaching Check.sfc")));
	Creature* restored = nullptr;
	for (auto* obj : *g_engine_context.objects)
		if (auto* c = obj->as_creature(); c && c->moniker == moniker) restored = c;
	ASSERT_NE(restored, nullptr);
	EXPECT_EQ(restored->vocabulary[1].in, "push");
	EXPECT_EQ(restored->vocabulary[1].out, "push");
	EXPECT_EQ(restored->vocabulary[1].strength, 255u);
}

TEST_F(BreedingData, WorldSaveFailsWithoutDestroyingPreviousSaveOrChangingActiveSlot) {
	set_world_file_name("Atomic Check.sfc");
	save_world(get_world_file_name());
	const auto folder = world_folder(get_world_file_name());
	std::ifstream before_file(folder / get_world_file_name(), std::ios::binary);
	const std::string before((std::istreambuf_iterator<char>(before_file)), {});
	ASSERT_FALSE(before.empty());
	std::filesystem::create_directory(folder / "Atomic Check.sfc.tmp");
	EXPECT_THROW(save_world(get_world_file_name()), std::exception);
	std::ifstream after_file(folder / get_world_file_name(), std::ios::binary);
	const std::string after((std::istreambuf_iterator<char>(after_file)), {});
	EXPECT_EQ(after, before);
	EXPECT_EQ(get_world_file_name(), "Atomic Check.sfc");
	std::filesystem::remove_all(folder / "Atomic Check.sfc.tmp");
}

TEST_F(BreedingData, DeathSignalIsReadOnlyAtTheHundredAndTwentyEightTickCheck) {
	// batch 7: the death locus is checked every 128 ticks, not every brain step; and it's a
	// signal, so glycogen that recovers before the check saves the creature
	auto& chemicals = dad->biochemistry->chemicals;
	dad->asleep = 1;
	dad->dreaming = 0;
	auto run = [&](uint32_t from, uint32_t to, uint8_t glycogen) {
		for (uint32_t t = from; t <= to; ++t) {
			chemicals[59].concentration = glycogen;
			g_engine_context.world->tick = t;
			dad->creature_tick();
		}
	};
	run(129, 250, 0);
	EXPECT_EQ(dad->loci.death_signal, 128);
	EXPECT_EQ(dad->dead, 0);
	run(251, 256, 197); // the brain step at 255 sees the glycogen
	EXPECT_EQ(dad->loci.death_signal, 0);
	EXPECT_EQ(dad->dead, 0);
	run(257, 383, 0);
	EXPECT_EQ(dad->dead, 0);
	run(384, 384, 0);
	EXPECT_NE(dad->dead, 0);
}

TEST_F(BreedingData, FertilityChangesOnlyAtTheHundredAndTwentyEightTickCheck) {
	// batch 7: so a male is fertile again only at the next check after a kiss
	dad->asleep = 1;
	dad->dreaming = 0;
	auto run = [&](uint32_t from, uint32_t to) {
		for (uint32_t t = from; t <= to; ++t) {
			dad->biochemistry->chemicals[64].concentration = 255; // testosterone
			g_engine_context.world->tick = t;
			dad->creature_tick();
		}
	};
	dad->gamete.clear();
	run(129, 256);
	ASSERT_EQ(dad->gamete, dad->moniker);
	dad->gamete.clear(); // as a kiss leaves him
	run(257, 383);
	EXPECT_TRUE(dad->gamete.empty());
	EXPECT_EQ(dad->loci.fertile_output, 255); // also read at the check
	run(384, 384);
	EXPECT_EQ(dad->gamete, dad->moniker);
}

TEST_F(BreedingData, EveryCreatureGetsEqualBrainAndChemistryOpportunities) {
	std::vector<Creature*> population{mum, dad};
	for (int n = 0; n < 7; ++n) {
		auto id = create_creature("tDad", 1);
		population.push_back(g_engine_context.objects->try_get(id)->as_creature());
	}
	std::vector<uint32_t> before;
	for (auto* c : population) { c->asleep = 1; c->dreaming = 0; before.push_back(c->biotick); }
	for (uint32_t turn = 1; turn <= 25; ++turn) {
		g_engine_context.world->tick = turn;
		for (auto* c : population) c->creature_tick();
	}
	for (size_t n = 0; n < population.size(); ++n) EXPECT_EQ(population[n]->biotick - before[n], 5u) << n;
}

TEST_F(BreedingData, FreshBrainsInitializeNeuronActivityAndSusceptibility) {
	auto brain = build_brain(*mum->genome, 2);
	ASSERT_FALSE(brain->neurons.empty());
	for (const auto& n : brain->neurons) {
		EXPECT_EQ(n.state, 0);
		EXPECT_EQ(n.output, 0);
		for (const auto* list : {&n.dendrites0, &n.dendrites1})
			for (const auto& d : *list) EXPECT_EQ(d.suscept, 0);
	}
}

#include "Registration.h"
TEST_F(BreedingData, RecoverySavesRotateWithoutOverwritingManualProgressAndKeepCompanions) {
	set_world_file_name("Recovery Check.sfc");
	g_engine_context.world->tick = 123;
	save_world(get_world_file_name());
	mum->history_owner_name = "Owner before recovery";
	save_registration(*mum);
	set_autosave_enabled(true);
	for (uint32_t round = 1; round <= 4; ++round) {
		g_engine_context.world->tick = round * 100;
		for (int n = 0; n < 6000; ++n) world_systems_tick();
	}
	set_autosave_enabled(false);
	EXPECT_EQ(sfc::read_sfc_v1_file(find_world("Recovery Check.sfc")).tick, 123u);
	for (uint32_t slot = 1; slot <= 3; ++slot) {
		const auto file = "Recovery Check.autosave." + std::to_string(slot) + ".sfc";
		EXPECT_EQ(sfc::read_sfc_v1_file(find_world(file)).tick, (5 - slot) * 100);
		EXPECT_TRUE(std::filesystem::exists(world_folder(file) / (file + "." + mum->moniker + ".registration")));
	}
	EXPECT_EQ(get_world_file_name(), "Recovery Check.sfc");
	EXPECT_EQ(last_saved_world(), "Recovery Check.sfc");
}

TEST_F(BreedingData, WrappedCreatureKeepsItsPhysicalPositionAndCompactBodyBounds) {
	mum->move_ground_to(12535, 1085); // observed in Jeff's copied World.sfc
	EXPECT_EQ(mum->ground_x(), 4183);
	EXPECT_LT(mum->get_body_bbox().width, 300);
	for (int i = 0; i < 40; ++i) {
		mum->move_by(CREATURES1_WORLD_WIDTH, 0);
		EXPECT_EQ(mum->ground_x(), 4183);
	}
	mum->move_ground_to(-10, 927);
	EXPECT_EQ(mum->ground_x(), CREATURES1_WORLD_WIDTH - 10);
	EXPECT_LT(mum->get_body_bbox().width, 300);
	mum->move_ground_to(1, 927);
	EXPECT_LT(mum->get_body_bbox().width, 300);
	mum->move_ground_to(-1000, -1000); // intentional dormant/off-world placement retained
	EXPECT_EQ(mum->ground_x(), -1000);
	EXPECT_EQ(mum->ground_y(), -1000);
}

#include "fileformats/sfc/CInstinct.h"
TEST_F(BreedingData, OriginalGenomeInstinctConsolidatesDecisionConnections) {
	std::filesystem::copy_file(data / "Genetics/dad1.gen", data / "Genetics/bLrn.gen", std::filesystem::copy_options::overwrite_existing);
	Creature* pupil = g_engine_context.objects->try_get(create_creature("bLrn", 1))->as_creature();
	ASSERT_EQ(pupil->age, 0);
	pupil->instincts.clear();
	auto instinct = std::make_shared<sfc::CInstinctV1>();
	instinct->dendrites = {{{LOBE_STIMULUS_SOURCE, 6}, {LOBE_DRIVE, 2}, {0, 0}}};
	instinct->motor_decision = 1;
	instinct->reinforcement_chemical = 49;
	instinct->reinforcement_amount = 255;
	instinct->phase = 0;
	pupil->instincts.push_back(instinct);
	pupil->dreaming = 1;
	const auto before = *pupil->brain;
	const auto biotick = pupil->biotick;
	pupil->vocabulary[17].in = "carrot";
	pupil->vocabulary[17].out = "cawrot";
	pupil->vocabulary[17].strength = 120;
	const auto moniker = pupil->moniker;
	auto reload = [&]() {
		save_world("Learning Check.sfc");
		g_engine_context.reset();
		g_engine_context.paths->set_main_directory(data);
		g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
		g_engine_context.images->load_default_palette();
		sfc_load_everything(sfc::read_sfc_v1_file(find_world("Learning Check.sfc")));
		pupil = nullptr;
		for (auto* obj : *g_engine_context.objects)
			if (auto* c = obj->as_creature(); c && c->moniker == moniker) pupil = c;
	};
	for (int step = 0; step < 20; ++step) pupil->dream_tick();
	const auto phase = pupil->instincts.front()->phase;
	reload();
	ASSERT_NE(pupil, nullptr);
	ASSERT_EQ(pupil->instincts.size(), 1u);
	EXPECT_EQ(pupil->instincts.front()->phase, phase);
	EXPECT_EQ(pupil->vocabulary[17].in, "carrot");
	EXPECT_EQ(pupil->vocabulary[17].out, "cawrot");
	EXPECT_EQ(pupil->vocabulary[17].strength, 120u);
	for (int step = 20; step < 41; ++step) pupil->dream_tick();
	EXPECT_EQ(pupil->dreaming, 0);
	EXPECT_TRUE(pupil->instincts.empty());
	EXPECT_EQ(pupil->biotick - biotick, 40u); // the last, learning step isn't a chemistry tick
	unsigned strengthened = 0;
	size_t first = 0;
	for (int lobe = 0; lobe < LOBE_DECISION; ++lobe) first += before.lobes[static_cast<size_t>(lobe)].num_neurons;
	const auto& old = before.neurons[first + 1];
	const auto& learned = pupil->brain->neurons[first + 1];
	for (const auto pair : {std::pair{&old.dendrites0, &learned.dendrites0}, std::pair{&old.dendrites1, &learned.dendrites1}}) {
		for (size_t n = 0; n < pair.first->size(); ++n)
			strengthened += (*pair.second)[n].ltw > (*pair.first)[n].ltw;
	}
	EXPECT_GT(strengthened, 0u);
	const auto consolidated = *pupil->brain;
	reload();
	ASSERT_NE(pupil, nullptr);
	ASSERT_EQ(pupil->brain->neurons.size(), consolidated.neurons.size());
	for (size_t i = 0; i < consolidated.neurons.size(); ++i) {
		const auto& expected = consolidated.neurons[i];
		const auto& actual = pupil->brain->neurons[i];
		ASSERT_EQ(actual.dendrites0.size(), expected.dendrites0.size());
		ASSERT_EQ(actual.dendrites1.size(), expected.dendrites1.size());
		for (size_t d = 0; d < expected.dendrites0.size(); ++d) EXPECT_EQ(actual.dendrites0[d].ltw, expected.dendrites0[d].ltw);
		for (size_t d = 0; d < expected.dendrites1.size(); ++d) EXPECT_EQ(actual.dendrites1[d].ltw, expected.dendrites1[d].ltw);
	}
	for (const auto& n : pupil->brain->neurons) {
		EXPECT_EQ(n.state, 0);
		EXPECT_EQ(n.output, 0);
	}
}

TEST(Genetics, ReceptorNominalsApplyOnlyToExpressedGenesAndAvailableTargets) {
	Creature c;
	c.sex = 1;
	c.dead = 0;
	c.brain = std::make_shared<sfc::CBrainV1>();
	c.brain->lobes.resize(8);
	genomeFile genome;
	auto receptor = [&](uint8_t organ, uint8_t tissue, uint8_t locus, uint8_t nominal, lifestage stage = baby) {
		auto r = std::make_unique<bioReceptorGene>(1);
		r->header.switchontime = stage;
		r->organ = organ; r->tissue = tissue; r->locus = locus;
		r->chemical = 49; r->threshold = 255; r->nominal = nominal;
		r->gain = 0; r->inverted = false; r->digital = false;
		genome.genes.push_back(std::move(r));
	};
	receptor(0, 6, 0, 37); // decision threshold
	receptor(0, 6, 2, 81); // decision resting state
	receptor(1, 5, 2, 99); // hunger drive
	receptor(1, 5, 2, 123, adult);
	receptor(0, 255, 255, 255); // invalid target is harmless
	receptor(1, 3, 0, 255); // nominal is a signal baseline, not immediate death
	c.brain->lobes[6].inputgain = 77;
	c.biochemistry = std::make_shared<sfc::CBiochemistryV1>();
	c.biochemistry->chemicals[49].concentration = 100;
	express_genes(c, genome, 0);
	EXPECT_EQ(c.brain->lobes[6].threshold, 37);
	EXPECT_EQ(c.brain->lobes[6].reststate, 81);
	EXPECT_EQ(c.brain->lobes[6].inputgain, 77);
	EXPECT_EQ(c.loci.drives[2], 99);
	EXPECT_EQ(c.biochemistry->chemicals[49].concentration, 100); // no chemistry step
	EXPECT_EQ(c.dead, 0);
	EXPECT_EQ(c.loci.death_signal, 0); // the death signal waits for the chemistry
	express_genes(c, genome, adult);
	EXPECT_EQ(c.loci.drives[2], 123);
	c.dead = 255;
	express_genes(c, genome, 0);
	EXPECT_EQ(c.dead, 255); // expression never rewrites the death latch
}

TEST_F(BreedingData, FreshBrainReceivesExpressedReceptorNominalsBeforeFirstStep) {
	std::filesystem::copy_file(data / "Genetics/dad1.gen", data / "Genetics/nBio.gen", std::filesystem::copy_options::overwrite_existing);
	auto* newborn = g_engine_context.objects->try_get(create_creature("nBio", 1))->as_creature();
	EXPECT_EQ(newborn->biotick, 0u);
	EXPECT_EQ(newborn->dead, 0);
	bool saw_brain_target = false;
	for (const auto& r : newborn->biochemistry->receptors) {
		if (r.organ != 0) continue;
		auto* locus = creature_locus(newborn->loci, newborn->brain.get(), newborn->dead, true, r.organ, r.tissue, r.locus);
		if (!locus) continue;
		// Last expressed receptor on a shared target determines its initial value.
		uint8_t expected = r.nominal;
		for (const auto& later : newborn->biochemistry->receptors)
			if (later.organ == r.organ && later.tissue == r.tissue && later.locus == r.locus) expected = later.nominal;
		EXPECT_EQ(*locus, expected);
		saw_brain_target = true;
	}
	EXPECT_TRUE(saw_brain_target);
}

#include "CreatureBrain.h"
TEST_F(BreedingData, EatingOriginalCheeseApproachesActivatesAndTeachesFoodBenefit) {
	ObjectHandle food;
	for (auto* obj : *g_engine_context.objects)
		if (obj->family == 2 && obj->genus == 6 && obj->species == 1) { food = obj->uid; break; }
	ASSERT_TRUE(food);
	auto* cheese = g_engine_context.objects->try_get(food);
	cheese->set_position(dad->ground_x() + 110, dad->ground_y() - 40);
	cheese->attr &= ~ATTR_INVISIBLE;
	dad->attention = food;
	dad->update_target_point();
	ASSERT_TRUE(dad->has_target);
	ASSERT_GT(dad->target_x - dad->ground_x(), 55);
	dad->learned_chemicals[6].fill(0); // measure real learning, without seeded food assistance
	auto& chemicals = dad->biochemistry->chemicals;
	for (size_t chem : {34u, 35u, 49u, 57u, 58u}) chemicals[chem].concentration = 0;
	chemicals[3].concentration = 200;
	for (uint32_t decision = 0; decision < 16; ++decision)
		brain_neuron(*dad->brain, LOBE_DECISION, decision)->output = decision == 1 ? 255 : 0;
	dad->current_decision = 0;
	dad->choose_voluntary_action(); // actual decision dispatch selects the stock eat script
	ASSERT_EQ(dad->current_decision, 1);
	const auto start_x = dad->ground_x();
	bool fed = false;
	// Isolate this meal from rival decisions: normal pose, target, macro and queued
	// stimulus delivery still run. This proves the pipeline, not autonomous choice.
	for (int step = 0; step < 800 && !fed; ++step) {
		++g_engine_context.world->tick;
		creature_tick_pose(*dad);
		dad->update_target_point();
		g_engine_context.macros->tick();
		if (g_engine_context.world->tick % 4 == 0) {
			g_engine_context.messages->tick();
			auto pending = std::move(dad->queued_stimuli);
			dad->queued_stimuli.clear();
			for (const auto& q : pending) dad->receive_stimulus(q.stimulus, q.from);
		}
		fed = chemicals[57].concentration > 0;
	}
	ASSERT_TRUE(fed);
	EXPECT_GT(dad->ground_x(), start_x);
	EXPECT_EQ(chemicals[57].concentration, 150);
	EXPECT_EQ(chemicals[35].concentration, 250);
	EXPECT_EQ(chemicals[34].concentration, 10);
	EXPECT_EQ(dad->learned_chemicals[6][2], 50); // hunger reduction / 5
	EXPECT_EQ(dad->learned_chemicals[6][1], 2); // pleasure reduction / 5
	uint8_t reward_peak = 0, glucose_peak = 0;
	for (int step = 0; step < 200; ++step) {
		creature_biochemistry_tick(*dad->biochemistry, dad->loci, dad->brain.get(), dad->dead, dad->biotick++);
		reward_peak = std::max(reward_peak, chemicals[49].concentration);
		glucose_peak = std::max(glucose_peak, chemicals[58].concentration);
	}
	EXPECT_GT(reward_peak, 0);
	EXPECT_GT(glucose_peak, 0);
	g_engine_context.objects->destroy_pending();
	save_world("Meal Check.sfc");
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(data);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	sfc_load_everything(sfc::read_sfc_v1_file(find_world("Meal Check.sfc")));
	Creature* restored = nullptr;
	for (auto* obj : *g_engine_context.objects)
		if (auto* c = obj->as_creature(); c && c->moniker == "tDad") restored = c;
	ASSERT_NE(restored, nullptr);
	EXPECT_EQ(restored->learned_chemicals[6][2], 50);
	EXPECT_EQ(restored->learned_chemicals[6][1], 2);
}

#include "CreatureSprites.h"
#include "common/io/FileReader.h"
#include "fileformats/sprImage.h"
#include "../../../third_party/librecreatures/C1Palette.h"
TEST_F(BreedingData, PigmentedNewbornWritesTintedArtAndReloadDoesNotTintItAgain) {
	auto genes = genome_gene_bytes("tDad");
	unsigned pigments = 0;
	for (auto& record : genes) {
		if (record.size() >= 12 && record[4] == 2 && record[5] == 6) {
			record[11] = record[10] == 0 ? 255 : 0;
			++pigments;
		}
	}
	ASSERT_GT(pigments, 0u);
	save_genome_bytes("pTnt", genes);
	auto* pupil = g_engine_context.objects->try_get(create_creature("pTnt", 1))->as_creature();
	const auto remap = openc1::librecreatures::build_creature_palette_remap(
		g_engine_context.images->default_palette(), pupil->pigments[0], pupil->pigments[1], pupil->pigments[2], pupil->pigment_rotation, pupil->pigment_swap);
	const auto source_name = creature_body_part_sprite('A', 0, 0, pupil->breeds[0]);
	FileReader source_file(g_engine_context.paths->find_path(PATH_TYPE_IMAGE, source_name + ".spr"));
	const auto source = ReadSprFile(source_file);
	FileReader generated_file(data / "Images/pTnt.spr");
	const auto generated = ReadSprFile(generated_file);
	ASSERT_GE(generated.size(), source.size());
	unsigned changed = 0;
	for (size_t frame = 0; frame < source.size(); ++frame) {
		ASSERT_EQ(source[frame].data.size(), generated[frame].data.size());
		for (size_t pixel = 0; pixel < source[frame].data.size(); ++pixel) {
			EXPECT_EQ(generated[frame].data[pixel], remap[source[frame].data[pixel]]);
			changed += source[frame].data[pixel] != generated[frame].data[pixel];
		}
	}
	EXPECT_GT(changed, 0u);
	save_world("Pigment Check.sfc");
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(data);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	sfc_load_everything(sfc::read_sfc_v1_file(find_world("Pigment Check.sfc")));
	FileReader restored_file(data / "Images/pTnt.spr");
	const auto restored = ReadSprFile(restored_file);
	ASSERT_EQ(restored.size(), generated.size());
	for (size_t frame = 0; frame < generated.size(); ++frame) {
		ASSERT_EQ(restored[frame].data.size(), generated[frame].data.size());
		for (size_t pixel = 0; pixel < generated[frame].data.size(); ++pixel)
			ASSERT_EQ(restored[frame].data[pixel], generated[frame].data[pixel]);
	}
}

#include "objects/Blackboard.h"
TEST_F(BreedingData, BlackboardScriptTeachesAtSixtyAndPersistsAcrossReload) {
	Blackboard* board = nullptr;
	for (auto* obj : *g_engine_context.objects)
		if (auto* b = obj->as_blackboard()) { board = b; break; }
	ASSERT_NE(board, nullptr);
	board->set_position(dad->ground_x() + 20, dad->ground_y() - 80);
	board->attr &= ~ATTR_INVISIBLE;
	board->obv0 = 0;
	board->words[0] = {30, "carrot"};
	ASSERT_TRUE(dad->can_see(*board));
	dad->vocabulary[30] = {"gak", "gak", 35};
	auto emit = [&]() {
		Macro command{"bbd: emit 0,endm"};
		command.ownr = command.targ = board->uid;
		command.inst = true;
		g_engine_context.macros->run_now(command, false);
	};
	emit();
	EXPECT_EQ(dad->vocabulary[30].strength, 60u);
	EXPECT_EQ(dad->vocabulary[30].in, "carrot");
	emit();
	EXPECT_EQ(dad->vocabulary[30].strength, 120u);
	EXPECT_EQ(dad->vocabulary[30].out, "cawrot");
	save_world("Blackboard Check.sfc");
	g_engine_context.reset();
	g_engine_context.paths->set_main_directory(data);
	g_engine_context.sounds->set_listener_world_wrap_width(CREATURES1_WORLD_WIDTH);
	g_engine_context.images->load_default_palette();
	sfc_load_everything(sfc::read_sfc_v1_file(find_world("Blackboard Check.sfc")));
	Creature* pupil = nullptr;
	board = nullptr;
	for (auto* obj : *g_engine_context.objects) {
		if (auto* c = obj->as_creature(); c && c->moniker == "tDad") pupil = c;
		if (auto* b = obj->as_blackboard(); b && b->words[0].text == "carrot") board = b;
	}
	ASSERT_NE(pupil, nullptr);
	ASSERT_NE(board, nullptr);
	EXPECT_EQ(pupil->vocabulary[30].strength, 120u);
	EXPECT_EQ(pupil->vocabulary[30].out, "cawrot");
	emit();
	EXPECT_EQ(pupil->vocabulary[30].strength, 180u);
	EXPECT_EQ(pupil->vocabulary[30].out, "carrot");
}
