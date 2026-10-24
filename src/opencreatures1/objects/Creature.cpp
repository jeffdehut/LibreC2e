#include "Recording.h"
#include <optional>
#include "Creature.h"
#include "HdArt.h"

#include "CreatureGenetics.h"
#include "../../../third_party/librecreatures/C1Palette.h"
#include "CreatureSprites.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "CreatureBrain.h"
#include "ViewportManager.h"
#include "Scriptorium.h"
#include "MacroManager.h"
#include "ObjectManager.h"
#include "C1SoundManager.h"
#include "CompoundObject.h"
#include "MessageManager.h"
#include "PointerManager.h"
#include "SFCSerialization.h"
#include "common/Color.h"
#include "common/Random.h"
#include "common/render/RenderItem.h"
#include "common/render/RenderSystem.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/Creature.h"
#include "fileformats/sfc/SimpleObject.h"

#include <algorithm>
#include <cmath>
#include <fmt/core.h>
#include <fmt/ranges.h>

// Limbs in each chain, outwards from the body.
static std::vector<Limb*> chain_limbs(Limb* first) {
	std::vector<Limb*> result;
	for (Limb* l = first; l; l = l->next_limb.get()) {
		result.push_back(l);
	}
	return result;
}

static std::array<Limb*, 6> chain_roots(const Creature& c) {
	return {c.head.get(), c.left_thigh.get(), c.right_thigh.get(), c.left_arm.get(), c.right_arm.get(), c.tail.get()};
}

std::vector<DullPart*> Creature::all_parts() {
	std::vector<DullPart*> result{body.get()};
	for (Limb* root : chain_roots(*this)) {
		for (Limb* l : chain_limbs(root)) {
			result.push_back(l);
		}
	}
	return result;
}

std::vector<const DullPart*> Creature::all_parts() const {
	auto parts = const_cast<Creature*>(this)->all_parts();
	return {parts.begin(), parts.end()};
}

CreatureFacing Creature::facing() const {
	int32_t frame = body->pose();
	if (frame < 4) {
		return FACING_RIGHT;
	}
	if (frame < 8) {
		return FACING_LEFT;
	}
	return frame == 8 ? FACING_CAMERA : FACING_AWAY;
}

static SkeletonPose skeleton_pose_for(const Creature& c) {
	SkeletonPose pose;
	pose.body_data = &c.body->body_data;
	pose.body_frame = c.body->pose();
	auto roots = chain_roots(c);
	for (size_t i = 0; i < roots.size(); ++i) {
		for (Limb* l : chain_limbs(roots[i])) {
			pose.chains[i].push_back({&l->limb_data, l->pose()});
		}
	}
	return pose;
}

void Creature::reposition() {
	// Sprites wrap independently; the simulation's planted foot must wrap too.
	// Otherwise a visible creature can accumulate x > world width and lose hearing,
	// sight and room lookup. Preserve deliberately off-world holding positions.
	if (!unplaced && footy >= 0 && footy <= CREATURES1_WORLD_HEIGHT)
		footx = (footx % CREATURES1_WORLD_WIDTH + CREATURES1_WORLD_WIDTH) % CREATURES1_WORLD_WIDTH;
	auto pose = skeleton_pose_for(*this);
	auto positions = position_skeleton(pose, !downfoot_right, footx, footy);
	const auto current_facing = facing();

	body->set_position(numeric_cast<float>(positions.body.x), numeric_cast<float>(positions.body.y));
	body->set_z_order(numeric_cast<int32_t>(z_order));
	auto roots = chain_roots(*this);
	for (size_t i = 0; i < roots.size(); ++i) {
		auto limbs = chain_limbs(roots[i]);
		for (size_t j = 0; j < limbs.size(); ++j) {
			limbs[j]->set_position(numeric_cast<float>(positions.chains[i][j].x), numeric_cast<float>(positions.chains[i][j].y));
			limbs[j]->set_z_order(numeric_cast<int32_t>(z_order) + skeleton_z_offset(static_cast<CreatureLimbChain>(i), j, current_facing));
		}
	}

	// A carried object's sprite centre goes on the end of the carrying arm, one plane in
	// front of the creature facing right or towards the camera, one behind otherwise (spec Q12).
	auto hand = carry_point();
	const int32_t plane = numeric_cast<int32_t>(z_order) + ((direction == 1 || direction == 2) ? 1 : -1);
	for (auto* obj : *g_engine_context.objects) {
		if (obj->carrier == this->uid && obj->movement_status == MOVEMENT_CARRIED) {
			auto bbox = obj->get_bbox();
			obj->set_position(numeric_cast<float>(hand.x) - bbox.width / 2, numeric_cast<float>(hand.y) - bbox.height / 2);
			if (auto* part = obj->get_part(0)) {
				part->set_z_order(plane);
			}
		}
	}
}

