#include "EngineContext.h"
#include "Macro.h"
#include "MacroCommands.h"
#include "MacroContext.h"
#include "MacroManager.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/SimpleObject.h"

#include <gtest/gtest.h>

static void deliver(Creature& c) {
	for (const auto& q : c.queued_stimuli) {
		c.receive_stimulus(q.stimulus, q.from);
	}
	c.queued_stimuli.clear();
}

TEST(Stimulus, StimWritAddsChemicals) {
	g_engine_context.reset();
	auto creature_handle = g_engine_context.objects->add<Creature>();
	auto* creature = g_engine_context.objects->try_get(creature_handle)->as_creature();
	creature->dead = 0;
	creature->asleep = 0;
	creature->biochemistry = std::make_shared<sfc::CBiochemistryV1>();
	creature->biochemistry->chemicals[35].concentration = 200;
	auto jar = g_engine_context.objects->add<SimpleObject>();

	// the honey jar's stimulus when eaten: +20 of 34, +150 of 35 (capped at 255), +40 of 57
	Macro m{"stim writ targ 10 255 0 0 34 20 35 150 57 40 0 0,endm"};
	m.ownr = jar;
	m.targ = creature_handle;
	g_engine_context.macros->ctx.tick_macro(m);

	// script stimuli wait for the next 4-tick delivery (spec Q2)
	EXPECT_EQ(creature->biochemistry->chemicals[34].concentration, 0);
	ASSERT_EQ(creature->queued_stimuli.size(), 1u);
	deliver(*creature);

	EXPECT_EQ(creature->biochemistry->chemicals[34].concentration, 20);
	EXPECT_EQ(creature->biochemistry->chemicals[35].concentration, 255);
	EXPECT_EQ(creature->biochemistry->chemicals[57].concentration, 40);

	// STM# uses the creature's own numbered stimuli
	creature->stimuli[3].chem0 = 12;
	creature->stimuli[3].amount0 = 7;
	m = Macro{"stm# writ targ 3,endm"};
	m.ownr = jar;
	m.targ = creature_handle;
	g_engine_context.macros->ctx.tick_macro(m);
	deliver(*creature);
	EXPECT_EQ(creature->biochemistry->chemicals[12].concentration, 7);
	g_engine_context.reset();
}

TEST(Disease, BacteriumEffectsAndCure) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto* creature = g_engine_context.objects->try_get(handle)->as_creature();
	creature->dead = 0;
	creature->biochemistry = std::make_shared<sfc::CBiochemistryV1>();
	auto& chems = creature->biochemistry->chemicals;
	// antigen 250 (antibody 242), cure level 180, infection level 80, toxins 232-235
	creature->bacterium = {1, 250, 180, 80, {232, 233, 234, 235}};

	creature->disease_update();
	EXPECT_EQ(creature->bacterium.state, 2); // antibody 0 is below the infection level: active
	EXPECT_EQ(chems[250].concentration, 80);
	EXPECT_EQ(chems[232].concentration, 30);
	EXPECT_EQ(chems[235].concentration, 30);

	chems[242].concentration = 181; // enough antibody
	creature->disease_update();
	EXPECT_EQ(creature->bacterium.state, 0);
	EXPECT_EQ(chems[250].concentration, 80); // no more antigen once cured
	g_engine_context.reset();
}

TEST(Language, LearningWordsForWhatTheCreatureAttendsTo) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto* creature = g_engine_context.objects->try_get(handle)->as_creature();
	creature->dead = creature->asleep = 0;
	creature->family = 4;
	creature->genus = 1;
	auto ball = g_engine_context.objects->add<SimpleObject>();
	auto* ball_obj = g_engine_context.objects->try_get(ball);
	ball_obj->family = 2;
	ball_obj->genus = 5; // attention category 5, noun entry 21
	creature->attention = ball;
	auto hand = g_engine_context.objects->add<SimpleObject>();
	auto* speaker = g_engine_context.objects->try_get(hand);

	auto& noun = creature->vocabulary[21];
	noun.in = noun.out = "goo";
	noun.strength = 50;
	// an unknown word wears the entry down, then replaces it with strength 60 (batch 2, Q22)
	creature->hear_sentence("ball", speaker);
	EXPECT_EQ(noun.strength, 5u);
	creature->hear_sentence("ball", speaker);
	EXPECT_EQ(noun.in, "ball");
	EXPECT_EQ(noun.strength, 60u);
	EXPECT_EQ(noun.out, "gaga");
	// saying the known noun for _IT_ reinforces it; at 160 the creature says it too
	creature->hear_sentence("ball", speaker);
	creature->hear_sentence("ball", speaker);
	EXPECT_EQ(noun.strength, 180u);
	EXPECT_EQ(noun.out, "ball");
	g_engine_context.reset();
}

