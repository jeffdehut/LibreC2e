#include "opencreatures1/DullPart.h"
#include "opencreatures1/MapManager.h"

#include <gtest/gtest.h>

// Animation strings: digits are frames 0-9, letters from 'a' frames 10 and up, and 'R'
// repeats (batch 5, Q4). The Grendel Mother's birth script uses "anim [99a]".
TEST(Animation, LettersAreFramesTenAndUp) {
	DullPart part;
	part.set_animation(0, "99a");
	EXPECT_EQ(part.pose(), 9);
	part.update_animation();
	EXPECT_EQ(part.pose(), 9);
	part.update_animation();
	EXPECT_EQ(part.pose(), 10);
	part.set_animation(0, "bR");
	EXPECT_EQ(part.pose(), 11);
}

// The room lookup for limits (batch 5, Q1): rooms whose x range includes the left edge, both
// ends inclusive; the floor nearest the bottom edge wins, the first room on a tie; with no
// room, the old limits with their bottom at 9999.
TEST(Limits, RoomLookup) {
	MapManager map;
	map.add_room(Room{0, 100, 100, 200, 0});
	map.add_room(Room{100, 100, 300, 260, 0});
	map.add_room(Room{100, 0, 300, 90, 0});

	auto in_first = map.limits_for(50, 190, Rect2i{});
	EXPECT_EQ(in_first, (Rect2i{0, 100, 100, 100}));
	// x = 100 is in both of the first two rooms; the floor at 200 is nearer to 195
	EXPECT_EQ(map.limits_for(100, 195, Rect2i{}), (Rect2i{0, 100, 100, 100}));
	// nearest floor, above or below
	EXPECT_EQ(map.limits_for(150, 100, Rect2i{}), (Rect2i{100, 0, 200, 90}));
	EXPECT_EQ(map.limits_for(150, 240, Rect2i{}), (Rect2i{100, 100, 200, 160}));
    // Approved correction F022 supersedes the old unbounded-floor expectation.
    EXPECT_EQ(map.limits_for(500, 100, Rect2i{10, 20, 30, 40}), (Rect2i{100, 100, 200, 160}));
    MapManager empty;
    EXPECT_EQ(empty.limits_for(500, 100, {}), (Rect2i{0, 0, 8352, 1200}));
}