DullPart* Creature::pose_part(size_t character) {
	// pose string order: head, body, left leg (thigh, shin, foot), right leg, left arm
	// (humerus, radius), right arm, tail (root, tip)
	auto limb = [](Limb* first, size_t n) -> DullPart* {
		for (Limb* l = first; l; l = l->next_limb.get(), --n) {
			if (n == 0) {
				return l;
			}
		}
		return nullptr;
	};
	switch (character) {
		case 1: return head.get();
		case 2: return body.get();
		case 3:
		case 4:
		case 5: return limb(left_thigh.get(), character - 3);
		case 6:
		case 7:
		case 8: return limb(right_thigh.get(), character - 6);
		case 9:
		case 10: return limb(left_arm.get(), character - 9);
		case 11:
		case 12: return limb(right_arm.get(), character - 11);
		case 13:
		case 14: return limb(tail.get(), character - 13);
		default: return nullptr;
	}
}

// The frame for a part value: 0-3 facing right, +4 facing left, 8 towards the camera, 9 away
// (batch 2, Q4).
static int32_t frame_for_value(int32_t facing, int32_t value) {
	if (value == 4 || (value < 4 && facing == 1)) {
		return 8;
	}
	if (value == 5 || (value < 4 && facing == 0)) {
		return 9;
	}
	return facing == 3 ? value + 4 : value;
}

void Creature::update_frames() {
	const int32_t facing_value = pose_values[0];
	for (size_t i = 1; i < pose_values.size(); ++i) {
		DullPart* part = pose_part(i);
		if (!part) {
			continue;
		}
		int32_t frame = frame_for_value(facing_value, pose_values[i]);
		// what the original saves for each part (as LibreCreatures has it): the value reached,
		// and its frame without the face or closed eyes
		auto* saved = static_cast<BodyPart*>(part);
		saved->angle = pose_values[i];
		saved->view = frame;
		if (part == head.get()) {
			// expressions show on the front-facing head: frame 10 or 11, or 12 for the angry
			// face (LibreCreatures row 88)
			if (frame == 8 && (expression == 1 || expression == 2 || expression == 3)) {
				frame = 9 + expression;
			}
			if (!eyes_open) {
				frame += 13;
			}
		}
		part->set_pose(frame);
	}
}

void Creature::init_pose_values_from_frames() {
	// facing from the body's frame, part values from their frames
	int32_t body_frame = body->pose();
	pose_values[0] = static_cast<int8_t>(body_frame < 4 ? 2 : body_frame < 8 ? 3 : body_frame == 8 ? 1 : 0);
	for (size_t i = 1; i < pose_values.size(); ++i) {
		DullPart* part = pose_part(i);
		if (!part) {
			continue;
		}
		int32_t frame = part->pose();
		if (part == head.get()) {
			frame = frame >= 13 ? frame - 13 : frame;
			if (frame >= 10) {
				frame = 8;
			}
		}
		pose_values[i] = static_cast<int8_t>(frame < 8 ? frame % 4 : frame == 8 ? 4 : 5);
	}
	pose_targets = pose_values;
	target_facing = pose_values[0];
}

