#include "Recording.h"

#include <gtest/gtest.h>
#include <string>

namespace {
std::string md5(const std::string& text) {
	return Recording::md5_hex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}
} // namespace

TEST(Recording, Md5MatchesKnownValues) {
	EXPECT_EQ(md5(""), "d41d8cd98f00b204e9800998ecf8427e");
	EXPECT_EQ(md5("abc"), "900150983cd24fb0d6963f7d28e17f72");
	EXPECT_EQ(md5("The quick brown fox jumps over the lazy dog"), "9e107d9d372bb6826bd81d3542a419d6");
	// longer than a block, and exactly 64
	EXPECT_EQ(md5(std::string(64, 'a')), "014842d480b571495a4a0363793f7367");
	EXPECT_EQ(md5(std::string(1000, 'a')), "cabe45dcc9ae5b66ba86600cca6b8ba8");
}
