#include "EngineContext.h"
#include "GameText.h"
#include "PathManager.h"

#include <cstdlib>
#include <gtest/gtest.h>

// The original wording comes only from a player's copy of the game: with OPENC1_GAME_DATA
// naming a Creatures 1 folder, a known line (the Emergency Kit's restore question, known here
// only by its fingerprint) is found there; without one, our own wording is used.
TEST(GameText, FindsTheOriginalLineByItsFingerprintOrFallsBack) {
	g_engine_context.reset();
	const GameText::Original restore = GameText::Original{43, 0xf8dda8b75727cf1aull};
	const char* folder = std::getenv("OPENC1_GAME_DATA");
	if (!folder) {
		EXPECT_EQ(GameText::line(restore, "ours"), "ours");
		GTEST_SKIP() << "Set OPENC1_GAME_DATA to a Creatures 1 folder to check the lookup";
	}
	g_engine_context.paths->set_main_directory(folder);
	const std::string found = GameText::line(restore, "ours");
	EXPECT_NE(found, "ours");
	EXPECT_EQ(found.size(), restore.length);
	EXPECT_EQ(GameText::fingerprint(found), restore.hash);
	g_engine_context.reset();
}
