#include "CreatureBrain.h"
#include "fileformats/sfc/CBrain.h"

#include <gtest/gtest.h>

namespace {
sfc::CBrainV1 brain_with_one_neuron_per_lobe() {
	sfc::CBrainV1 b;
	b.lobes.resize(9);
	b.neurons.resize(9);
	for (auto& l : b.lobes) l.num_neurons = 1;
	return b;
}

sfc::DendriteV1 connection(uint8_t stw, uint8_t strength) {
	sfc::DendriteV1 d{};
	d.stw = d.ltw = stw;
	d.strength = strength;
	return d;
}
}

TEST(Brain, AttentionMatchesBatch8WorkedExample) {
	auto b = brain_with_one_neuron_per_lobe();
	auto& stimulus = b.lobes[2];
	stimulus.leakage = 16;
	b.neurons[2].state = 79;
	auto& attention = b.lobes[7];
	attention.inputgain = 255;
	attention.leakage = 48;
	attention.svrule = {9, 23, 12, 23, 13}; // state + type 0 + type 1
	attention.d0_type.source_lobe = 2;
	attention.d1_type.source_lobe = 4;
	b.neurons[7].dendrites0 = {connection(255, 255)};
	b.neurons[7].dendrites1 = {connection(255, 255)};
	BrainMigrationState migration;
	const int stimulus_outputs[] = {79, 35, 15, 6};
	const int attention_outputs[] = {77, 106, 113, 111};
	for (uint32_t step = 0; step < 4; ++step) {
		brain_tick(b, step, migration);
		EXPECT_EQ(b.neurons[2].output, stimulus_outputs[step]);
		EXPECT_EQ(b.neurons[7].output, attention_outputs[step]);
	}
}

TEST(Brain, DendriteInputUsesShortTermWeightAndTwoRoundedDivisions) {
	auto b = brain_with_one_neuron_per_lobe();
	b.neurons[2].state = 200;
	auto& decision = b.lobes[6];
	decision.inputgain = 128;
	decision.reststate = 80;
	decision.leakage = 24;
	decision.svrule = {9, 23, 12, 24, 13}; // state + excitatory - inhibitory
	decision.d0_type.source_lobe = 2;
	b.neurons[6].state = 80;
	b.neurons[6].dendrites0 = {connection(64, 255)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	// floor(200 * 64 / 256) * 128 / 256 = 25, with no inhibitory connections.
	EXPECT_EQ(b.neurons[6].output, 105);
	EXPECT_EQ(b.neurons[6].state, 96);
}

TEST(Brain, MissingConnectionsSupplyZeroInput) {
	auto b = brain_with_one_neuron_per_lobe();
	b.lobes[6].inputgain = 255;
	b.lobes[6].svrule = {12};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[6].output, 0);
}

TEST(Brain, DendriteSumSaturatesBeforeInputGain) {
	auto b = brain_with_one_neuron_per_lobe();
	b.neurons[2].state = 200;
	b.lobes[6].inputgain = 128;
	b.lobes[6].svrule = {12};
	b.lobes[6].d0_type.source_lobe = 2;
	b.neurons[6].dendrites0 = {connection(255, 255), connection(255, 255)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[6].output, 127);
}

TEST(Brain, WinnerTakesAllHonoursDisabledNeuronsAndPreservesStates) {
	sfc::CBrainV1 b;
	b.lobes.resize(3);
	b.lobes[2].num_neurons = 3;
	b.lobes[2].flags = 1;
	b.lobes[2].leakage = 248;
	b.neurons.resize(3);
	b.neurons[0].state = 200;
	b.neurons[0].wta_disable = 1;
	b.neurons[1].state = b.neurons[2].state = 100;
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[0].output, 0);
	EXPECT_EQ(b.neurons[1].output, 100);
	EXPECT_EQ(b.neurons[2].output, 0); // lowest index wins ties
	EXPECT_EQ(b.neurons[0].state, 200);
	EXPECT_EQ(b.neurons[1].state, 100);
	EXPECT_EQ(b.neurons[2].state, 100);
}

TEST(Brain, ConnectionTypesAndNeuronTemporaryValuesAreIndependent) {
	sfc::CBrainV1 b{};
	b.lobes.resize(8);
	b.lobes[2].num_neurons = 1;
	b.lobes[7].num_neurons = 2;
	b.neurons.resize(3);
	b.neurons[0].state = 200;
	auto& l = b.lobes[7];
	l.inputgain = 255;
	l.svrule = {13}; // only type 1; type 0 is empty
	l.d1_type.source_lobe = 2;
	b.neurons[1].dendrites1 = {connection(255, 255)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[1].output, 198);
	EXPECT_EQ(b.neurons[2].output, 0); // previous neuron's sum cannot leak
}

