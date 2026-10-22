#include "Scriptorium.h"

#include <gtest/gtest.h>

TEST(Creatures1Scriptorium, BasicFunctionality) {
	Scriptorium s;
	s.add(2, 1, 1, 50, "anim [1110],endm");
	s.add(2, 1, 1, 53, "anim [776],endm");
	s.add(2, 1, 1, 54, "anim [777720],endm");

	EXPECT_EQ(s.get(2, 1, 1, 50), "anim [1110],endm");
	EXPECT_EQ(s.get(2, 1, 1, 53), "anim [776],endm");
	EXPECT_EQ(s.get(2, 1, 1, 54), "anim [777720],endm");
}

TEST(Creatures1Scriptorium, MaximumNumberOfScripts) {
	Scriptorium s;
	for (int i = 0; i < 600; ++i) {
		const int family = 2;
		const int genus = 1 + i / 255;
		const int species = 1 + i % 255;
		const int scriptno = 9;
		s.add(family, genus, species, scriptno, "anim [0],endm");
	}
	EXPECT_THROW(s.add(2, 3, 91, 9, "dummy"), Exception) << "Scriptorium should max out at 600 scripts";
}

TEST(Creatures1Scriptorium, NoDuplicates) {
	Scriptorium s;
	s.add(2, 1, 1, 50, "anim [1110],endm");
	EXPECT_THROW(s.add(2, 1, 1, 50, "anim [0],endm"), Exception) << "Scriptorium should not a script number to be registered twice";
}

TEST(Creatures1Scriptorium, NoLargeGenusesOrSpecies) {
	Scriptorium s;

	EXPECT_THROW(s.add(2, 256, 1, 9, "endm"), Exception);
	EXPECT_THROW(s.get(2, 256, 1, 9), Exception);
	EXPECT_THROW(s.add(2, 1, 256, 9, "endm"), Exception);
	EXPECT_THROW(s.get(2, 1, 256, 9), Exception);
}

TEST(Creatures1Scriptorium, NoScenery) {
	// No Scenery scripts in Eden.sfc, disallow adding them.
	// Allow requesting them though because it makes the logic easier

	Scriptorium s;
	EXPECT_THROW(s.add(1, 0, 0, 9, "endm"), Exception);
	EXPECT_EQ(s.get(1, 0, 0, 9), "");
}

TEST(Creatures1Scriptorium, AllowUnusedEventNumbersAndFamilies) {
	// The highest event number the engine itself uses is 72 (Creature Death), but worlds
	// saved by the original engine can contain other scripts, e.g. "scrp 255 255 255 101".
	Scriptorium s;
	s.add(255, 255, 255, 101, "endm");
	EXPECT_EQ(s.get(255, 255, 255, 101), "endm");
}

TEST(Creatures1Scriptorium, NoOutOfRangeEventNumbersOrFamilies) {
	Scriptorium s;
	EXPECT_THROW(s.add(2, 1, 1, 256, "anim [0],endm"), Exception);
	EXPECT_THROW(s.get(2, 1, 1, 256), Exception);
	EXPECT_THROW(s.add(256, 1, 1, 9, "endm"), Exception);
	EXPECT_THROW(s.get(256, 1, 1, 9), Exception);
}

TEST(Creatures1Scriptorium, AllowWildcards) {
	// Creatures Wiki says 0 in any part of the classifier acts as a wildcard,
	// but Creatures 1 has objects and scripts with 0 as part of their
	// classifier!

	Scriptorium s;

	// allow CallButtons
	s.add(2, 2, 0, 1, "pose 1,snde butt,endm");
	EXPECT_EQ(s.get(2, 2, 0, 1), "pose 1,snde butt,endm");

	// allow generic Creatures scripts
	s.add(4, 0, 0, 1, "setv actv 0,endm");
	EXPECT_EQ(s.get(4, 0, 0, 1), "setv actv 0,endm");
}