int8_t Creature::head_look_value() const {
	// batch 2, Q3: look up or down at the target point
	auto* it = g_engine_context.objects->try_get(attention);
	if (!it || !has_target) {
		return 1;
	}
	const bool it_behind = it->get_z_order() < numeric_cast<int32_t>(z_order);
	const float d = std::abs(target_x - ground_x());
	if (d < 16) {
		return it_behind ? 5 : 4;
	}
	const int32_t column = numeric_cast<int32_t>(d) / 32;
	if (column > 3) {
		return 1;
	}
	static const int8_t LOOK[10][4] = {
		{3, 3, 3, 3}, {3, 3, 3, 2}, {3, 3, 2, 2}, {3, 2, 2, 2}, {2, 2, 2, 2},
		{1, 2, 2, 2}, {0, 1, 2, 2}, {0, 1, 1, 2}, {0, 0, 1, 1}, {0, 0, 1, 1},
	};
	const int32_t row = std::clamp(numeric_cast<int32_t>(target_y - body->y()) / 8 + 5, 0, 9);
	return LOOK[row][column];
}

void Creature::turn_step() {
	// Turning goes through the camera-facing frames, one step of this table at a time
	// (batch 2, Q3). Each entry is (new facing, new head value).
	int32_t current = pose_values[0];
	const int32_t head_value = pose_values[1];
	int32_t requested = target_facing;
	if (head_value == 4 && current == 0) {
		requested = 2;
	}
	struct Step {
		int8_t facing;
		int8_t head;
	};
	static const Step TABLE[5][4] = {
		{{2, 5}, {2, 4}, {2, 1}, {2, 4}}, // facing 2, head 0-3
		{{0, 5}, {1, 4}, {2, 1}, {1, 4}}, // facing 2, head 4-5
		{{3, 5}, {3, 4}, {3, 4}, {3, 1}}, // facing 3, head 0-3
		{{0, 5}, {1, 4}, {1, 4}, {3, 1}}, // facing 3, head 4-5
		{{0, 5}, {1, 4}, {2, 1}, {3, 1}}, // facing 0 or 1
	};
	size_t row = current == 2 ? (head_value >= 4 ? 1 : 0) : current == 3 ? (head_value >= 4 ? 3 : 2) : 4;
	Step step = TABLE[row][requested];
	if (facing_from_question) {
		auto* it = g_engine_context.objects->try_get(attention);
		const bool it_behind = it && it->get_z_order() < numeric_cast<int32_t>(z_order);
		if (it_behind && step.facing == 1) {
			step.facing = 0;
		}
		if (it_behind && step.head == 4) {
			step.head = 5;
		}
	}
	pose_values[0] = step.facing;
	pose_values[1] = step.head;
	direction = numeric_cast<uint8_t>(step.facing);
}

void Creature::set_pose_string(const std::string& p) {
	if (p.size() != 15) {
		throw Exception(fmt::format("Creature pose string should be 15 characters, but got {:?}", p));
	}
	// facing: digits set it, '?' faces the target point and '!' faces away from it
	if (p[0] >= '0' && p[0] <= '3') {
		target_facing = static_cast<int8_t>(p[0] - '0');
		facing_from_question = false;
	} else if ((p[0] == '?' || p[0] == '!') && has_target && g_engine_context.objects->try_get(attention)) {
		const bool left = target_x < ground_x();
		target_facing = static_cast<int8_t>((left != (p[0] == '!')) ? 3 : 2);
		facing_from_question = p[0] == '?';
	}
	head_looks_at_it = p[1] == '?';
	for (size_t i = 1; i < pose_targets.size(); ++i) {
		const char c = p[i];
		pose_targets[i] = static_cast<int8_t>(c >= '0' && c <= '5' ? c - '0' : -1);
	}
	if (head_looks_at_it) {
		pose_targets[1] = head_look_value();
	}
	current_pose = p;
	// the facing step happens at once
	if (pose_values[0] != target_facing) {
		turn_step();
		update_frames();
		reposition();
	}
}

