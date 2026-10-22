#include "Blackboard.h"

#include "Bubble.h"
#include "Creature.h"
#include "ImageManager.h"
#include "WorldState.h"
#include "ObjectManager.h"
#include "SFCSerialization.h"
#include "common/Ascii.h"
#include "common/render/RenderSystem.h"
#include "fileformats/sfc/Blackboard.h"

ObjectHandle g_editing_blackboard;

// Batch 4, Q19: 16 word slots, each a word of up to 10 characters and the vocabulary entry
// it teaches; OBV0 is the current slot.

void Blackboard::blackboard_show_word(int32_t word_index) {
	if (word_index < 0 || numeric_cast<size_t>(word_index) >= words.size()) {
		throw Exception(fmt::format("Blackboard word index {} out of bounds", word_index));
	}
	current_word_index = word_index;
}

void Blackboard::blackboard_hide_word() {
	current_word_index = -1;
}

void Blackboard::blackboard_enable_edit() {
	// the current word is cleared and the player's typing goes into it
	if (obv0 < 0 || numeric_cast<size_t>(obv0) >= words.size()) {
		return;
	}
	current_word_index = obv0;
	words[numeric_cast<size_t>(obv0)].text.clear();
	editing = true;
	g_editing_blackboard = uid;
}

void Blackboard::blackboard_disable_edit() {
	editing = false;
	if (g_editing_blackboard == uid) {
		g_editing_blackboard = {};
	}
}

void Blackboard::type_character(char c) {
	if (!editing || current_word_index < 0) {
		return;
	}
	auto& text = words[numeric_cast<size_t>(current_word_index)].text;
	if (text.size() < MAX_WORD && static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 128) {
		text += c;
	}
}

void Blackboard::type_backspace() {
	if (editing && current_word_index >= 0) {
		auto& text = words[numeric_cast<size_t>(current_word_index)].text;
		if (!text.empty()) {
			text.pop_back();
		}
	}
}

void Blackboard::blackboard_emit_eyesight(int32_t word_index) {
	// every creature that can see the board is taught the word
	if (word_index < 0 || numeric_cast<size_t>(word_index) >= words.size()) {
		return;
	}
	const auto& word = words[numeric_cast<size_t>(word_index)];
	for (auto* obj : *g_engine_context.objects) {
		if (auto* creature = obj->as_creature(); creature && creature->can_see(*this)) {
			creature->teach_word(word.value, word.text);
		}
	}
}

void Blackboard::blackboard_emit_earshot(int32_t word_index) {
	// the board "says" the word in a 20-tick bubble; creatures ignore speech from a board
	if (word_index < 0 || numeric_cast<size_t>(word_index) >= words.size() || words[numeric_cast<size_t>(word_index)].text.empty()) {
		return;
	}
	make_speech_bubble(*this, words[numeric_cast<size_t>(word_index)].text, 20);
}

void Blackboard::render(RenderSystem& rendersystem) const {
	Object::render(rendersystem);

	if (current_word_index >= 0) {
		const auto& word = words[numeric_cast<size_t>(current_word_index)];

		float x = get_bbox().x + text_x_position;
		const float y = get_bbox().y + text_y_position;

		// TODO: What should the actual z-order be? Should this actually be drawn on the
		// main blackboard part?
		const int32_t z = get_z_order() + 1;

		std::string text = word.text;
		if (editing && (g_engine_context.world->tick / 4) % 2 == 0) {
			text += '_'; // the cursor, after the text
		}
		for (size_t i = 0; i < text.size(); ++i) {
			const unsigned int frame = static_cast<unsigned char>(to_ascii_uppercase(text[i]));
			rendersystem.add(create_render_item_texture(
				charset_sprite.texture,
				charset_sprite.texture_locations[frame],
				x, y, z));
			x += numeric_cast<float>(charset_sprite.width(numeric_cast<int32_t>(frame)) + 1);
		}
	}
}

void Blackboard::serialize(SFCContext& ctx, sfc::BlackboardV1* bbd) {
	if (ctx.is_storing()) {
		bbd->background_color = background_color;
		bbd->chalk_color = chalk_color;
		bbd->alias_color = alias_color;
		bbd->text_x_position = text_x_position;
		bbd->text_y_position = text_y_position;
		for (size_t i = 0; i < bbd->words.size(); ++i) {
			bbd->words[i].value = words[i].value;
			bbd->words[i].text = words[i].text;
		}
	} else {
		background_color = bbd->background_color;
		chalk_color = bbd->chalk_color;
		alias_color = bbd->alias_color;
		text_x_position = bbd->text_x_position;
		text_y_position = bbd->text_y_position;
		for (size_t i = 0; i < bbd->words.size(); ++i) {
			auto& word = bbd->words[i];
			words[i].value = word.value;
			words[i].text = word.text;
		}
		charset_sprite = g_engine_context.images->get_charset_dta(bbd->background_color, bbd->chalk_color, bbd->alias_color);
	}
	CompoundObject::serialize(ctx, bbd);
}