TEST(Brain, EmptyLobesRemainInactive) {
	sfc::CBrainV1 b{};
	b.lobes.resize(9);
	for (auto& l : b.lobes) { l.activity = 255; l.d0_type.migration = l.d1_type.migration = 2; }
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	for (auto& l : b.lobes) EXPECT_EQ(l.activity, 0);
}

TEST(Brain, LooseConnectionsDoNotBlockMigrationToTheirOldSource) {
	auto b = brain_with_one_neuron_per_lobe();
	b.neurons[2].state = 200;
	auto& l = b.lobes[6];
	l.svrule = {4};
	l.d0_type.source_lobe = 2;
	l.d0_type.migration = 1;
	l.d0_type.min_strength = l.d0_type.max_strength = 90;
	l.d0_type.min_ltw = l.d0_type.max_ltw = 80;
	b.neurons[6].dendrites0 = {connection(0, 0), connection(0, 0)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[6].dendrites0[0].strength, 90);
	EXPECT_EQ(b.neurons[6].dendrites0[1].strength, 0);
	brain_tick(b, 1, migration);
	EXPECT_EQ(b.neurons[6].dendrites0[1].strength, 0); // live duplicate still refused
}

TEST(Brain, WholeSetMigrationDoesNotRejectTheMigratingNeuronItself) {
	auto b = brain_with_one_neuron_per_lobe();
	b.neurons[2].state = 200;
	auto& l = b.lobes[6];
	l.svrule = {4};
	l.d0_type.source_lobe = 2;
	l.d0_type.migration = 2;
	l.d0_type.min_strength = l.d0_type.max_strength = 90;
	l.d0_type.min_ltw = l.d0_type.max_ltw = 80;
	b.neurons[6].dendrites0 = {connection(0, 0)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[6].dendrites0[0].strength, 90);
}

TEST(Brain, WholeSetDuplicateDetectionUsesActualSourcesRatherThanTheirSum) {
	sfc::CBrainV1 b{};
	b.lobes.resize(7);
	b.lobes[2].num_neurons = 4;
	b.lobes[6].num_neurons = 2;
	b.neurons.resize(6);
	b.neurons[0].state = b.neurons[3].state = 200;
	auto& l = b.lobes[6];
	l.svrule = {4};
	l.d0_type.source_lobe = 2;
	l.d0_type.migration = 2;
	l.d0_type.min_strength = l.d0_type.max_strength = 90;
	l.d0_type.min_ltw = l.d0_type.max_ltw = 80;
	b.neurons[4].dendrites0 = {connection(0, 0), connection(0, 0)};
	b.neurons[5].dendrites0 = {connection(40, 90), connection(40, 90)};
	b.neurons[5].dendrites0[0].source_index = 1;
	b.neurons[5].dendrites0[1].source_index = 2;
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[4].dendrites0[0].source_index, 0u);
	EXPECT_EQ(b.neurons[4].dendrites0[1].source_index, 3u);
	EXPECT_EQ(b.neurons[4].dendrites0[0].strength, 90); // {0,3} differs from {1,2}
}

TEST(Brain, RewardReinforcesShortTermWeightsAndConsolidatesLongTermMemory) {
	auto b = brain_with_one_neuron_per_lobe();
	b.neurons[2].state = 255;
	auto& l = b.lobes[6];
	l.svrule = {9};
	l.d0_type.source_lobe = 2;
	l.d0_type.suscept_svrule = {4};
	l.d0_type.reinforce_svrule = {5}; // reward chemical, supplied by receptors
	l.d0_type.relax_stw = 248;
	l.d0_type.relax_suscept = 248;
	l.d0_type.ltw_gain_rate = 1;
	b.neurons[6].dendrites0 = {connection(40, 100)};
	BrainMigrationState migration;
	brain_tick(b, 0, migration);
	EXPECT_EQ(b.neurons[6].dendrites0[0].stw, 40);
	l.chemicals[0] = 255;
	brain_tick(b, 1, migration);
	EXPECT_GT(b.neurons[6].dendrites0[0].stw, 40);
	EXPECT_EQ(b.neurons[6].dendrites0[0].ltw, 41);
	l.chemicals[0] = 0;
	const auto learned = b.neurons[6].dendrites0[0].stw;
	brain_tick(b, 2, migration);
	EXPECT_EQ(b.neurons[6].dendrites0[0].stw, learned);
	EXPECT_EQ(b.neurons[6].dendrites0[0].ltw, 42);
}