bool Creature::pose_reached() const {
	auto* self = const_cast<Creature*>(this);
	for (size_t i = 1; i < pose_targets.size(); ++i) {
		if (pose_targets[i] >= 0 && self->pose_part(i) && pose_values[i] != pose_targets[i]) {
			return false;
		}
	}
	return true;
}

void Creature::step_pose() {
	if (head_looks_at_it) {
		pose_targets[1] = head_look_value();
	}
	bool moved = false;
	if (pose_values[0] != target_facing) {
		turn_step();
		moved = true;
	}
	for (size_t i = 1; i < pose_values.size(); ++i) {
		const int8_t target = pose_targets[i];
		int8_t& value = pose_values[i];
		if (target < 0 || value == target || !pose_part(i)) {
			continue;
		}
		// one step per tick for angles; towards the camera or away jumps (batch 2, Q2-Q3)
		if (i == 1 && value >= 4 && target <= 3) {
			value = 1;
		} else if (value >= 4 || target >= 4) {
			value = target;
		} else {
			value = static_cast<int8_t>(value + (target > value ? 1 : -1));
		}
		muscle_counter++;
		moved = true;
	}
	update_frames();
	if (!moved) {
		reposition();
		return;
	}

	reposition();
	settle_on_limits();
}

void Creature::settle_on_limits() {
	// Batch 5, Q1 and 1b. After the body is re-posed round the ground point: if the other
	// foot is now below the bottom of the limits, the creature has taken a step. The ground
	// point moves to that foot, on the limit bottom, and the feet swap roles. A creature
	// above its floor never steps, so it hangs there; one below it is lifted.
	auto pose = skeleton_pose_for(*this);
	const bool left_foot_down = !downfoot_right;
	auto planted = skeleton_chain_tip(pose, left_foot_down ? CHAIN_LEFT_LEG : CHAIN_RIGHT_LEG);
	auto swinging = skeleton_chain_tip(pose, left_foot_down ? CHAIN_RIGHT_LEG : CHAIN_LEFT_LEG);
	if (footy + swinging.y - planted.y > limit.bottom()) {
		footx += swinging.x - planted.x;
		footy = limit.bottom();
		downfoot_right = !downfoot_right;
		reposition();
		// only heard on or near the screen
		g_engine_context.sounds->play_uncontrolled_sound("step", get_body_bbox());
	}

	// The whole body is kept inside the limits horizontally, with no margin: pushed back by
	// the overlap, or centred if it's wider than them. Being pushed back (not centred) is
	// felt at the next sense update.
	auto box = get_body_bbox();
	float shift = 0;
	if (box.width > numeric_cast<float>(limit.width)) {
		shift = numeric_cast<float>(limit.x) + (numeric_cast<float>(limit.width) - box.width) / 2 - box.x;
	} else if (box.right() > numeric_cast<float>(limit.right())) {
		shift = numeric_cast<float>(limit.right()) - box.right();
		pushed_by_limit = true;
	} else if (box.x < numeric_cast<float>(limit.x)) {
		shift = numeric_cast<float>(limit.x) - box.x;
		pushed_by_limit = true;
	}
	if (shift != 0) {
		footx += numeric_cast<int32_t>(std::lround(shift));
		reposition();
	}
}

void Creature::set_pose_number(int32_t pose) {
	if (pose < 0 || numeric_cast<size_t>(pose) >= poses.size() || poses[numeric_cast<size_t>(pose)].empty()) {
		throw Exception(fmt::format("Creature {} has no pose {}", moniker, pose));
	}
	pose_number = pose;
	set_pose_string(poses[numeric_cast<size_t>(pose)]);
}

static int32_t animation_pose(const std::string& animation, size_t index) {
	return (animation[index] - '0') * 10 + (animation[index + 1] - '0');
}

void Creature::set_pose_animation(const std::string& animation) {
	const bool repeats = !animation.empty() && animation.back() == 'R';
	const size_t digits = animation.size() - (repeats ? 1 : 0);
	if (digits == 0 || digits % 2 != 0) {
		throw Exception(fmt::format("Creature animation should be pairs of digits, but got {:?}", animation));
	}
	for (size_t i = 0; i < digits; ++i) {
		if (animation[i] < '0' || animation[i] > '9') {
			throw Exception(fmt::format("Creature animation should be pairs of digits, but got {:?}", animation));
		}
	}
	pose_animation = animation;
	pose_animation_index = 2;
	set_pose_number(animation_pose(animation, 0));
}

