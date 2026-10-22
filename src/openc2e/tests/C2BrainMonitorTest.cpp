#include "openc2e/Agent.h"
#include "openc2e/World.h"
#include "openc2e/C2BrainMonitor.h"
#include "openc2e/Engine.h"
#include <gtest/gtest.h>
using namespace c2monitor;
static Brain single() {
	Lobe lobe;
	lobe.x = 12; lobe.y = 23; lobe.width = lobe.height = 1;
	lobe.neurons.resize(1);
	return {lobe};
}
TEST(C2Monitor, AllStateAndOutputBytesHaveTheSpecifiedPrecisionAndZeroOmission) {
	auto brain = single();
	for (unsigned value = 0; value <= 255; ++value) {
		brain[0].neurons[0].state = static_cast<uint8_t>(value);
		brain[0].neurons[0].output = static_cast<uint8_t>(255 - value);
		auto automation = lneu(brain, 0);
		auto state = dde_activity(brain, Activity::state);
		auto output = dde_activity(brain, Activity::output);
		if (value) {
			ASSERT_EQ(automation.size(), 3u); ASSERT_EQ(state.size(), 3u);
			EXPECT_EQ(automation[0], 60); EXPECT_EQ(automation[1], 71);
			EXPECT_EQ(automation[2], 48 + value / 4);
			EXPECT_EQ(state[2], 48 + value / 16);
		} else { EXPECT_TRUE(automation.empty()); EXPECT_TRUE(state.empty()); }
		if (value != 255) { ASSERT_EQ(output.size(), 3u); EXPECT_EQ(output[2], 48 + (255 - value) / 16); }
		else EXPECT_TRUE(output.empty());
	}
}
TEST(C2Monitor, EveryMaskUsesOrderedGroupsAndCapsEightEligibleSources) {
	Lobe lobe;
	lobe.width = 12; lobe.height = 1;
	lobe.neurons.resize(12);
	lobe.neurons[0].state = 255;
	for (size_t n = 1; n < 12; ++n) {
		lobe.neurons[n].state = n == 1 ? 0 : 4;
		lobe.neurons[0].sources[0].push_back({0, n});
	}
	lobe.neurons[0].sources[1].push_back({0,11});
	Brain brain{lobe};
	for (unsigned mask = 0; mask < 4; ++mask) {
		auto reply = lneu(brain, mask);
		std::string expected{48,48,111};
		if (mask & 1) {
			expected += char(56);
			for (int n = 2; n <= 9; ++n) expected += std::string{char(48+n),48,49};
		}
		if (mask & 2) expected += std::string{49,59,48,49};
		EXPECT_EQ(reply.substr(0, expected.size()), expected);
	}
}
TEST(C2Monitor, RowMajorAbsoluteCoordinatesAndSourceLobeOrigins) {
	Brain brain{{10,20,2,2,std::vector<Neuron>(4)}, {30,40,1,1,std::vector<Neuron>(1)}};
	brain[0].neurons[3].state = 1;
	brain[0].neurons[3].sources[0].push_back({1,0});
	brain[1].neurons[0].state = 255;
	auto reply = lneu(brain, 1);
	EXPECT_EQ(reply, (std::string{59,69,48,49,78,88,111,78,88,111,48}));
}
TEST(C2Monitor, InvalidRequestsAndCapacityFailuresAreExplicit) {
	auto brain = single(); brain[0].neurons[0].state = 255;
	EXPECT_ANY_THROW(lneu(brain,4));
	EXPECT_EQ(lneu(brain,0,3).size(), 3u);
	EXPECT_ANY_THROW(lneu(brain,0,2));
	EXPECT_ANY_THROW(dde_activity(brain, Activity::state, 2));
	brain[0].neurons[0].sources[0].push_back({1,0});
	EXPECT_ANY_THROW(lneu(brain,1));
	brain[0].x = 64;
	EXPECT_ANY_THROW(lneu(brain,0));
	brain[0].x = 0; brain[0].width = 0;
	EXPECT_ANY_THROW(lneu(brain,0));
}
TEST(C2Monitor, SynchronousRequestsTerminateAndDoNotReuseEarlierOutput) {
	const auto version = engine.version;
	const auto dialect = engine.gametype;
	engine.gametype = "c2";
	engine.version = 2;
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv 23"), "23");
	EXPECT_EQ(engine.executeSelectedC2Command(""), "");
	EXPECT_EQ(engine.executeSelectedC2Command("inst,dde: lneu 0,endm"), "0000");
	EXPECT_EQ(engine.executeSelectedC2Command("endm"), "");
	engine.version = 1;
	EXPECT_ANY_THROW(engine.executeSelectedC2Command("endm"));
	engine.version = version;
	engine.gametype = dialect;
}

TEST(C2Monitor, EachRequestBindsTheCurrentSelection) {
	struct Subject : Agent {
		explicit Subject(unsigned char genus) : Agent(4, genus, 1, 0) {}
		CompoundPart* part(unsigned int) override { return nullptr; }
	};
	const auto version = engine.version;
	const auto dialect = engine.gametype;
	const auto selection = world.selectedcreature;
	engine.version = 2; engine.gametype = "c2";
	auto first = std::make_shared<Subject>(1);
	auto second = std::make_shared<Subject>(2);
	world.selectedcreature = first.get();
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv ownr"), std::to_string(first->getUNID()));
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv gnus"), "1");
	world.selectedcreature = second.get();
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv ownr"), std::to_string(second->getUNID()));
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv gnus"), "2");
	world.selectedcreature = nullptr;
	const bool paused = world.paused;
	world.paused = true;
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv paus"), "1");
	world.paused = false;
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv paus"), "0");
	world.paused = paused;
	EXPECT_EQ(engine.executeSelectedC2Command("dde: putv ownr"), "0");
	world.selectedcreature = selection;
	engine.version = version; engine.gametype = dialect;
}
