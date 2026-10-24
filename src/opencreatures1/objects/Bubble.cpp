#include "Recording.h"
#include "Bubble.h"

#include "ImageManager.h"
#include "common/Ascii.h"
#include "common/render/RenderItem.h"
#include "common/render/RenderSystem.h"

#include "EngineContext.h"
#include "objects/Bubble.h"
#include "ObjectManager.h"
#include "SFCSerialization.h"
#include "fileformats/sfc/Bubble.h"

void Bubble::serialize(SFCContext& ctx, sfc::BubbleV1* bub) {
	SimpleObject::serialize(ctx, bub);
	if (ctx.is_storing()) {
		bub->life = life;
		bub->creator = ctx.dump_object(g_engine_context.objects->try_get(creator)).get();
		bub->text = text;
	} else {
		life = bub->life;
		creator = ctx.load_object(bub->creator);
		text = bub->text;
	}
}

void Bubble::bubble_tick() {
	if (life > 0 && --life == 0) {
		g_engine_context.objects->destroy_later(uid);
	}
	// follow the speaker
	if (auto* speaker = g_engine_context.objects->try_get(creator)) {
		auto at = speaker->get_bbox();
		if (!has_offset) {
			auto here = get_bbox();
			offset_x = here.x - at.x;
			offset_y = here.y - at.y;
			has_offset = true;
		}
		set_position(at.x + offset_x, at.y + offset_y);
	}
}

void Bubble::render(RenderSystem& rendersystem) const {
	SimpleObject::render(rendersystem);
	// Batch 4, Q18: one line in the game's font (CHARSET.DTA: 0 is colour 242, 1 is colour 11,
	// 2 is colour 194), centred on the band from (6, 3) to (150, 15): x = 6 + (144 - width) / 2,
	// y = 3. Characters are spaced by their width + 1.
	static ImageGallery charset;
	if (!charset) {
		charset = g_engine_context.images->get_charset_dta(242, 11, 194);
	}
	float width = 0;
	for (char ch : text) {
		width += numeric_cast<float>(charset.width(static_cast<unsigned char>(to_ascii_uppercase(ch))) + 1);
	}
	auto bbox = get_bbox();
	float x = bbox.x + 6 + (144 - width) / 2;
	const float y = bbox.y + 3;
	for (char ch : text) {
		const auto frame = static_cast<unsigned char>(to_ascii_uppercase(ch));
		if (Recording::g_capturing) {
			Recording::note(charset, frame, x, y, get_z_order() + 1);
		}
		rendersystem.add(create_render_item_texture(charset.texture, charset.texture_locations[frame], x, y, get_z_order() + 1));
		x += numeric_cast<float>(charset.width(frame) + 1);
	}
}