void Creature::walk() {
	// the gait with the strongest gait locus among the gaits the genome defines, ties to the
	// lower gait, gait 0 if none is above 0 (spec Q25). As LibreCreatures has it (checked
	// against the original), the last gait is only for walking to something it attends to.
    std::optional<size_t> gait;
    uint8_t best = 0;
    const bool attending = g_engine_context.objects->try_get(attention) != nullptr;
    for (size_t i = 0; i < gait_animations.size() && i < loci.gait.size(); ++i) {
        if (i == loci.gait.size() - 1 && !attending) {
            continue;
        }
        if (!gait_animations[i].empty() && (!gait || loci.gait[i] > best)) {
            best = loci.gait[i];
            gait = i;
        }
    }
    if (gait) set_pose_animation(gait_animations[*gait]);
    else pose_animation.clear();
}

// Moves the pose one step towards its target, and the animation on to its next pose once the
// current one is reached.
// TODO: the specs don't say when an animation moves to its next pose; once the current one is
// reached is assumed
static void tick_pose(Creature& c) {
	c.step_pose();
	if (c.pose_animation.empty() || !c.pose_reached()) {
		return;
	}
	const bool repeats = c.pose_animation.back() == 'R';
	const size_t digits = c.pose_animation.size() - (repeats ? 1 : 0);
	if (c.pose_animation_index >= digits) {
		if (!repeats) {
			c.pose_animation.clear();
			return;
		}
		c.pose_animation_index = 0;
	}
	int32_t pose = animation_pose(c.pose_animation, c.pose_animation_index);
	c.pose_animation_index += 2;
	c.set_pose_number(pose);
}

void creature_tick_pose(Creature& c) {
	tick_pose(c);
}

SkeletonPoint Creature::carry_point() const {
	auto pose = skeleton_pose_for(*this);
	auto tip = skeleton_chain_tip(pose, CHAIN_RIGHT_ARM);
	return {numeric_cast<int32_t>(body->x()) + tip.x, numeric_cast<int32_t>(body->y()) + tip.y};
}

void Creature::drop_carried_objects() {
	for (auto* obj : *g_engine_context.objects) {
		if (obj->carrier == this->uid) {
			g_engine_context.messages->mesg_writ(this->uid, obj->uid, MESSAGE_DROP);
		}
	}
	// Approved correction F013: empty-handed drop has no learning/chemistry effect.
}

void Creature::move_by(float xdiff, float ydiff) {
	footx += numeric_cast<int32_t>(xdiff);
	footy += numeric_cast<int32_t>(ydiff);
	reposition();
}

Rect2f Creature::get_body_bbox() const {
	float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
	for (const DullPart* part : all_parts()) {
		// Keep a seam-crossing body contiguous around its ground point even though
		// individual sprite coordinates are wrapped into the world's image range.
		float x = part->x();
		if (footy >= 0 && footy <= CREATURES1_WORLD_HEIGHT) {
			x = ground_x() + std::remainder(x - ground_x(), numeric_cast<float>(CREATURES1_WORLD_WIDTH));
		}
		left = std::min(left, x);
		top = std::min(top, part->y());
		right = std::max(right, x + numeric_cast<float>(part->width()));
		bottom = std::max(bottom, part->y() + numeric_cast<float>(part->height()));
	}
	return Rect2f{left, top, right - left, bottom - top};
}

void Creature::render(RenderSystem& rendersystem) const {
	for (const DullPart* part : all_parts()) {
		if (!part->gallery().has_frame(part->frame())) {
			continue;
		}
		if (Recording::g_capturing) {
			Recording::note(part->gallery(), part->frame(), part->x(), part->y(), part->z_order());
		}
		rendersystem.add(HdArt::render_item(part->gallery(), part->frame(), part->x(), part->y(), part->z_order()));
	}
}