TEST(Language, BlackboardsTeachWords) {
	// Batch 9 Q8a corrects Batch 4 Q19: the same word strengthens the entry by 60, and at 160 it's the word said;
	// any other word weakens it by 45, and at 0 replaces the heard word
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto* creature = g_engine_context.objects->try_get(handle)->as_creature();
	creature->dead = creature->asleep = 0;
	auto& entry = creature->vocabulary[30];
	entry.in = entry.out = "gak";
	entry.strength = 80;
	creature->teach_word(30, "carrot");
	EXPECT_EQ(entry.strength, 35u);
	creature->teach_word(30, "carrot");
	EXPECT_EQ(entry.in, "carrot");
	EXPECT_EQ(entry.strength, 60u);
	creature->teach_word(30, "carrot");
	EXPECT_EQ(entry.strength, 120u);
	EXPECT_EQ(entry.out, "cawrot");
	creature->teach_word(30, "carrot");
	EXPECT_EQ(entry.strength, 180u);
	EXPECT_EQ(entry.out, "carrot");
	// asleep, nothing is learnt
	creature->asleep = 1;
	creature->teach_word(30, "other");
	EXPECT_EQ(entry.strength, 180u);
	g_engine_context.reset();
}

TEST(Language, NewbornVocabularyProducesWordsBeforeTeaching) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.initialize_vocabulary();
	for (size_t i = 0; i < c.vocabulary.size(); ++i) {
		const auto& word = c.vocabulary[i];
		EXPECT_FALSE(word.in.empty());
		EXPECT_EQ(word.out, word.in);
		EXPECT_GE(word.strength, 30u);
		EXPECT_LE(word.strength, 50u);
	}
	EXPECT_TRUE(c.said_word(16) == "em" || c.said_word(16) == "me" || c.said_word(16) == "eem" || c.said_word(16) == "bub");
	// Computer teaching replaces the weak babble through the existing learning rules.
	for (int i = 0; i < 5; ++i) c.teach_word(1, "push");
	EXPECT_EQ(c.said_word(1), "push");
	g_engine_context.reset();
}

TEST(Language, VocabularyRepairPreservesTaughtWordsAndLearningProgress) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.vocabulary[1] = {"push", "push", 255};
	c.vocabulary[2] = {"pull", "", 60};
	c.vocabulary[3] = {"stop", "", 180};
	c.initialize_vocabulary();
	EXPECT_EQ(c.vocabulary[1].in, "push");
	EXPECT_EQ(c.vocabulary[1].out, "push");
	EXPECT_EQ(c.vocabulary[1].strength, 255u);
	EXPECT_EQ(c.vocabulary[2].in, "pull");
	EXPECT_EQ(c.vocabulary[2].strength, 60u);
	EXPECT_FALSE(c.vocabulary[2].out.empty());
	EXPECT_EQ(c.vocabulary[3].out, "stop");
	const auto words = c.vocabulary;
	c.initialize_vocabulary();
	for (size_t i = 0; i < words.size(); ++i) {
		EXPECT_EQ(c.vocabulary[i].in, words[i].in);
		EXPECT_EQ(c.vocabulary[i].out, words[i].out);
		EXPECT_EQ(c.vocabulary[i].strength, words[i].strength);
	}
	g_engine_context.reset();
}

TEST(Stimulus, CorrectedEmptyHandedDropHasNoDisappointment) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& creature = *g_engine_context.objects->try_get(handle)->as_creature();
	creature.biochemistry = std::make_shared<sfc::CBiochemistryV1>();
	creature.stimuli[0].chem0 = 12;
	creature.stimuli[0].amount0 = 30;
	creature.biochemistry->chemicals[12].concentration = 47;
	Macro m{"drop,endm"}; m.ownr = m.targ = handle;
	g_engine_context.macros->ctx.tick_macro(m);
	deliver(creature);
	EXPECT_EQ(creature.biochemistry->chemicals[12].concentration, 47);
	g_engine_context.reset();
}

