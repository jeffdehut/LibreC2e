#include "EngineContext.h"
#include "KitStock.h"
#include "PathManager.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>

namespace fs = std::filesystem;

TEST(KitStock, ReadsAndWritesTheShopFiles) {
	// batch 4, Q26: a count, then per item a stock, a script, a picture, a name and a
	// description; the layout read from the shipped files
	g_engine_context.reset();
	auto dir = fs::temp_directory_path() / "opencreatures1-kitstock-test";
	fs::create_directories(dir);
	g_engine_context.paths->set_main_directory(dir);

	KitStock::Item herb;
	herb.stock = 12;
	herb.script = "inst,new: simp herb 3 0 3500 0,edit";
	herb.width = 3;
	herb.height = 2;
	herb.row_length = 3;
	herb.pixels = {0, 1, 2, 3, 4, 5};
	herb.name = "Feverfew";
	herb.description = "Reduces sleepiness";
	KitStock::save("Health", {herb, herb});

	auto items = KitStock::load("Health");
	ASSERT_EQ(items.size(), 2u);
	EXPECT_EQ(items[1].stock, 12);
	EXPECT_EQ(items[1].script, herb.script);
	EXPECT_EQ(items[1].pixels, herb.pixels);
	EXPECT_EQ(items[1].name, "Feverfew");
	EXPECT_EQ(items[1].description, "Reduces sleepiness");

	// Replenish Stocks copies HealthBU back
	items[0].stock = 0;
	KitStock::save("Health", items);
	KitStock::save("HealthBU", {herb, herb});
	EXPECT_TRUE(KitStock::replenish("Health"));
	EXPECT_EQ(KitStock::load("Health")[0].stock, 12);

	fs::remove_all(dir);
	g_engine_context.reset();
}