const DullPart* Creature::get_part(int32_t partno) const {
	// TODO: no idea
	if (partno == 0) {
		return body.get();
	}
	return nullptr;
}

static std::unique_ptr<Limb> sfc_load_limb(sfc::LimbV1* p) {
	if (!p) {
		return nullptr;
	}
	auto limb = std::make_unique<Limb>();
	static_cast<DullPart&>(*limb) = sfc_load_entity(p);
	limb->angle = p->angle;
	limb->view = p->view;
	limb->limb_data = p->limb_data;
	limb->next_limb = sfc_load_limb(p->next.get());
	return limb;
}

static std::shared_ptr<sfc::LimbV1> sfc_dump_limb(const Limb* limb, const std::shared_ptr<sfc::CGalleryV1>& gallery) {
	if (!limb) {
		return nullptr;
	}
	// sorta implemented
	auto p = std::make_shared<sfc::LimbV1>();
	*static_cast<sfc::EntityV1*>(p.get()) = *sfc_dump_entity(*limb, gallery);
	p->limb_data = limb->limb_data;
	p->next = sfc_dump_limb(limb->next_limb.get(), gallery);

	// not implemented
	p->angle = limb->angle;
	p->view = limb->view;

	return p;
};

void Creature::serialize(SFCContext& ctx, sfc::CreatureV1* crea) {
	if (ctx.is_loading()) {
		// not implemented
		moniker = crea->moniker;
		mother = crea->mother;
		father = crea->father;
		genome = load_genome(crea->moniker);
		if (genome) {
			const auto controls = creature_palette_controls(*genome, crea->sex);
			std::copy_n(controls.begin(), 3, pigments.begin());
			pigment_rotation = controls[3];
			pigment_swap = controls[4];
		}

		// The original engine keeps each creature's sprite file in Images/<moniker>.spr. If it's
		// missing (e.g. a world copied without its Images folder), rebuild it from body parts.
		std::vector<std::string> parts;
		try {
			parts = creature_body_part_sprites_for_saved_gallery(*crea);
		} catch (std::exception& e) {
			fmt::print("WARN [SFCLoader] couldn't match creature {}'s sprites to body parts: {}\n", crea->moniker, e.what());
		}
		if (crea->gallery && !g_engine_context.images->has_spr_image(crea->gallery->filename)) {
			if (parts.empty()) {
				throw Exception(fmt::format("Creature {} has no sprite file and its body parts couldn't be worked out", crea->moniker));
			}
			fmt::print("* Rebuilding sprite for creature {} from body parts: {}\n", crea->moniker, fmt::join(parts, " "));
			const auto remap = openc1::librecreatures::build_creature_palette_remap(
				g_engine_context.images->default_palette(), pigments[0], pigments[1], pigments[2], pigment_rotation, pigment_swap);
			g_engine_context.images->add_joined_spr_image(crea->gallery->filename, parts, "", &remap);
		}
		sprite_parts = parts;
		// the breeds (head, body, legs, arms) of its parts, for rebuilding the body as it ages
		if (parts.size() == 12) {
			breeds = {parts[0][3] - '0', parts[1][3] - '0', parts[2][3] - '0', parts[8][3] - '0'};
		}

		// sorta implemented
		body = std::make_unique<Body>();
		static_cast<DullPart&>(*body) = sfc_load_entity(crea->body.get());
		body->body_data = crea->body->body_data;
		body->angle = crea->body->angle;
		body->view = crea->body->view;

		head = sfc_load_limb(crea->head.get());
		left_thigh = sfc_load_limb(crea->left_thigh.get());
		right_thigh = sfc_load_limb(crea->right_thigh.get());
		left_arm = sfc_load_limb(crea->left_arm.get());
		right_arm = sfc_load_limb(crea->right_arm.get());
		tail = sfc_load_limb(crea->tail.get());
		// the pose as saved is where every part is heading
		init_pose_values_from_frames();
		// remembered positions and learned chemical averages per attention category (batch 2, Q9)
		for (size_t c = 0; c < attention_memory.size(); ++c) {
			const auto& pos = crea->object_positions[c];
			// as LibreCreatures has it: a place at or below the world's bottom is unknown (the
			// original parks killed objects at (1000, 4000), and remembers them there)
			attention_memory[c].has_position = (pos.x != -1 || pos.y != -1) && pos.y < 1200;
			attention_memory[c].last_x = numeric_cast<float>(pos.x);
			attention_memory[c].last_y = numeric_cast<float>(pos.y);
			for (size_t d = 0; d < 16; ++d) {
				learned_chemicals[c][d] = static_cast<uint8_t>(std::min<uint32_t>(crea->goals[c][d], 255));
			}
		}

		// sorta implemented
		direction = crea->direction;
		downfoot_right = crea->downfoot_right;
		footx = crea->footx;
		footy = crea->footy;

		z_order = saved_z_order = crea->z_order;
		// we used to create creatures on plane 100, and save the hand's plane for one held
		if (saved_z_order < 1000 || saved_z_order > 3000) {
			choose_plane();
		}
		current_pose = crea->current_pose;
		expression = crea->expression;
		eyes_open = crea->eyes_open;
		asleep = crea->asleep;

		// sorta implemented
		poses = crea->poses;
		gait_animations = crea->gait_animations;
        auto trim_pose = [&](std::string& pose) {
            if (pose.size() > 15) {
                fmt::print("WARN [SFCLoader] Truncating overlong pose for {} to 15 characters\n", moniker);
                pose.resize(15);
            }
        };
        trim_pose(current_pose);
        for (auto& pose : poses) trim_pose(pose);
        for (auto& gait : gait_animations) if (gait.size() > 17) {
            const bool repeat = gait.back() == 'R';
            fmt::print("WARN [SFCLoader] Truncating overlong gait for {} to eight pose pairs\n", moniker);
            gait.resize(16);
            if (repeat) gait += 'R';
        }

		// not implemented
		vocabulary = crea->vocabulary;
		object_positions = crea->object_positions;
		stimuli = crea->stimuli;
		brain = crea->brain;
		biochemistry = crea->biochemistry;
		sex = crea->sex;
		age = crea->age;

		// sorta implemented
		biotick = crea->biotick;

		// not implemented
		gamete = crea->gamete;
		zygote = crea->zygote;
		dead = crea->dead;

		// sorta implemented
		age_ticks = crea->age_ticks;

		// not implemented
		dreaming = crea->dreaming;
		instincts = crea->instincts;
		goals = crea->goals;
		zzzz = ctx.load_object(crea->zzzz.get());
		voices_lookup = crea->voices_lookup;
		voices = crea->voices;
		// The original writes the register's monikers in hex, father first. We used to write
		// them plain, mother first; plain ones are taken as ours.
		history_moniker = register_moniker(crea->history_moniker);
		history_name = crea->history_name;
		if (is_register_moniker_text(crea->history_dads_moniker) || is_register_moniker_text(crea->history_moms_moniker)) {
			history_dads_moniker = register_moniker(crea->history_dads_moniker);
			history_moms_moniker = register_moniker(crea->history_moms_moniker);
		} else {
			history_moms_moniker = crea->history_dads_moniker;
			history_dads_moniker = crea->history_moms_moniker;
		}
		history_birthday = crea->history_birthday;
		history_birthplace = crea->history_birthplace;
		history_owner_name = crea->history_owner_name;
		history_owner_phone = crea->history_owner_phone;
		history_owner_address = crea->history_owner_address;
		history_owner_email = crea->history_owner_email;
	} else {
		// not implemented
		crea->moniker = moniker;
		crea->mother = mother;
		crea->father = father;

		// sorta implemented
		crea->body = std::make_shared<sfc::BodyV1>();
		*static_cast<sfc::EntityV1*>(crea->body.get()) = *sfc_dump_entity(*body);
		auto gallery = static_cast<sfc::ObjectV1*>(crea)->gallery = crea->body->gallery;
		crea->body->body_data = body->body_data;

		// not implemented
		crea->body->angle = body->angle;
		crea->body->view = body->view;

		crea->head = sfc_dump_limb(head.get(), gallery);
		crea->left_thigh = sfc_dump_limb(left_thigh.get(), gallery);
		crea->right_thigh = sfc_dump_limb(right_thigh.get(), gallery);
		crea->left_arm = sfc_dump_limb(left_arm.get(), gallery);
		crea->right_arm = sfc_dump_limb(right_arm.get(), gallery);
		crea->tail = sfc_dump_limb(tail.get(), gallery);

		// sorta implemented
		crea->direction = direction;
		crea->downfoot_right = downfoot_right;
		crea->footx = footx;
		crea->footy = footy;

		// its own plane, not the hand's or a vehicle's (as LibreCreatures saves it)
		crea->z_order = saved_z_order;
		// the pose as reached, in digits, as the original saves it (LibreCreatures); the pose
		// asked for can hold '?', '!' and 'X', which the original would step through on loading
		{
			std::string reached(15, 'X');
			reached[0] = static_cast<char>('0' + std::clamp<int>(pose_values[0], 0, 3));
			for (size_t i = 1; i < reached.size() && i < pose_values.size(); ++i) {
				if (pose_part(i)) {
					reached[i] = static_cast<char>('0' + std::clamp<int>(pose_values[i], 0, 9));
				}
			}
			crea->current_pose = reached;
		}
		// the original only knows 0-2: the angry face (3, LibreCreatures row 88) is saved as sad
		crea->expression = std::min<uint8_t>(expression, 2);
		crea->eyes_open = eyes_open;
		crea->asleep = asleep;

		// sorta implemented
		crea->poses = poses;
		crea->gait_animations = gait_animations;

		// not implemented
		crea->vocabulary = vocabulary;
		for (size_t c = 0; c < attention_memory.size(); ++c) {
			const auto& e = attention_memory[c];
			crea->object_positions[c] = e.has_position ? sfc::Vector2i{numeric_cast<int32_t>(e.last_x), numeric_cast<int32_t>(e.last_y)} : sfc::Vector2i{-1, -1};
			for (size_t d = 0; d < 16; ++d) {
				crea->goals[c][d] = learned_chemicals[c][d];
			}
		}
		crea->stimuli = stimuli;
		crea->brain = brain;
		crea->biochemistry = biochemistry;
		crea->biochemistry->owner = crea; // danger!!!
		crea->sex = sex;
		crea->age = age;

		// sorta implemented
		crea->biotick = biotick;

		// not implemented
		crea->gamete = gamete;
		crea->zygote = zygote;
		crea->dead = dead;

		// sorta implemented
		crea->age_ticks = age_ticks;

		// not implemented
		crea->dreaming = dreaming;
		crea->instincts = instincts;

		crea->zzzz = std::dynamic_pointer_cast<sfc::SimpleObjectV1>(ctx.dump_object(g_engine_context.objects->try_get(zzzz)));
		crea->voices_lookup = voices_lookup;
		crea->voices = voices;
		crea->history_moniker = register_moniker_text(history_moniker);
		crea->history_name = history_name;
		crea->history_moms_moniker = register_moniker_text(history_moms_moniker);
		crea->history_dads_moniker = register_moniker_text(history_dads_moniker);
		crea->history_birthday = history_birthday;
		crea->history_birthplace = history_birthplace;
		crea->history_owner_name = history_owner_name;
		crea->history_owner_phone = history_owner_phone;
		crea->history_owner_address = history_owner_address;
		crea->history_owner_email = history_owner_email;
	}

	Object::serialize(ctx, crea);
	if (ctx.is_loading()) {
		initialize_vocabulary();
	}
	if (ctx.is_loading() && std::all_of(voices.begin(), voices.end(), [](auto& v) { return v.delay_ticks == 0; })) {
		// creatures saved without a voice get the one for their genus and sex
		load_voice();
	}
}