TEST(Language, TeachInstallsActionWordsRatherThanReinforcingBabble) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.initialize_vocabulary();
	c.vocabulary[16] = {"Arlo", "Arlo", 255};
	c.age = 0;
	EXPECT_EQ(c.learn_action_words(), 18u);
	EXPECT_EQ(c.vocabulary[1].in, "push");
	EXPECT_EQ(c.vocabulary[1].out, "push");
	EXPECT_EQ(c.vocabulary[1].strength, 255u);
	EXPECT_EQ(c.vocabulary[7].out, "drop");
	EXPECT_EQ(c.vocabulary[74].out, "look");
	EXPECT_EQ(c.vocabulary[75].out, "what");
	EXPECT_EQ(c.vocabulary[16].out, "Arlo");
	EXPECT_EQ(c.age, 0);
	c.dead = 1;
	EXPECT_EQ(c.learn_action_words(), 0u);
	g_engine_context.reset();
}

#include "CreatureBrain.h"
#include "fileformats/sfc/CBrain.h"
TEST(Language, CompleteSentenceAndWhitespaceReachEveryListenerIndependently) {
	g_engine_context.reset();
	auto source = g_engine_context.objects->add<SimpleObject>();
	auto target = g_engine_context.objects->add<SimpleObject>();
	auto* object = g_engine_context.objects->try_get(target);
	object->family = 2; object->genus = 5;
	for (const std::string text : {"push ball", "  push  ball  ", "\tpush\tball\n"}) {
		auto h = g_engine_context.objects->add<Creature>();
		auto* c = g_engine_context.objects->try_get(h)->as_creature();
		c->attention = target;
		c->vocabulary[1] = {"push", "push", 255};
		c->vocabulary[21] = {"ball", "ball", 150};
		c->brain = std::make_shared<sfc::CBrainV1>();
		c->brain->lobes.resize(4);
		c->brain->lobes[3].num_neurons = 16;
		c->brain->neurons.resize(16);
		c->hear_sentence(text, g_engine_context.objects->try_get(source));
		EXPECT_EQ(c->vocabulary[21].strength, 210u);
		EXPECT_EQ(brain_neuron(*c->brain, 3, 1)->state, 179);
	}
	g_engine_context.reset();
}

TEST(Language, MissingAncestryDoesNotCreateSiblingRelationship) {
	g_engine_context.reset();
	auto h = g_engine_context.objects->add<Creature>();
	auto other = g_engine_context.objects->add<Creature>();
	auto* c = g_engine_context.objects->try_get(h)->as_creature();
	auto* relative = g_engine_context.objects->try_get(other)->as_creature();
	c->attention = other;
	c->brain = std::make_shared<sfc::CBrainV1>();
	c->brain->lobes.resize(6); c->brain->lobes[5].num_neurons = 32; c->brain->neurons.resize(32);
	c->update_eight_tick_inputs();
	EXPECT_EQ(brain_neuron(*c->brain, 5, 16)->state, 0);
	c->mother = relative->mother = "tMum";
	c->update_eight_tick_inputs();
	EXPECT_EQ(brain_neuron(*c->brain, 5, 16)->state, 255);
	g_engine_context.reset();
}

#include "../../../third_party/librecreatures/C1LearnedWords.h"
TEST(Language, PronunciationUsesOneOrderedSubstitutionThenExactLearnedWord) {
	using namespace openc1::librecreatures;
	EXPECT_EQ(normalize_word_response("phone"), "fone");
	EXPECT_EQ(normalize_word_response("sheep"), "theep");
	EXPECT_EQ(normalize_word_response("carrot"), "cawrot");
	EXPECT_EQ(normalize_word_response("snake"), "thnake");
	g_engine_context.reset();
	auto h = g_engine_context.objects->add<Creature>();
	auto* c = g_engine_context.objects->try_get(h)->as_creature();
	c->vocabulary[30] = {"carrot", "gaga", 60};
	c->teach_word(30, "carrot");
	EXPECT_EQ(c->vocabulary[30].out, "cawrot");
	c->teach_word(30, "carrot");
	EXPECT_EQ(c->vocabulary[30].out, "carrot");
	g_engine_context.reset();
}

