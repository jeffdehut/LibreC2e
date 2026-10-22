#include "Session.h"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
namespace fs = std::filesystem;
class SessionTest : public testing::Test {
protected:
	fs::path root;
	void SetUp() override {
		root = fs::temp_directory_path() / ("openc2e-session-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(root);
	}
	void TearDown() override { fs::remove_all(root); }
	fs::path installation(const char* folder, bool c2) {
		auto dir = root / folder;
		for (auto part : {"Images", "Body Data", "Genetics", "Sounds"}) fs::create_directories(dir / part);
		std::ofstream(dir / (c2 ? "CREATURES2.EXE" : "Creatures.exe")) << "fixture executable marker";
		world(dir / "World.sfc", c2);
		return dir;
	}
	void world(const fs::path& file, bool c2) {
		unsigned char header[] = {255,255,static_cast<unsigned char>(c2 ? 2 : 1),0,7,0,'M','a','p','D','a','t','a',static_cast<unsigned char>(c2 ? 1 : 0),0,0,0};
		std::ofstream out(file, std::ios::binary);
		out.write(reinterpret_cast<const char*>(header), sizeof(header));
	}
};
TEST_F(SessionTest, DetectsMetadataAndValidatesExplicitGame) {
	for (bool c2 : {false,true}) {
		auto dir = installation(c2 ? "two" : "one", c2);
		EXPECT_EQ(session::inspect(dir).game, c2 ? session::Game::c2 : session::Game::c1);
		EXPECT_THROW(session::inspect(dir, {}, c2 ? "c1" : "c2"), std::runtime_error);
	}
}
TEST_F(SessionTest, RejectsMissingAmbiguousAndMismatchedData) {
	EXPECT_THROW(session::inspect(root / "absent"), std::runtime_error);
	auto dir = installation("one", false);
	world(dir / "Wrong.sfc", true);
	EXPECT_THROW(session::inspect(dir, "Wrong.sfc"), std::runtime_error);
	std::ofstream(dir / "Creatures2.exe");
	EXPECT_THROW(session::inspect(dir), std::runtime_error);
	fs::remove(dir / "Creatures2.exe");
	fs::remove_all(dir / "Sounds");
	EXPECT_THROW(session::inspect(dir), std::runtime_error);
}
TEST_F(SessionTest, RejectsTruncationUnsupportedSchemasAndExternalWorlds) {
	auto dir = installation("one", false);
	std::ofstream(dir / "Broken.sfc") << "not an archive";
	EXPECT_THROW(session::inspect(dir, "Broken.sfc"), std::runtime_error);
	world(root / "External.sfc", false);
	EXPECT_THROW(session::inspect(dir, root / "External.sfc"), std::runtime_error);
	std::fstream out(dir / "World.sfc", std::ios::in | std::ios::out | std::ios::binary);
	out.seekp(2); out.put(9); out.close();
	EXPECT_THROW(session::inspect(dir), std::runtime_error);
}
TEST_F(SessionTest, CopiesAreDurableAndIsolatedAcrossGamesInstallationsAndWorlds) {
	auto one = installation("one", false), two = installation("two", true), other = installation("other", false);
	world(one / "Other.sfc", false);
	auto s1 = session::inspect(one);
	auto a = session::prepare(s1, root / "sessions");
	std::ofstream(a.data / "Genetics" / "generated.gen") << "generated";
	std::ofstream(a.profile / "options.ini") << "preferences";
	auto b = session::prepare(session::inspect(two), root / "sessions");
	auto c = session::prepare(session::inspect(one, "Other.sfc"), root / "sessions");
	auto d = session::prepare(session::inspect(other), root / "sessions");
	EXPECT_NE(a.root, b.root); EXPECT_NE(a.root, c.root); EXPECT_NE(a.root, d.root);
	EXPECT_FALSE(fs::exists(one / "Genetics" / "generated.gen"));
	EXPECT_FALSE(fs::exists(b.data / "Genetics" / "generated.gen"));
	EXPECT_FALSE(fs::exists(c.profile / "options.ini"));
	for (int i = 0; i < 4; ++i) {
		EXPECT_EQ(session::prepare(s1, root / "sessions").root, a.root);
		EXPECT_EQ(session::prepare(session::inspect(two), root / "sessions").root, b.root);
	}
	EXPECT_TRUE(fs::exists(a.data / "Genetics" / "generated.gen"));
	EXPECT_TRUE(fs::exists(a.profile / "options.ini"));
}
TEST_F(SessionTest, RefusesRecursiveCopiesLinksAndMismatchedManifest) {
	auto dir = installation("one", false);
	auto s = session::inspect(dir);
	EXPECT_THROW(session::prepare(s, dir / "sessions"), std::runtime_error);
	fs::create_directory_symlink(root / "external", dir / "link");
	EXPECT_THROW(session::prepare(s, root / "sessions"), std::runtime_error);
	fs::remove(dir / "link");
	auto copy = session::prepare(s, root / "sessions");
	std::ofstream(copy.root / "source.txt") << "different identity";
	EXPECT_THROW(session::prepare(s, root / "sessions"), std::runtime_error);
	EXPECT_TRUE(fs::exists(copy.world));
}
