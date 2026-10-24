#include "CreatureBrain.h"
#include "EngineContext.h"
#include "MacroManager.h"
#include "Scriptorium.h"
#include "fileformats/sfc/CBrain.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/SimpleObject.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>
#include <fmt/core.h>

class SleepingCreature : public testing::Test {
protected:
	Creature* c = nullptr;
	void SetUp() override {
		g_engine_context.reset();
		auto handle = g_engine_context.objects->add<Creature>();
		c = g_engine_context.objects->try_get(handle)->as_creature();
		c->family = 4;
		c->genus = c->species = 1;
		c->asleep = 1;
		c->eyes_open = 0;
		c->running_involuntary = 5;
		c->brain = std::make_shared<sfc::CBrainV1>();
		c->brain->lobes.resize(8);
		for (auto& lobe : c->brain->lobes) lobe.num_neurons = 40;
		c->brain->neurons.resize(320);
		c->brain->lobes[LOBE_DECISION].svrule = {9};
		g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_INTRO_EAST, "wait 10,endm");
		g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_EXTRA_QUIESCENT, "wait 10,endm");
	}
	void TearDown() override { g_engine_context.reset(); }
};

TEST_F(SleepingCreature, InterruptedSleepDoesNotBlockWakingDecisionForever) {
	// The reflex script was stopped/replaced before its ASLP 0 and DONE commands.
	brain_set_input(*c->brain, LOBE_DECISION, 11, 200);
	c->brain_step();
	EXPECT_EQ(c->current_decision, 11);
	EXPECT_EQ(c->running_involuntary, -1);
	EXPECT_EQ(c->asleep, 0);
	EXPECT_EQ(c->eyes_open, 1);
}

TEST_F(SleepingCreature, RunningSleepScriptRetainsItsNormalWakeSequence) {
	g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_INVOLUNTARY5, "wait 5,aslp 0,done,endm");
	g_engine_context.macros->queue_script(c, c, SCRIPT_INVOLUNTARY5);
	brain_set_input(*c->brain, LOBE_DECISION, 11, 200);
	c->brain_step();
	EXPECT_EQ(c->asleep, 1);
	EXPECT_EQ(c->running_involuntary, 5);
	for (int tick = 0; tick < 10; ++tick) g_engine_context.macros->tick();
	EXPECT_EQ(c->asleep, 0);
	EXPECT_EQ(c->running_involuntary, -1);
}

TEST_F(SleepingCreature, AttentionReplacementClearsTheOldSleepAction) {
	auto target = g_engine_context.objects->add<SimpleObject>();
	c->attention_memory[5].object = target;
	brain_neuron(*c->brain, LOBE_ATTENTION, 5)->output = 200;
	c->choose_attention();
	EXPECT_EQ(c->attention, target);
	EXPECT_EQ(c->running_involuntary, -1);
	EXPECT_EQ(c->asleep, 0);
}

TEST_F(SleepingCreature, LibreCreaturesMissingReflexDoesNotLatchAction) {
	c->asleep = 0;
	c->running_involuntary = -1;
	c->current_decision = 11;
	c->importance = 17;
	c->loci.involuntary[0] = 255;
	for (int step = 0; step < 32; ++step) {
		EXPECT_FALSE(c->try_involuntary_action());
		EXPECT_EQ(c->running_involuntary, -1);
		EXPECT_EQ(c->current_decision, 11);
		EXPECT_EQ(c->importance, 17);
	}
}

TEST_F(SleepingCreature, LibreCreaturesExistingReflexStillRunsAndCompletes) {
	c->asleep = 0;
	c->running_involuntary = -1;
	c->loci.involuntary[0] = 255;
	g_engine_context.scriptorium->add(4, 0, 0, SCRIPT_INVOLUNTARY0, "done,endm");
	bool started = false;
	for (int attempt = 0; attempt < 32 && !started; ++attempt) started = c->try_involuntary_action();
	ASSERT_TRUE(started);
	EXPECT_EQ(c->running_involuntary, 0);
	g_engine_context.macros->tick();
	EXPECT_EQ(c->running_involuntary, -1);
}

TEST_F(SleepingCreature, CorrectedLatencyRangesAlreadyWork) {
	for (const auto& range : std::vector<std::pair<int32_t, int32_t>>{{INT32_MIN, INT32_MAX}, {INT32_MAX, INT32_MIN}, {1, 0}, {7, 7}}) {
		for (int sample = 0; sample < 64; ++sample) {
			Macro m{fmt::format("ltcy 0 {} {},endm", range.first, range.second)};
			m.targ = m.ownr = c->uid;
			ASSERT_NO_THROW(g_engine_context.macros->ctx.tick_macro(m));
			EXPECT_GE(c->involuntary_latency[0], std::min(range.first, range.second));
			EXPECT_LE(c->involuntary_latency[0], std::max(range.first, range.second));
		}
	}
}

TEST_F(SleepingCreature, WakingRunsTheZzzDeactivateScriptBeforeItGoes) {
	// the shipped zzz (2 3 13) fades its snore when it's deactivated
	g_engine_context.scriptorium->add(2, 3, 13, SCRIPT_DEACTIVATE, "fade,endm");
	auto zzz = g_engine_context.objects->add<SimpleObject>();
	auto* obj = g_engine_context.objects->try_get(zzz);
	obj->family = 2;
	obj->genus = 3;
	obj->species = 13;
	c->zzzz = zzz;
	c->asleep = 0;
	c->update_zzz();
	g_engine_context.objects->destroy_pending();
	for (int tick = 0; tick < 4; ++tick) {
		g_engine_context.macros->tick();
	}
	EXPECT_EQ(g_engine_context.objects->try_get(zzz), nullptr);
	EXPECT_TRUE(g_engine_context.macros->ctx.error_messages.empty()) << g_engine_context.macros->ctx.error_messages.front();
}
