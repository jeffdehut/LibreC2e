#include "EngineContext.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"

#include <gtest/gtest.h>

// The face (batch 2, Q4-Q5) and the angry face from LibreCreatures row 88.
TEST(Expression, AngerStrongestShowsTheAngryFaceOnlyWhenEnabled) {
	g_engine_context.reset();
	auto handle = g_engine_context.objects->add<Creature>();
	auto& c = *g_engine_context.objects->try_get(handle)->as_creature();
	c.loci.drives.fill(0);
	c.loci.drives[11] = 230; // anger, past the upper threshold
	c.loci.drives[2] = 150; // hunger, noticeable but weaker

	g_creature_angry_face = false; // the original: just sad
	c.update_expression();
	EXPECT_EQ(c.expression, 2);

	g_creature_angry_face = true;
	c.update_expression();
	EXPECT_EQ(c.expression, 3);

	// a tie goes to the other drive: still sad
	c.loci.drives[2] = 230;
	c.update_expression();
	EXPECT_EQ(c.expression, 2);

	// anger under the upper threshold: no angry face
	c.loci.drives.fill(0);
	c.loci.drives[11] = 180;
	c.update_expression();
	EXPECT_EQ(c.expression, 0);

	g_creature_angry_face = false;
	g_engine_context.reset();
}
