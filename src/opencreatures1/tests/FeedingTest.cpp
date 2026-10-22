#include "CreatureBrain.h"
#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "MacroManager.h"
#include "fileformats/sfc/CBrain.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/SimpleObject.h"

#include <gtest/gtest.h>

TEST(Feeding, SeededMemoryGuidesHungryCreaturesWithoutOverwritingLearning) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.brain = std::make_shared<sfc::CBrainV1>();
	c.brain->lobes.resize(8);
	for (auto& lobe : c.brain->lobes) lobe.num_neurons = 40;
	c.brain->neurons.resize(320);
	auto food = g_engine_context.objects->add<SimpleObject>();
	c.attention_memory[6].object = food;
	c.attention_memory[6].has_position = true;
	c.loci.drives[2] = 255; // hunger
	c.need_driven_recall();
	EXPECT_EQ(brain_neuron(*c.brain, LOBE_STIMULUS_SOURCE, 6)->state, 0);
	seed_food_memory(c);
	c.need_driven_recall();
	EXPECT_GT(brain_neuron(*c.brain, LOBE_STIMULUS_SOURCE, 6)->state, 0);
	// Learned experience, including an unrelated chemical, must survive subsequent loads.
	c.learned_chemicals[6][2] = 17;
	c.learned_chemicals[4].fill(0);
	c.learned_chemicals[4][9] = 23;
	const auto learned = c.learned_chemicals;
	seed_food_memory(c);
	EXPECT_EQ(c.learned_chemicals, learned);
	g_engine_context.reset();
}

TEST(Feeding, MealProtectsAttentionAndDecisionUntilScriptFinishes) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.brain = std::make_shared<sfc::CBrainV1>();
	c.brain->lobes.resize(8);
	for (auto& lobe : c.brain->lobes) lobe.num_neurons = 40;
	c.brain->neurons.resize(320);
	auto food = g_engine_context.objects->add<SimpleObject>();
	auto other = g_engine_context.objects->add<SimpleObject>();
	c.attention = food;
	c.has_target = true;
	c.target_x = c.ground_x();
	c.target_y = c.ground_y();
	c.current_decision = 1;
	c.attention_memory[5].object = other;
	brain_neuron(*c.brain, LOBE_ATTENTION, 5)->output = 200;
	brain_neuron(*c.brain, LOBE_DECISION, 2)->output = 200;
	// Run both food and plant meals to completion.
	for (uint8_t genus : {6, 4}) {
		Macro meal{"endm"};
		meal.ownr = handle;
		meal._it_ = food;
		meal.classifier = {2, genus, 1};
		meal.event = SCRIPT_EXTRA_ACTIVATE1;
		g_engine_context.macros->add(meal);
		c.choose_attention();
		c.choose_voluntary_action();
		EXPECT_EQ(c.attention, food);
		EXPECT_EQ(c.current_decision, 1);
		EXPECT_FALSE(g_engine_context.macros->is_eating(handle, other));
		g_engine_context.macros->tick();
		EXPECT_FALSE(g_engine_context.macros->is_eating(handle, food));
	}
	c.choose_attention();
	EXPECT_EQ(c.attention, other);
	g_engine_context.reset();
}

TEST(Feeding, DeletedFoodDoesNotKeepCreatureInMeal) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.brain = std::make_shared<sfc::CBrainV1>();
	auto food = g_engine_context.objects->add<SimpleObject>();
	c.attention = food;
	Macro meal{"wait 100,endm"};
	meal.ownr = handle;
	meal._it_ = food;
	meal.classifier = {2, 6, 1};
	meal.event = SCRIPT_EXTRA_ACTIVATE1;
	g_engine_context.macros->add(meal);
	g_engine_context.objects->destroy_later(food);
	g_engine_context.objects->destroy_pending();
	c.choose_attention();
	EXPECT_FALSE(c.attention);
	g_engine_context.reset();
}

TEST(Feeding, UnreachableApproachesAndStalledMealsCanBeAbandoned) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.brain = std::make_shared<sfc::CBrainV1>();
	c.brain->lobes.resize(8);
	for (auto& lobe : c.brain->lobes) lobe.num_neurons = 40;
	c.brain->neurons.resize(320);
	auto food = g_engine_context.objects->add<SimpleObject>();
	auto other = g_engine_context.objects->add<SimpleObject>();
	c.attention = food;
	c.has_target = true;
	c.target_x = c.ground_x() + 100; // approach still pending
	c.target_y = c.ground_y();
	c.attention_memory[5].object = other;
	brain_neuron(*c.brain, LOBE_ATTENTION, 5)->output = 200;
	Macro meal{"wait 1000,endm"};
	meal.ownr = handle;
	meal._it_ = food;
	meal.classifier = {2, 6, 1};
	meal.event = SCRIPT_EXTRA_ACTIVATE1;
	g_engine_context.macros->add(meal);
	c.choose_attention();
	EXPECT_EQ(c.attention, other);
	// Once close enough, the assist is temporary even if a script never finishes.
	c.attention = food;
	c.target_x = c.ground_x();
	c.target_y = c.ground_y();
	c.has_target = true;
	EXPECT_TRUE(g_engine_context.macros->is_eating(handle, food));
	for (int tick = 0; tick < 128; ++tick) g_engine_context.macros->tick();
	EXPECT_FALSE(g_engine_context.macros->is_eating(handle, food));
	c.choose_attention();
	EXPECT_EQ(c.attention, other);
	g_engine_context.reset();
}