TEST(Language, VowelOnlyResponseStopsAtWordEndAndPreservesAdjacentRecords) {
	using namespace openc1::librecreatures;
	EXPECT_EQ(regenerate_word_response("aeiou", "goo"), "geiou");
	EXPECT_EQ(regenerate_word_response("AEIOU", "foo"), "fEIOU");
	EXPECT_EQ(regenerate_word_response("a", "goo"), "gg");
	EXPECT_EQ(regenerate_word_response("school", "goo"), "gchogcho");
	EXPECT_EQ(regenerate_word_response("", "goo"), "");
	EXPECT_EQ(regenerate_word_response("phone", ""), "fone");
	for (size_t length = 1; length <= 128; ++length) {
		const auto response = regenerate_word_response(std::string(length, 'a'), "goo");
		EXPECT_LE(response.size(), 24u);
	}
	g_engine_context.reset();
	auto h = g_engine_context.objects->add<Creature>();
	auto* c = g_engine_context.objects->try_get(h)->as_creature();
	c->vocabulary[29] = {"before", "before", 255};
	c->vocabulary[30] = {"old", "goo", 1};
	c->vocabulary[31] = {"after", "after", 255};
	c->teach_word(30, std::string(128, 'a'));
	EXPECT_EQ(c->vocabulary[30].in.size(), 24u);
	EXPECT_LE(c->vocabulary[30].out.size(), 24u);
	EXPECT_EQ(c->vocabulary[29].out, "before");
	EXPECT_EQ(c->vocabulary[31].in, "after");
	EXPECT_EQ(c->vocabulary[31].strength, 255u);
	g_engine_context.reset();
}

TEST(Language, GrendelNewbornVocabularyUsesItsOwnBanks) {
	g_engine_context.reset();
	auto h = g_engine_context.objects->add<Creature>();
	auto* c = g_engine_context.objects->try_get(h)->as_creature();
	c->genus = 2;
	c->initialize_vocabulary();
	for (size_t i = 0; i < 16; ++i) {
		const auto& word = c->vocabulary[i].out;
		EXPECT_TRUE(word == "grah" || word == "beh" || word == "urg" || word == "urrh");
	}
	const auto& name = c->vocabulary[16].out;
	EXPECT_TRUE(name == "grndl" || name == "beh" || name == "ung" || name == "fig");
	g_engine_context.reset();
}

TEST(Language, AttentionRetainsUnsignedStimulusIntensityAndRejectsHighInputAsAnIndex) {
	for (int value = 0; value <= 255; ++value) {
		g_engine_context.reset();
		auto h = g_engine_context.objects->add<Creature>();
		auto source = g_engine_context.objects->add<SimpleObject>();
		auto* obj = g_engine_context.objects->try_get(source);
		obj->family = 2; obj->genus = 5;
		auto* c = g_engine_context.objects->try_get(h)->as_creature();
		c->brain = std::make_shared<sfc::CBrainV1>();
		c->brain->lobes.resize(8);
		for (auto& l : c->brain->lobes) l.num_neurons = 40;
		c->brain->neurons.resize(320);
		sfc::StimulusV1 stimulus{};
		stimulus.input = 31;
		stimulus.intensity = static_cast<uint8_t>(value);
		c->receive_stimulus(stimulus, source);
		brain_neuron(*c->brain, LOBE_ATTENTION, 5)->output = 255;
		c->choose_attention();
		EXPECT_EQ(c->attention, source);
		EXPECT_EQ(brain_neuron(*c->brain, LOBE_GENERAL_SENSE, 31)->state, value);
		c->attention = {};
		brain_clear_lobe(*c->brain, LOBE_GENERAL_SENSE);
		stimulus.input = static_cast<uint8_t>(value);
		stimulus.intensity = 255;
		c->receive_stimulus(stimulus, source);
		c->choose_attention();
		for (uint32_t i = 0; i < 40; ++i)
			EXPECT_EQ(brain_neuron(*c->brain, LOBE_GENERAL_SENSE, i)->state, value <= 32 && i == static_cast<uint32_t>(value) ? 255 : 0);
	}
	g_engine_context.reset();
}

TEST(Language, BlackboardReplacementRetainsSixtyAfterSixMismatches) {
	g_engine_context.reset();
	auto id = g_engine_context.objects->add<Creature>();
	auto* c = g_engine_context.objects->try_get(id)->as_creature();
	c->vocabulary[30] = {"apple", "apple", 255};
	for (unsigned expected : {210u, 165u, 120u, 75u, 30u}) {
		c->teach_word(30, "pear");
		EXPECT_EQ(c->vocabulary[30].strength, expected);
		EXPECT_EQ(c->vocabulary[30].in, "apple");
	}
	c->teach_word(30, "pear");
	EXPECT_EQ(c->vocabulary[30].in, "pear");
	EXPECT_EQ(c->vocabulary[30].strength, 60u);
	c->teach_word(30, "PEAR");
	EXPECT_EQ(c->vocabulary[30].strength, 120u);
	c->teach_word(30, "pear");
	EXPECT_EQ(c->vocabulary[30].strength, 180u);
	EXPECT_EQ(c->vocabulary[30].out, "pear");
	g_engine_context.reset();
}
