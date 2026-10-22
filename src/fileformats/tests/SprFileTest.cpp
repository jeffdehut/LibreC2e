#include "fileformats/sprImage.h"

#include "common/io/SpanReader.h"
#include "common/io/VectorWriter.h"

#include <gtest/gtest.h>
#include <stdint.h>
#include <vector>

// Two images: 2x1 pixels {1, 2} and 1x2 pixels {3, 4}. Header offsets point directly after
// the header, the way both the game's shipped sprites and its creature sprites record them.
static std::vector<uint8_t> make_spr(bool creatures1_gap) {
	std::vector<uint8_t> spr = {
		2, 0, // image count
		18, 0, 0, 0, 2, 0, 1, 0, // offset 18, 2x1
		20, 0, 0, 0, 1, 0, 2, 0, // offset 20, 1x2
	};
	if (creatures1_gap) {
		spr.insert(spr.end(), {0, 0});
	}
	spr.insert(spr.end(), {1, 2, 3, 4});
	return spr;
}

static std::vector<uint8_t> pixels(const Image& img) {
	return {img.data.data(), img.data.data() + img.data.size()};
}

TEST(fileformats, SprFile) {
	auto spr = make_spr(false);
	SpanReader in(spr);
	auto images = ReadSprFile(in);
	ASSERT_EQ(images.size(), 2u);
	EXPECT_EQ(pixels(images[0]), (std::vector<uint8_t>{1, 2}));
	EXPECT_EQ(pixels(images[1]), (std::vector<uint8_t>{3, 4}));
}

TEST(fileformats, SprFileWithCreatures1Gap) {
	// Creature sprites written by the original Creatures 1 engine (Images/<moniker>.spr)
	// have two extra bytes between the header and the image data.
	auto spr = make_spr(true);
	SpanReader in(spr);
	auto images = ReadSprFile(in);
	ASSERT_EQ(images.size(), 2u);
	EXPECT_EQ(pixels(images[0]), (std::vector<uint8_t>{1, 2}));
	EXPECT_EQ(pixels(images[1]), (std::vector<uint8_t>{3, 4}));
}

TEST(fileformats, SprFileWithMetadataAndCreatures1Gap) {
	auto spr = make_spr(true);
	SpanReader in(spr);
	auto data = ReadSprFileWithMetadata(in, 1, 1);
	ASSERT_EQ(data.images.size(), 1u);
	EXPECT_EQ(data.offsets[0], 20);
	EXPECT_EQ(pixels(data.images[0]), (std::vector<uint8_t>{3, 4}));
}

TEST(fileformats, WriteSprFileInBothLayouts) {
	// Writing the images back gives the same bytes, with or without the creature sprite gap.
	for (bool gap : {false, true}) {
		auto spr = make_spr(gap);
		SpanReader in(spr);
		auto images = ReadSprFile(in);
		VectorWriter out;
		WriteSprFile(out, images, gap);
		EXPECT_EQ(out.vector(), spr);
	}
}
