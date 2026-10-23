#include "EngineContext.h"
#include "KitPreferences.h"
#include "Registration.h"
#include "CobInjector.h"
#include "PersistentFile.h"
#include "Macro.h"
#include "MacroManager.h"
#include "PathManager.h"
#include "PointerManager.h"
#include "Scriptorium.h"
#include "WorldCorrections.h"
#include "WorldSystems.h"
#include "WorldState.h"
#include "objects/Creature.h"
#include "objects/CompoundObject.h"
#include "objects/ObjectManager.h"
#include "objects/CallButton.h"
#include "objects/Lift.h"
#include "objects/PointerTool.h"
#include "objects/SimpleObject.h"
#include "fileformats/sfc/CBrain.h"
#include "../../../third_party/librecreatures/C1WorldScriptFixes.h"
#include "../../../third_party/librecreatures/C1CobAnalysis.h"
#include "fileformats/sfc/Entity.h"
#include "fileformats/sfc/SFCFile.h"
#include <optional>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
class EngineWorldBatch : public testing::Test {
protected:
    fs::path dir;
    void SetUp() override {
        g_engine_context.reset();
        dir = fs::temp_directory_path() / ("openc1-engine-world-batch-" + std::string(testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(dir);
        fs::create_directories(dir);
        g_engine_context.paths->set_main_directory(dir);
        set_world_file_name("Eden.sfc");
    }
    void TearDown() override { g_engine_context.reset(); fs::remove_all(dir); }
    void run(const std::string& text, ObjectHandle target = {}) {
        Macro m(text); m.ownr = m.targ = target; m.inst = true;
        g_engine_context.macros->run_now(m, false);
    }
    void cob(const std::string& file, unsigned quantity, const std::vector<std::string>& objects, const std::vector<std::string>& installs) {
        std::string data;
        auto u16 = [&](unsigned v) { data += char(v); data += char(v >> 8); };
        auto u32 = [&](unsigned v) { u16(v); u16(v >> 16); };
        auto text = [&](const std::string& s) { data += char(s.size()); data += s; };
        u16(1); u16(quantity); u32(0); u32(0); u32(0);
        u16(objects.size()); u16(installs.size()); u32(0);
        for (const auto& s : objects) text(s);
        for (const auto& s : installs) text(s);
        u32(0); u32(0); u16(0); text("Regression package");
        write_persistent_file(dir / file, data);
    }
};

TEST_F(EngineWorldBatch, ConditionalScanningIgnoresBracketedKeywords) {
    auto owner = g_engine_context.objects->add<SimpleObject>();
    run("doif 0 eq 1,dbug [else,endi,doif],else,setv obv0 17,endi,endm", owner);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 17);
    run("doif 1 eq 1,setv obv0 23,else,dbug [endi,doif],endi,setv obv1 42,endm", owner);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 23);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv1, 42);
}
TEST_F(EngineWorldBatch, EmptyEnumerationSkipsNestedBlocksAndLiteralNext) {
    auto owner = g_engine_context.objects->add<SimpleObject>();
    run("enum 255 255 255,dbug [next],enum 1 1 1,setv obv0 99,next,next,setv obv1 7,endm", owner);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 0);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv1, 7);
}
TEST_F(EngineWorldBatch, SubroutineLabelsComeFromSyntax) {
    auto owner = g_engine_context.objects->add<SimpleObject>();
    run("gsub good,stop,dbug [subr good,setv obv0 99],subr good,setv obv0 42,retn,endm", owner);
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 42);
}
TEST_F(EngineWorldBatch, DeletedObjectClearsHandAttentionAndOnlyItsSelection) {
    const auto attended = g_engine_context.objects->add<SimpleObject>();
    const auto selected = g_engine_context.objects->add<Creature>();
    const auto unselected = g_engine_context.objects->add<Creature>();
    auto* c = g_engine_context.objects->try_get(selected)->as_creature();
    c->attention = attended; c->has_target = true;
    c->attention_memory[3].object = attended; c->attention_memory[3].has_position = true;
    g_engine_context.world->current_norn = selected;
    g_engine_context.pointer->m_carried_object = attended;
    g_engine_context.pointer->m_edited_object = attended;
    g_engine_context.pointer->m_edit_free = true;
    g_engine_context.objects->destroy_later(attended);
    g_engine_context.objects->destroy_later(unselected);
    g_engine_context.objects->destroy_pending();
    EXPECT_EQ(g_engine_context.world->current_norn, selected);
    EXPECT_FALSE(c->attention); EXPECT_FALSE(c->has_target);
    EXPECT_FALSE(c->attention_memory[3].object); EXPECT_FALSE(c->attention_memory[3].has_position);
    EXPECT_FALSE(g_engine_context.pointer->m_carried_object);
    EXPECT_FALSE(g_engine_context.pointer->m_edited_object);
    EXPECT_FALSE(g_engine_context.pointer->m_edit_free);
    g_engine_context.objects->destroy_later(selected);
    g_engine_context.objects->destroy_pending();
    EXPECT_FALSE(g_engine_context.world->current_norn);
}
TEST_F(EngineWorldBatch, DeletingLiftOrButtonInvalidatesAssociations) {
    const auto lift_id = g_engine_context.objects->add<Lift>();
    const auto a = g_engine_context.objects->add<CallButton>();
    const auto b = g_engine_context.objects->add<CallButton>();
    auto* lift = g_engine_context.objects->try_get(lift_id)->as_lift();
    for (auto button : {a, b}) {
        g_engine_context.objects->try_get(button)->as_call_button()->lift = lift_id;
        lift->activated_call_buttons.insert(button);
    }
    g_engine_context.objects->destroy_later(a); g_engine_context.objects->destroy_pending();
    EXPECT_EQ(lift->activated_call_buttons.size(), 1u);
    g_engine_context.objects->destroy_later(lift_id); g_engine_context.objects->destroy_pending();
    EXPECT_FALSE(g_engine_context.objects->try_get(b)->as_call_button()->lift);
}
// The original scripts aren't kept in this project: these tests read them from a Creatures 1
// folder named by OPENC1_GAME_DATA (its Eden.sfc), and skip without one.
static std::optional<std::vector<std::string>> stock_scripts_from_game() {
    using namespace openc1::librecreatures;
    const char* folder = std::getenv("OPENC1_GAME_DATA");
    if (!folder) return std::nullopt;
    const auto eden = std::filesystem::path(folder) / "Eden.sfc";
    if (!std::filesystem::exists(eden)) return std::nullopt;
    const auto world = sfc::read_sfc_v1_file(eden);
    std::vector<std::string> stock;
    for (const auto& fix : kStockScriptFixes) {
        const sfc::ScriptV1* found = nullptr;
        for (const auto& script : world.scripts)
            if (script.family == fix.family && script.genus == fix.genus && script.species == fix.species && script.eventno == fix.event) found = &script;
        if (!found || found->text.size() != fix.stock_length || script_fingerprint(found->text) != fix.stock_hash) return std::nullopt;
        stock.push_back(found->text);
    }
    return stock;
}
TEST_F(EngineWorldBatch, StockScriptRepairsAreExactAndPreserveCustomization) {
    using namespace openc1::librecreatures;
    const auto stock = stock_scripts_from_game();
    if (!stock) GTEST_SKIP() << "Set OPENC1_GAME_DATA to a Creatures 1 folder with the shipped Eden.sfc";
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        const auto& fix = kStockScriptFixes[i];
        g_engine_context.scriptorium->add(fix.family, fix.genus, fix.species, fix.event, (*stock)[i]);
    }
    const auto drum = g_engine_context.objects->add<SimpleObject>();
    auto* d = g_engine_context.objects->try_get(drum);
    d->family = 2; d->genus = 9; d->species = 2; d->actv = ACTV_ACTIVE1;
    apply_world_corrections();
    for (const auto& fix : kStockScriptFixes) {
        const auto now = g_engine_context.scriptorium->get(fix.family, fix.genus, fix.species, fix.event);
        EXPECT_EQ(now.size(), fix.fixed_length);
        EXPECT_EQ(script_fingerprint(now), fix.fixed_hash);
    }
    EXPECT_EQ(d->actv, ACTV_INACTIVE);
    // a corrected world stays as it is
    apply_world_corrections();
    for (const auto& fix : kStockScriptFixes)
        EXPECT_EQ(script_fingerprint(g_engine_context.scriptorium->get(fix.family, fix.genus, fix.species, fix.event)), fix.fixed_hash);
    g_engine_context.scriptorium->remove(4, 0, 0, 21);
    g_engine_context.scriptorium->add(4, 0, 0, 21, "customized,endm");
    apply_world_corrections();
    EXPECT_EQ(g_engine_context.scriptorium->get(4, 0, 0, 21), "customized,endm");
}
TEST_F(EngineWorldBatch, CustomizedBoatRetainsAllItsScripts) {
    using namespace openc1::librecreatures;
    const auto stock = stock_scripts_from_game();
    if (!stock) GTEST_SKIP() << "Set OPENC1_GAME_DATA to a Creatures 1 folder with the shipped Eden.sfc";
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        const auto& fix = kStockScriptFixes[i];
        if (fix.family == 3) g_engine_context.scriptorium->add(3, 1, 7, fix.event, fix.event == 9 ? "custom,endm" : (*stock)[i]);
    }
    apply_world_corrections();
    for (size_t i = 0; i < std::size(kStockScriptFixes); ++i) {
        const auto& fix = kStockScriptFixes[i];
        if (fix.family == 3) EXPECT_EQ(g_engine_context.scriptorium->get(3, 1, 7, fix.event), fix.event == 9 ? "custom,endm" : (*stock)[i]);
    }
}
TEST_F(EngineWorldBatch, PreferencesPersistImmediatelyAndDoNotLeakAcrossProfiles) {
    KitPreferences prefs;
    prefs.load(dir / "kits.txt");
    prefs.set("observe.pregnant", 0); prefs.set("page.Observe", 7); prefs.set("top.Score", 1);
    KitPreferences reopened; reopened.load(dir / "kits.txt");
    EXPECT_EQ(reopened.get("observe.pregnant", 1), 0);
    EXPECT_EQ(reopened.get("page.Observe", 0), 7);
    EXPECT_EQ(reopened.get("top.Score", 0), 1);
    reopened.load(dir / "other-profile.txt");
    EXPECT_EQ(reopened.get("top.Score", 0), 0);
}
TEST_F(EngineWorldBatch, FailedCompanionWritePreservesPreviousContents) {
    write_persistent_file(dir / "record", "previous");
    // A nonempty directory at the staging path reliably fails, even when tests run as root.
    fs::create_directories(dir / "record.tmp");
    write_persistent_file(dir / "record.tmp" / "blocker", "x");
    EXPECT_THROW(write_persistent_file(dir / "record", "replacement"), std::exception);
    std::ifstream in(dir / "record"); std::string value; in >> value;
    EXPECT_EQ(value, "previous");
}
TEST_F(EngineWorldBatch, RegistrationSurvivesReloadAndIsScopedToWorldAndIdentity) {
    Creature c; c.moniker = "TEST"; c.history_name = "A name"; c.history_owner_address = "Line one\nLine two";
    save_registration(c);
    Creature loaded; loaded.moniker = "TEST"; load_registration(loaded);
    EXPECT_EQ(loaded.history_name, "A name"); EXPECT_EQ(loaded.history_owner_address, c.history_owner_address);
    set_world_file_name("Other.sfc"); loaded.history_name = "other"; load_registration(loaded);
    EXPECT_EQ(loaded.history_name, "other");
}
TEST_F(EngineWorldBatch, ExhaustedCobRejectedAndSuccessfulAllowanceSurvivesReload) {
    cob("one.cob", 1, {"scrp  2\t8 9 1 ,endm"}, {"endm"});
    EXPECT_EQ(cob_remaining("one.cob"), 1);
    inject_cob("one.cob"); EXPECT_EQ(cob_remaining("one.cob"), 0);
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 8, 9, 1), "endm");
    EXPECT_THROW(inject_cob("one.cob"), std::exception);
    g_engine_context.reset(); g_engine_context.paths->set_main_directory(dir);
    EXPECT_EQ(cob_remaining("one.cob"), 0);
    set_world_file_name("Other.sfc"); EXPECT_EQ(cob_remaining("one.cob"), 1);
}
TEST_F(EngineWorldBatch, BadCobHeadersAndRejectedInstallDoNotConsumeOrPartiallyInstall) {
    cob("badheader.cob", 2, {"scrp 2 8 9 1,endm", "scrp 999 1 1 1,endm"}, {});
    EXPECT_THROW(inject_cob("badheader.cob"), std::exception);
    EXPECT_EQ(cob_remaining("badheader.cob"), 2);
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 8, 9, 1), "");
    cob("badscript.cob", 2, {"scrp 2 8 9 1,endm"}, {"xxxx,endm"});
    EXPECT_THROW(inject_cob("badscript.cob"), std::exception);
    EXPECT_EQ(cob_remaining("badscript.cob"), 2);
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 8, 9, 1), "");
}
TEST_F(EngineWorldBatch, UnlimitedCobDoesNotBecomeFinite) {
    cob("unlimited.cob", 255, {}, {"endm"});
    for (int i = 0; i < 5; ++i) inject_cob("unlimited.cob");
    EXPECT_EQ(cob_remaining("unlimited.cob"), -1);
}
TEST_F(EngineWorldBatch, EmptyCompoundCanBeMovedBeforeItsFirstPart) {
    const auto id = g_engine_context.objects->add<CompoundObject>();
    EXPECT_NO_THROW(run("mvto 20 30,mvby 5 10,endm", id));
    EXPECT_TRUE(g_engine_context.objects->try_get(id)->as_compound_object()->parts.empty());
}
TEST_F(EngineWorldBatch, WalkingUsesOnlyAvailableGaitsIncludingZeroStrength) {
    Creature c;
    c.poses[0] = "000000000000000";
    c.gait_animations[6] = "00R";
    EXPECT_NO_THROW(c.walk());
    EXPECT_EQ(c.pose_animation, "00R");
    c.gait_animations[6].clear();
    EXPECT_NO_THROW(c.walk()); EXPECT_TRUE(c.pose_animation.empty());
    // the last gait is only for walking to something it attends to (as LibreCreatures has it)
    c.gait_animations[7] = "00R";
    EXPECT_NO_THROW(c.walk()); EXPECT_TRUE(c.pose_animation.empty());
    c.attention = g_engine_context.objects->add<SimpleObject>();
    EXPECT_NO_THROW(c.walk()); EXPECT_EQ(c.pose_animation, "00R");
}

#include "KitMonitoring.h"
#include "ImageManager.h"
#include "common/backend/Backend.h"
#include "common/backend/NullBackend.h"
#include "fileformats/sfc/CBiochemistry.h"

TEST_F(EngineWorldBatch, ChemicalHistoryRollsPastCapacityAndResetsOnSubjectChange) {
    Creature c; c.moniker = "ONE1"; c.biochemistry = std::make_shared<sfc::CBiochemistryV1>();
    ChemicalHistory graph;
    for (uint32_t tick = 0; tick < 5000; ++tick) {
        c.biochemistry->chemicals[255].concentration = static_cast<uint8_t>(tick);
        graph.sample(c, tick);
        graph.sample(c, tick); // rendering the paused frame must not duplicate a sample
    }
    EXPECT_EQ(graph.samples[255].size(), ChemicalHistory::capacity);
    EXPECT_EQ(graph.samples[255].front(), static_cast<uint8_t>(5000 - ChemicalHistory::capacity));
    EXPECT_EQ(graph.samples[255].back(), static_cast<uint8_t>(4999));
    c.moniker = "TWO2"; graph.sample(c, 0);
    EXPECT_EQ(graph.samples[255].size(), 1u);
    graph.sample(c, UINT32_MAX); graph.sample(c, 0);
    EXPECT_EQ(graph.samples[255].size(), 3u);
}
TEST_F(EngineWorldBatch, AlertsFollowIdentityAndCatchSkippedPregnancyStagesWithoutDeadWarnings) {
    ObservationHistory history; ObservationOptions options;
    EXPECT_TRUE(history.poll("ONE1", false, 100, 0, options).empty());
    EXPECT_TRUE(history.poll("TWO2", false, 100, 0, options).empty());
    const auto alerts = history.poll("TWO2", false, 100, 8, options);
    EXPECT_EQ(alerts, (std::vector<ObservationHistory::Alert>{ObservationHistory::PREGNANT, ObservationHistory::BIRTH}));
    EXPECT_TRUE(history.poll("ONE1", false, 100, 0, options).empty());
    EXPECT_TRUE(history.poll("TWO2", false, 100, 8, options).empty());
    EXPECT_TRUE(history.poll("DEAD", true, 0, 0, options).empty());
    EXPECT_EQ(history.poll("ONE1", false, 1, 0, options), (std::vector<ObservationHistory::Alert>{ObservationHistory::LOW_LIFE}));
    history.clear();
    EXPECT_EQ(history.poll("TWO2", false, 100, 8, options).size(), 2u);
}
TEST_F(EngineWorldBatch, SavedDisabledAlertsApplyBeforeFirstPoll) {
    ObservationOptions disabled; disabled.warn_life_force = disabled.warn_pregnant = disabled.warn_birth = false;
    ObservationHistory history;
    EXPECT_TRUE(history.poll("ONE1", false, 0, 8, disabled).empty());
    ObservationOptions enabled;
    EXPECT_TRUE(history.poll("ONE1", false, 0, 8, enabled).empty());
    history.poll("ONE1", false, 100, 0, enabled);
    EXPECT_EQ(history.poll("ONE1", false, 0, 8, enabled).size(), 3u);
}
TEST_F(EngineWorldBatch, HitTestingPrefersRealControlsAndEnclosedObjects) {
    const auto background = g_engine_context.objects->add<CompoundObject>();
    auto* compound = g_engine_context.objects->try_get(background)->as_compound_object();
    ImageGallery gallery; gallery.texture_locations = {Rect2i{0, 0, 100, 100}};
    compound->parts.resize(1); static_cast<DullPart&>(compound->parts[0]) = DullPart(gallery, 0, 100, 100, 9000);
    compound->attr = ATTR_ACTIVATEABLE;
    compound->functions_to_hotspots.fill(-1);
    compound->functions_to_hotspots[3] = 0; compound->hotspots[0] = Rect2i{20, 20, 10, 10};
    const auto enclosed = g_engine_context.objects->add<SimpleObject>();
    auto* small = g_engine_context.objects->try_get(enclosed)->as_simple_object();
    gallery.texture_locations[0] = Rect2i{0, 0, 30, 30};
    small->part = DullPart(gallery, 0, 110, 110, 100);
    small->attr = ATTR_ACTIVATEABLE;
    EXPECT_EQ(find_object_under_pointer(125, 125, ATTR_ACTIVATEABLE), compound);
    EXPECT_EQ(find_object_under_pointer(115, 115, ATTR_ACTIVATEABLE), small);
    EXPECT_EQ(find_object_under_pointer(180, 180, ATTR_ACTIVATEABLE), compound);
    compound->functions_to_hotspots[3] = 200; // malformed hotspot is not an array index
    EXPECT_NO_THROW(compound->handle_left_click(25, 25));
    EXPECT_EQ(find_object_under_pointer(125, 125, ATTR_ACTIVATEABLE), small);
}
TEST_F(EngineWorldBatch, CreationAndPlacementFinishBeforeTheSchedulerCanInterrupt) {
    if (!get_backend()) set_backend(NullBackend::get_instance());
    fs::create_directories(dir / "Images");
    // A generated 2x2 SPR fixture, with no game assets.
    const std::string sprite{"\x01\x00\x0a\x00\x00\x00\x02\x00\x02\x00\x01\x02\x03\x04", 14};
    write_persistent_file(dir / "Images/test.spr", sprite);
    const auto owner = g_engine_context.objects->add<SimpleObject>();
    Macro m("new: simp test 1 0 100 0,pose 0,setv clas 33620224,mvto 120 200,endm");
    m.ownr = m.targ = owner; // deliberately no INST prefix
    g_engine_context.macros->add(m);
    g_engine_context.macros->tick();
    for (auto* obj : *g_engine_context.objects) if (obj->uid != owner) {
        EXPECT_FALSE(obj->unplaced);
        EXPECT_EQ(obj->get_bbox().x, 120);
        EXPECT_EQ(obj->get_bbox().y, 200);
    }
    g_engine_context.macros->delete_macros_owned_by(owner);
    g_engine_context.macros->tick();
    for (auto* obj : *g_engine_context.objects) if (obj->uid != owner) EXPECT_EQ(obj->get_bbox().y, 200);
}
TEST_F(EngineWorldBatch, MutationKeepsDiseaseChemicalsAndClampsThresholds) {
    WorldState::Bacterium b;
    b.antigen = 248; b.fatal_level = 255; b.infect_level = 0; b.toxins.fill(232);
    for (int i = 0; i < 10000; ++i) {
        b = mutate_bacterium(b);
        EXPECT_GE(b.fatal_level, 130); EXPECT_LE(b.infect_level, 130);
        EXPECT_GE(b.antigen, 248);
        for (auto chemical : b.toxins) { EXPECT_GE(chemical, 232); EXPECT_LE(chemical, 235); }
    }
}
TEST_F(EngineWorldBatch, RemoverRunsEvenWhenItsPackageQuantityIsZero) {
    cob("remove.rcb", 0, {}, {"endm"});
    EXPECT_EQ(cob_remaining("remove.rcb"), -1);
    EXPECT_NO_THROW(inject_cob("remove.rcb"));
}
TEST_F(EngineWorldBatch, FrameAtCapacityHasNoSizeOrDrawableFrame) {
    ImageGallery gallery; gallery.texture_locations.resize(8, Rect2i{0, 0, 10, 20});
    DullPart object(gallery, 0, 0, 0, 0);
    object.set_animation(0, "8");
    EXPECT_EQ(object.frame(), 8); EXPECT_FALSE(object.gallery().has_frame(object.frame()));
    EXPECT_EQ(object.width(), 0); EXPECT_EQ(object.height(), 0);
}

#include "common/io/FileReader.h"
#include "fileformats/sprImage.h"
TEST_F(EngineWorldBatch, GeneratedSpriteOffsetsPointToPixelsWithoutLeadingGap) {
    if (!get_backend()) set_backend(NullBackend::get_instance());
    fs::create_directories(dir / "Images");
    const std::string sprite{"\x01\x00\x0a\x00\x00\x00\x02\x00\x02\x00\x01\x02\x03\x04", 14};
    write_persistent_file(dir / "Images/part.spr", sprite);
    g_engine_context.images->add_joined_spr_image("joined", {"part", "part"}, "joined");
    EXPECT_EQ(fs::file_size(dir / "Images/joined.spr"), 2u + 2u * 8u + 8u);
    FileReader input(dir / "Images/joined.spr");
    auto images = ReadSprFile(input);
    ASSERT_EQ(images.size(), 2u);
    for (const auto& image : images) {
        EXPECT_EQ(image.data[0], 1); EXPECT_EQ(image.data[3], 4);
    }
}

#include "Graveyard.h"
TEST_F(EngineWorldBatch, GraveDatesAlbumsAndInscriptionsRoundTripAcrossYears) {
    GraveRecord g{"TEST", "Norn | name", "Female", "100:00:00", "A \"quoted\" | inscription\nsecond line", 27};
    g.born = "31 December 2025, 23:59"; g.died = "01 January 2026, 00:01";
    save_graveyard(dir / "graveyard.txt", {g});
    const auto loaded = load_graveyard(dir / "graveyard.txt");
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].name, g.name); EXPECT_EQ(loaded[0].headstone, g.headstone);
    EXPECT_EQ(loaded[0].photo, 27); EXPECT_EQ(loaded[0].born, g.born); EXPECT_EQ(loaded[0].died, g.died);
    write_persistent_file(dir / "legacy.txt", "TEST|Legacy|Male|2:00:00|Remembered|7|Parents|31 December 2025\n");
    const auto legacy = load_graveyard(dir / "legacy.txt");
    ASSERT_EQ(legacy.size(), 1u); EXPECT_EQ(legacy[0].photo, 7); EXPECT_EQ(legacy[0].name, "Legacy");
}

TEST_F(EngineWorldBatch, RaisedHeadstonesRoundTripAndOlderFilesInferThem) {
    GraveRecord raised{"RAIS", "Raised", "Male", "1:00:00", "", -1};
    raised.stone = true; // raised with no epitaph
    GraveRecord unraised{"UNRA", "Unraised", "Female", "1:00:00", "Written but not raised", -1};
    save_graveyard(dir / "stones.txt", {raised, unraised});
    const auto loaded = load_graveyard(dir / "stones.txt");
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_TRUE(loaded[0].stone); EXPECT_FALSE(loaded[1].stone);
    // before the flag, an inscription meant a stone
    write_persistent_file(dir / "v2.txt", "OPENC1-GRAVES 2\n\"A\" \"A\" \"Male\" \"1:00\" \"Rest\" -1 \"\" \"\" \"\"\n\"B\" \"B\" \"Male\" \"1:00\" \"\" -1 \"\" \"\" \"\"\n");
    const auto v2 = load_graveyard(dir / "v2.txt");
    ASSERT_EQ(v2.size(), 2u);
    EXPECT_TRUE(v2[0].stone); EXPECT_FALSE(v2[1].stone);
}

TEST_F(EngineWorldBatch, CobAnalysisReadsChemicalsWarningsAndRemoval) {
    namespace lc = openc1::librecreatures;
    const std::vector<std::string> objects{"scrp 2 10 12 1,stim writ from 0 255 0 0 34 10 12 20,endm", "scrp 2 10 12 9,chem 17 30,kill norn,endm"};
    const std::vector<std::string> installs{"inst,new: simp 2 10 12 1 0,setv clas 34212864,endm"};
    EXPECT_EQ(lc::affected_chemicals(objects, installs), (std::vector<int>{34, 12, 17}));
    const auto warnings = lc::cob_warnings(objects, installs);
    EXPECT_TRUE(warnings.kills_creature); EXPECT_FALSE(warnings.fires_neurons); EXPECT_FALSE(warnings.makes_nothing);
    const auto made = lc::created_classifiers(installs);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].family, 2); EXPECT_EQ(made[0].genus, 10); EXPECT_EQ(made[0].species, 12);
    EXPECT_EQ(lc::generated_removal(objects, installs), "inst,enum 2 10 12,kill targ,next,scrx 2 10 12 1,scrx 2 10 12 9,endm");
    EXPECT_TRUE(lc::cob_warnings(objects, {}).makes_nothing);
}

TEST_F(EngineWorldBatch, RemovingACobWithoutRemoverTakesOutItsScripts) {
    cob("plain.cob", 255, {"scrp 2 21 3 1,endm", "scrp 2 21 3 9,endm"}, {});
    inject_cob("plain.cob");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 21, 3, 9), "endm");
    remove_cob("plain.cob");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 21, 3, 1), "");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 21, 3, 9), "");
    // with an .rcb, that does the removing
    cob("kept.cob", 255, {"scrp 2 21 4 1,endm"}, {});
    cob("kept.rcb", 0, {}, {"scrx 2 21 4 1,endm"});
    inject_cob("kept.cob");
    remove_cob("kept.cob");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 21, 4, 1), "");
}

TEST_F(EngineWorldBatch, RemovalCleansMatchingHistoricalWhitespaceMisinstallationOnly) {
    cob("legacy.cob", 1, {"scrp 2 13 10  5,endm"}, {});
    cob("legacy.rcb", 0, {}, {"scrx 2 13 10  5,endm"});
    // The original parser installed the double-space event 5 under event 101.
    g_engine_context.scriptorium->add(2, 13, 10, 101, "endm");
    g_engine_context.scriptorium->add(2, 13, 10, 5, "endm");
    inject_cob("legacy.rcb");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 13, 10, 101), "");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 13, 10, 5), "");
    g_engine_context.scriptorium->add(2, 13, 10, 101, "custom,endm");
    inject_cob("legacy.rcb");
    EXPECT_EQ(g_engine_context.scriptorium->get(2, 13, 10, 101), "custom,endm");
}

TEST_F(EngineWorldBatch, ScoreSaturationDoesNotWrapAtLargeSupportedCounts) {
    g_engine_context.world->natural_eggs = 1u << 24;
    EXPECT_EQ(player_score(), 99999u);
    g_engine_context.world->natural_eggs = UINT32_MAX;
    g_engine_context.world->breeders_score = UINT32_MAX;
    EXPECT_EQ(player_score(), 99999u);
}

#include "C1SoundManager.h"
#include "common/audio/NullAudioBackend.h"
TEST_F(EngineWorldBatch, LoopSurvivesOtherClipsAndRestoresItsVolumeAfterMute) {
    struct Audio : NullAudioBackend {
        uint32_t next = 0;
        std::map<uint32_t, float> volumes;
        AudioChannel play_clip(const fs::path&, bool) override { return {++next}; }
        AudioState audio_channel_get_state(AudioChannel) override { return AUDIO_PLAYING; }
        void audio_channel_set_volume(AudioChannel c, float v) override { volumes[c.handle] = v; }
    };
    fs::create_directories(dir / "Sounds"); write_persistent_file(dir / "Sounds/loop.wav", "fixture");
    // AudioBackend deliberately permits installation only once; isolate the fake
    // in a subprocess so the rest of the engine suite retains its real backend.
    EXPECT_EXIT({
        Audio audio;
        set_audio_backend(&audio);
        auto loop = g_engine_context.sounds->play_controlled_sound("loop", {}, true);
        loop.set_volume(0.6f);
        for (int n = 0; n < 1000; ++n) g_engine_context.sounds->play_uncontrolled_sound("loop", {});
        bool ok = loop.get_looping() && loop.get_state() == AUDIO_PLAYING;
        g_engine_context.sounds->set_muted(true); ok &= audio.volumes[1] == 0;
        g_engine_context.sounds->set_muted(false); ok &= audio.volumes[1] == 0.6f;
        std::_Exit(ok ? 0 : 1);
    }, testing::ExitedWithCode(0), "");
}
TEST_F(EngineWorldBatch, OrdinaryPerceptionIgnoresUnplacedAndOffWorldObjects) {
    Creature listener; listener.family = 4; listener.genus = 1; listener.species = 1;
    listener.footx = 100; listener.footy = 250; listener.direction = 2;
    listener.limit = Rect2i{0, 0, 1000, 1000};
    const auto id = g_engine_context.objects->add<SimpleObject>();
    auto* source = g_engine_context.objects->try_get(id)->as_simple_object();
    ImageGallery gallery; gallery.texture_locations = {Rect2i{0, 0, 20, 20}};
    source->part = DullPart(gallery, 0, 120, 220, 0);
    EXPECT_TRUE(listener.can_see(*source)); EXPECT_TRUE(listener.can_hear(*source));
    source->unplaced = true;
    EXPECT_FALSE(listener.can_see(*source)); EXPECT_FALSE(listener.can_hear(*source));
    source->unplaced = false; listener.footy = 1500;
    EXPECT_FALSE(listener.can_see(*source)); EXPECT_FALSE(listener.can_hear(*source));
    source->family = 2; source->genus = 25; EXPECT_EQ(Creature::attention_category(*source), -1);
    source->family = 3; source->genus = 10; EXPECT_EQ(Creature::attention_category(*source), -1);
}

#include "objects/Vehicle.h"
TEST_F(EngineWorldBatch, VehicleMotionContinuesFromScriptedPositionThroughRepeatedWraps) {
    const auto id = g_engine_context.objects->add<Vehicle>();
    auto* vehicle = g_engine_context.objects->try_get(id)->as_vehicle();
    ImageGallery gallery; gallery.texture_locations.resize(12, Rect2i{0, 0, 20, 20});
    vehicle->parts.resize(2);
    for (size_t i = 0; i < 2; ++i) {
        static_cast<DullPart&>(vehicle->parts[i]) = DullPart(gallery, 0, 0, 100, 100);
        vehicle->parts[i].relx = static_cast<int32_t>(i * 10);
    }
    vehicle->xvel = 2; vehicle->yvel = 0;
    run("mvto 600 100,endm", id);
    vehicle->tick(); EXPECT_EQ(vehicle->get_bbox().x, 602);
    for (int n = 0; n < 100; ++n) {
        vehicle->add_position(8352 + 17, 0);
        EXPECT_EQ(vehicle->get_bbox().x, (602 + 17 * (n + 1)) % 8352);
        EXPECT_EQ(vehicle->parts[1].x(), (612 + 17 * (n + 1)) % 8352);
    }
    vehicle->parts[0].set_animation(0, "99a");
    SimpleObject stationary{}; stationary.part = DullPart(gallery, 0, 0, 0, 0); stationary.part.set_animation(0, "99a");
    for (int n = 0; n < 4; ++n) { vehicle->tick(); stationary.tick(); EXPECT_EQ(vehicle->parts[0].pose(), stationary.part.pose()); }
}

TEST_F(EngineWorldBatch, FuneralSnapshotSurvivesImmediateBodyDeletion) {
    const auto id = g_engine_context.objects->add<Creature>();
    auto* c = g_engine_context.objects->try_get(id)->as_creature();
    c->moniker = "DEAD"; c->history_name = "Remember me"; c->history_birthday = "31 December 2025";
    c->sex = 2;
    c->die();
    g_engine_context.objects->destroy_later(id); g_engine_context.objects->destroy_pending();
    ASSERT_EQ(g_engine_context.world->pending_graves.size(), 1u);
    EXPECT_EQ(g_engine_context.world->pending_graves[0].name, "Remember me");
    EXPECT_EQ(g_engine_context.world->pending_graves[0].born, "31 December 2025");
    EXPECT_FALSE(g_engine_context.world->pending_graves[0].died.empty());
}

#include "common/io/SpanReader.h"
#include "fileformats/genomeFile.h"
TEST_F(EngineWorldBatch, TruncatedGenesCannotConsumeBytesBeyondDeclaredC1OrC2Genome) {
    for (int version : {1, 2}) {
        std::vector<uint8_t> bytes{'d', 'n', 'a', static_cast<uint8_t>('0' + version), 'g', 'e', 'n', 'e', 2, 1, 0, 0, 0, 0};
        if (version == 2) bytes.push_back(0); // C2 mutation-weight byte
        bytes.insert(bytes.end(), {0, 'g', 'e', 'n', 'd'}); // incomplete genus payload
        const auto declared = bytes.size();
        bytes.insert(bytes.end(), 64, 42); // unrelated memory following the declared record
        SpanReader reader(std::span<const uint8_t>(bytes.data(), declared));
        genomeFile genome;
        EXPECT_THROW(reader >> genome, std::exception);
        EXPECT_TRUE(genome.genes.empty());
        EXPECT_LE(reader.tell(), declared);
    }
}

#include "MessageManager.h"
#include "MacroContext.h"
TEST_F(EngineWorldBatch, InvalidTargetsAndTargetlessCommandsDoNotBlockOtherScripts) {
    const auto survivor = g_engine_context.objects->add<SimpleObject>();
    const auto missing = g_engine_context.objects->add<SimpleObject>();
    g_engine_context.objects->destroy_later(missing); g_engine_context.objects->destroy_pending();
    g_engine_context.messages->mesg_writ({}, missing, MESSAGE_ACTIVATE1);
    EXPECT_NO_THROW(g_engine_context.messages->tick());
    Macro bad("setv obv0 99,endm"); bad.targ = missing;
    EXPECT_THROW(g_engine_context.macros->run_now(bad, false), std::exception);
    Macro targetless("setv var0 42,wait 1,endm");
    EXPECT_NO_THROW(g_engine_context.macros->run_now(targetless, false));
    run("setv obv0 17,endm", survivor);
    EXPECT_EQ(g_engine_context.objects->try_get(survivor)->obv0, 17);
}
TEST_F(EngineWorldBatch, EnumerationVisitsEachOriginalOnceWhileDeletingIt) {
    const auto owner = g_engine_context.objects->add<SimpleObject>();
    for (int n = 0; n < 30; ++n) {
        const auto id = g_engine_context.objects->add<SimpleObject>();
        auto* obj = g_engine_context.objects->try_get(id); obj->family = 2; obj->genus = 3; obj->species = 4;
    }
    Macro script("enum 2 3 4,addv var0 1,kill targ,next,setv obv0 var0,endm"); script.inst = true; script.ownr = script.targ = owner;
    g_engine_context.macros->add(script); g_engine_context.macros->tick();
    EXPECT_EQ(g_engine_context.objects->try_get(owner)->obv0, 30);
    EXPECT_EQ(g_engine_context.objects->count_classifier(2, 3, 4), 30); // deferred erasure
    g_engine_context.objects->destroy_pending();
    EXPECT_EQ(g_engine_context.objects->count_classifier(2, 3, 4), 0);
    EXPECT_NE(g_engine_context.objects->try_get(owner), nullptr);
}

TEST_F(EngineWorldBatch, GestationStatusHasDefinedZeroAndCompleteChemicalRange) {
    Creature c{}; c.sex = 2; c.zygote = "BABY"; c.biochemistry = std::make_shared<sfc::CBiochemistryV1>();
    EXPECT_EQ(creature_gestation_stage(c), 0);
    for (int value = 1; value <= 255; ++value) {
        c.biochemistry->chemicals[66].concentration = static_cast<uint8_t>(value);
        EXPECT_EQ(creature_gestation_stage(c), (value + 31) / 32);
    }
    c.zygote.clear(); EXPECT_EQ(creature_gestation_stage(c), 0);
}
TEST_F(EngineWorldBatch, ScannerReturnsRequestedMeasureAndConnectionWithSafeMissingValues) {
    sfc::NeuronV1 neuron{}; neuron.output = 11; neuron.state = 22;
    neuron.dendrites0.resize(2); neuron.dendrites1.resize(1);
    auto& d = neuron.dendrites0[1]; d.suscept = 33; d.stw = 44; d.ltw = 55; d.strength = 66;
    neuron.dendrites1[0].suscept = 77;
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::OUTPUT), 11);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::STATE), 22);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::SUSCEPTIBILITY, 0, 1), 33);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::SHORT_WEIGHT, 0, 1), 44);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::LONG_WEIGHT, 0, 1), 55);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::STRENGTH, 0, 1), 66);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::SUSCEPTIBILITY, 1, 0), 77);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::SUSCEPTIBILITY, 0, 2), 0);
    EXPECT_EQ(monitor_neuron(neuron, BrainMeasure::SUSCEPTIBILITY, 2, 0), 0);
}
TEST_F(EngineWorldBatch, TactileStimulusReachesOnlyTouchingCreature) {
    ImageGallery gallery; gallery.texture_locations = {Rect2i{0, 0, 20, 40}};
    std::vector<Creature*> creatures;
    for (int x : {130, 800}) {
        const auto id = g_engine_context.objects->add<Creature>(); auto* c = g_engine_context.objects->try_get(id)->as_creature();
        c->family = 4; c->genus = 1; c->footx = x; c->footy = 250;
        c->body = std::make_unique<Body>(); static_cast<DullPart&>(*c->body) = DullPart(gallery, 0, static_cast<float>(x), 210, 0);
        creatures.push_back(c);
    }
    const auto id = g_engine_context.objects->add<SimpleObject>(); auto* source = g_engine_context.objects->try_get(id)->as_simple_object();
    source->part = DullPart(gallery, 0, 125, 205, 0);
    sfc::StimulusV1 stimulus{}; source->stimulate(Object::STIM_TACT, nullptr, stimulus);
    EXPECT_EQ(creatures[0]->queued_stimuli.size(), 1u); EXPECT_TRUE(creatures[1]->queued_stimuli.empty());
}

TEST_F(EngineWorldBatch, SaveNamesRejectTemplatesAndPathsWithoutChangingActiveWorld) {
    EXPECT_EQ(world_save_name("  My World  "), "My World.sfc");
    EXPECT_EQ(world_save_name("My World.SFC"), "My World.SFC");
    for (const std::string& name : {"Eden", "WORLD.sfc", "../Other", "a/b", "a\\b", ".hidden", "a\nb", ""})
        EXPECT_THROW(world_save_name(name), std::exception) << name;
    EXPECT_EQ(get_world_file_name(), "Eden.sfc");
}

#include "CreatureGenetics.h"
#include "common/io/VectorWriter.h"
TEST_F(EngineWorldBatch, PigmentExtensionRoundTripsAndSurvivesInheritance) {
    fs::create_directories(dir / "Genetics");
    std::vector<uint8_t> record{'g','e','n','e',2,6,0,0,0,1,1,128,'g','e','x','t',77,199};
    save_genome_bytes("pExt", {record});
    auto genome = load_genome("pExt");
    ASSERT_NE(genome, nullptr);
    ASSERT_EQ(genome->genes.size(), 1u);
    auto* pigment = dynamic_cast<creaturePigmentGene*>(genome->genes[0].get());
    ASSERT_NE(pigment, nullptr);
    EXPECT_TRUE(pigment->has_extension);
    EXPECT_EQ(pigment->rotation, 77);
    EXPECT_EQ(pigment->swap, 199);
    VectorWriter out; out << *genome;
    SpanReader in(out.vector()); genomeFile restored; in >> restored;
    auto* again = dynamic_cast<creaturePigmentGene*>(restored.genes[0].get());
    ASSERT_NE(again, nullptr);
    EXPECT_TRUE(again->has_extension);
    EXPECT_EQ(again->rotation, 77);
    for (int n = 0; n < 128; ++n) {
        const auto child = make_child_genome("pExt", "");
        const auto records = genome_gene_bytes(child);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_TRUE(std::equal(records[0].begin() + 12, records[0].begin() + 16, "gext"));
        auto next = load_genome(child);
        ASSERT_NE(next, nullptr);
        auto* p = dynamic_cast<creaturePigmentGene*>(next->genes[0].get());
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(p->has_extension);
    }
    record.pop_back(); save_genome_bytes("badE", {record});
    EXPECT_THROW(load_genome("badE"), std::exception);
}

TEST_F(EngineWorldBatch, BrainChemicalLociControlBothConnectionTypesAndRestState) {
    sfc::CBrainV1 b{};
    b.lobes.resize(1);
    b.lobes[0].num_neurons = 1;
    b.neurons.resize(1);
    CreatureLoci loci;
    uint8_t dead = 0;
    auto pointer = [&](bool receptor, uint8_t locus) { return creature_locus(loci, &b, dead, receptor, 0, 0, locus); };
    EXPECT_EQ(pointer(true, 2), &b.lobes[0].reststate);
    const std::array<uint8_t*, 10> expected{
        &b.lobes[0].d0_type.relax_suscept, &b.lobes[0].d0_type.relax_stw,
        &b.lobes[0].d0_type.ltw_gain_rate, &b.lobes[0].d0_type.gain_rate, &b.lobes[0].d0_type.lose_rate,
        &b.lobes[0].d1_type.relax_suscept, &b.lobes[0].d1_type.relax_stw,
        &b.lobes[0].d1_type.ltw_gain_rate, &b.lobes[0].d1_type.gain_rate, &b.lobes[0].d1_type.lose_rate};
    for (uint8_t i = 0; i < 10; ++i) EXPECT_EQ(pointer(true, i + 3), expected[i]);
    EXPECT_EQ(pointer(false, 1), &b.lobes[0].loose_dendrites[0]);
    EXPECT_EQ(pointer(false, 2), &b.lobes[0].loose_dendrites[1]);
    EXPECT_EQ(pointer(true, 33), nullptr);
    EXPECT_EQ(pointer(false, 19), nullptr);
}

TEST_F(EngineWorldBatch, LooseConceptConnectionsDriveTheirChemicalFeedbackEmitter) {
    sfc::CBrainV1 b{};
    b.lobes.resize(9);
    b.lobes[8].num_neurons = 2;
    b.neurons.resize(2);
    b.neurons[0].dendrites0.resize(2); // loose
    b.neurons[1].dendrites0.resize(1); // loose
    BrainMigrationState migration;
    brain_tick(b, 0, migration);
    EXPECT_EQ(b.lobes[8].loose_dendrites[0], 2);
    sfc::CBiochemistryV1 bio{};
    bio.chemicals[52].decay = 255;
    bio.emitters = {{0,8,1,52,0,1,10,2}};
    CreatureLoci loci;
    uint8_t dead = 0;
    creature_biochemistry_tick(bio, loci, &b, dead, 0);
    EXPECT_GT(bio.chemicals[52].concentration, 0);
}

#include "../../../third_party/librecreatures/C1Palette.h"
TEST(CreaturePalette, HueSwapGainAndReservedColoursMatchKnownColours) {
    std::array<Color, 256> palette{};
    for (auto& c : palette) { c.r = c.g = c.b = 252; c.a = 255; }
    palette[10] = {0, 0, 0, 255};
    palette[11] = {252, 0, 0, 255};
    palette[12] = {0, 252, 0, 255};
    palette[13] = {0, 0, 252, 255};
    palette[17] = {128, 0, 0, 255};
    palette[18] = {96, 0, 0, 255};
    palette[19] = {160, 0, 0, 255};
    using openc1::librecreatures::build_creature_palette_remap;
    EXPECT_THROW(build_creature_palette_remap(std::vector<Color>{}, 128, 128, 128, 128, 128), std::invalid_argument);
    EXPECT_EQ(build_creature_palette_remap(palette, 128, 128, 128, 128, 128)[11], 11);
    EXPECT_EQ(build_creature_palette_remap(palette, 128, 128, 128, 0, 128)[11], 12);
    EXPECT_EQ(build_creature_palette_remap(palette, 128, 128, 128, 255, 128)[11], 13);
    EXPECT_EQ(build_creature_palette_remap(palette, 128, 128, 128, 128, 0)[11], 13);
    EXPECT_EQ(build_creature_palette_remap(palette, 128, 128, 128, 128, 255)[11], 13);
    EXPECT_EQ(build_creature_palette_remap(palette, 0, 128, 128, 128, 128)[17], 18);
    EXPECT_EQ(build_creature_palette_remap(palette, 255, 128, 128, 128, 128)[17], 19);
    for (uint8_t control : {uint8_t(0), uint8_t(127), uint8_t(128), uint8_t(255)}) {
        const auto remap = build_creature_palette_remap(palette, control, control, control, control, control);
        for (size_t i = 0; i < 256; ++i) {
            if (i < 10 || i >= 246) EXPECT_EQ(remap[i], i);
            else { EXPECT_GE(remap[i], 10); EXPECT_LT(remap[i], 246); }
        }
    }
}

TEST_F(EngineWorldBatch, JoinedSpritesWriteRemappedPixelsWithoutChangingSourceArt) {
    if (!get_backend()) set_backend(NullBackend::get_instance());
    fs::create_directories(dir / "Images");
    const std::string sprite{"\x01\x00\x0a\x00\x00\x00\x02\x00\x02\x00\x00\x0b\x0c\xff", 14};
    write_persistent_file(dir / "Images/tint.spr", sprite);
    std::array<uint8_t, 256> remap{};
    for (size_t i = 0; i < remap.size(); ++i) remap[i] = static_cast<uint8_t>(i);
    remap[11] = 13; remap[12] = 14;
    g_engine_context.images->add_joined_spr_image("coloured", {"tint"}, "coloured", &remap);
    FileReader output(dir / "Images/coloured.spr");
    const auto images = ReadSprFile(output);
    ASSERT_EQ(images.size(), 1u);
    EXPECT_EQ(images[0].data[0], 0); EXPECT_EQ(images[0].data[1], 13);
    EXPECT_EQ(images[0].data[2], 14); EXPECT_EQ(images[0].data[3], 255);
    FileReader input(dir / "Images/tint.spr");
    const auto source = ReadSprFile(input);
    EXPECT_EQ(source[0].data[1], 11); EXPECT_EQ(source[0].data[2], 12);
}

#include "../../../third_party/librecreatures/C1GenomeEdits.h"
TEST_F(EngineWorldBatch, DesignerGenomeEditsChangeOnlyWhatTheyDescribe) {
    namespace lc = openc1::librecreatures;
    fs::create_directories(dir / "Genetics");
    using R = lc::GeneRecord;
    // a hunger raiser that punishes, a pain raiser that punishes, an old-age death receptor, a
    // receptor in the brain, starting glycogen and one antibody
    std::vector<R> genes{
        // reactions: amount and chemical for two reactants, then two products, then the rate
        lc::make_gene(1, 2, 0, 7, {1, 19, 0, 0, 1, 3, 1, 50, 10}), // Hunger Increase -> Hunger + Punishment
        lc::make_gene(1, 2, 1, 7, {1, 16, 0, 0, 1, 1, 1, 50, 10}), // Pain Increase -> Pain + Punishment
        lc::make_gene(1, 0, 2, 7, {1, 0, 6, 56, 0, 0, 0, 0}),
        lc::make_gene(1, 0, 3, 7, {0, 1, 2, 49, 0, 0, 0, 0}), // a receptor wired to the brain
        lc::make_gene(1, 4, 4, 7, {59, 122}),
        lc::make_gene(1, 4, 5, 7, {240, 130}),
    };
    lc::DesignerChoices all{true, true, true, true};
    const auto summary = lc::apply_designer_choices(genes, all);
    EXPECT_NE(summary.find("drive raisers no longer punishing: 1"), std::string::npos);
    EXPECT_NE(summary.find("old-age death receptors removed: 1"), std::string::npos);
    EXPECT_EQ(lc::payload(genes[0], 7), 0); EXPECT_EQ(lc::payload(genes[0], 6), 0); // hunger's punishment gone
    EXPECT_EQ(lc::payload(genes[1], 7), 50); // pain keeps its own
    ASSERT_EQ(genes.size(), 5u + 7u); // the receptor removed; seven antibodies added
    EXPECT_EQ(genes[2][9] & 7, 0); // the brain's receptor locked
    EXPECT_EQ(lc::payload(genes[3], 1), 160); // glycogen raised
    lc::EggColours colours; colours.tint[0] = 200; colours.hue_rotation = 40;
    lc::append_colour_genes(genes, colours);
    save_genome_bytes("dsgn", genes);
    auto genome = load_genome("dsgn");
    ASSERT_NE(genome, nullptr);
    int pigments = 0;
    for (auto& g : genome->genes) {
        if (auto* p = dynamic_cast<creaturePigmentGene*>(g.get())) {
            ++pigments;
            EXPECT_TRUE(p->has_extension); EXPECT_EQ(p->rotation, 40);
            if (p->color == 0) EXPECT_EQ(p->amount, 200);
        }
    }
    EXPECT_EQ(pigments, 3 * lc::COLOUR_GENES_PER_CHANNEL);
}

#include "../../../third_party/librecreatures/C1EggFile.h"
#include "EggFiles.h"
TEST_F(EngineWorldBatch, EggFilesHaveLibreCreaturesLayoutAndRejectAnythingElse) {
    using openc1::librecreatures::EggFile;
    EggFile egg;
    egg.moniker = "3FZK";
    egg.sex = 2;
    egg.genome = {'g', 'e', 'n', 'e', 1, 2, 3, 0, 0, 1, 9, 9, 'g', 'e', 'n', 'd'};
    const auto bytes = egg.bytes();
    ASSERT_EQ(bytes.size(), EggFile::HEADER_BYTES + egg.genome.size());
    // the fixed layout: class record, version 2, classifier 2 5 2, sex, size, CRC, CGenome
    EXPECT_EQ(std::string(bytes.begin() + 6, bytes.begin() + 10), "CEgg");
    EXPECT_EQ(bytes[10], 2); EXPECT_EQ(bytes[14 + 1], 2); EXPECT_EQ(bytes[14 + 2], 5); EXPECT_EQ(bytes[14 + 3], 2);
    EXPECT_EQ(bytes[18], 2); EXPECT_EQ(bytes[22], egg.genome.size());
    EXPECT_EQ(std::string(bytes.begin() + 36, bytes.begin() + 43), "CGenome");
    EXPECT_EQ(bytes[43], egg.genome.size());
    EXPECT_EQ(std::string(bytes.begin() + 47, bytes.begin() + 51), "3FZK");
    EXPECT_EQ(bytes[51], 2); EXPECT_EQ(bytes[55], 0);
    EggFile back;
    ASSERT_TRUE(back.read(bytes.data(), bytes.size()));
    EXPECT_EQ(back.moniker, "3FZK"); EXPECT_EQ(back.sex, 2u); EXPECT_EQ(back.genome, egg.genome);
    auto damaged = bytes;
    damaged.back() ^= 1; // the CRC no longer matches
    EXPECT_FALSE(EggFile().read(damaged.data(), damaged.size()));
    auto renamed = bytes;
    renamed[47] = '/'; // not a safe file name
    EXPECT_FALSE(EggFile().read(renamed.data(), renamed.size()));
    std::vector<uint8_t> video(200, 0x47);
    EXPECT_FALSE(EggFile().read(video.data(), video.size()));
    auto truncated = bytes;
    truncated.pop_back();
    EXPECT_FALSE(EggFile().read(truncated.data(), truncated.size()));
}

TEST_F(EngineWorldBatch, NewEggsAreArchivedByGenerationAndImportsAreUsedUp) {
    fs::create_directories(dir / "Genetics");
    set_world_file_name("Test World.sfc");
    const std::vector<uint8_t> gene{'g','e','n','e',2,6,0,0,0,1,1,128};
    save_genome_bytes("mum1", {gene});
    save_genome_bytes("dad1", {gene});
    const auto child = make_child_genome("mum1", "dad1");
    ASSERT_FALSE(child.empty());
    const auto egg = g_engine_context.objects->add<SimpleObject>();
    auto* e = g_engine_context.objects->try_get(egg);
    e->family = 2; e->genus = 5; e->species = 2; e->obv0 = moniker_to_integer(child); e->obv1 = 1;
    g_engine_context.world->tick = 8;
    EggFiles::tick();
    const auto kept = EggFiles::archived(child);
    ASSERT_FALSE(kept.empty());
    EXPECT_EQ(kept.parent_path(), dir / "Worlds" / "Test World" / "Eggs" / "Generation 1");
    EXPECT_TRUE(fs::is_regular_file(dir / "Worlds" / "Test World" / "Eggs" / "eggs.txt"));
    // a child of that egg is generation 2
    const auto grandchild = make_child_genome(child, "dad1");
    const auto egg2 = g_engine_context.objects->add<SimpleObject>();
    auto* e2 = g_engine_context.objects->try_get(egg2);
    e2->family = 2; e2->genus = 5; e2->species = 2; e2->obv0 = moniker_to_integer(grandchild);
    g_engine_context.world->tick = 16;
    EggFiles::tick();
    EXPECT_EQ(EggFiles::archived(grandchild).parent_path().filename(), "Generation 2");
    // a copy saved anywhere imports as the same genes; its name is taken here, so a new one
    EggFiles::save_copy(child, 1, dir / "copy.egg");
    const auto imported = EggFiles::import_egg(dir / "copy.egg");
    EXPECT_NE(imported, child);
    EXPECT_FALSE(fs::exists(dir / "copy.egg")); // used up
    EXPECT_EQ(genome_gene_bytes(imported), genome_gene_bytes(child));
    // the world's own copies aren't used up
    EggFiles::import_egg(kept);
    EXPECT_TRUE(fs::exists(kept));
    write_persistent_file(dir / "not.egg", std::string("RIFF....AVI "));
    EXPECT_THROW(EggFiles::import_egg(dir / "not.egg"), std::exception);
    EXPECT_TRUE(fs::exists(dir / "not.egg")); // a file that isn't an egg is left alone
}

TEST_F(EngineWorldBatch, LooseWorldsMoveIntoTheirFoldersWithABackup) {
    write_persistent_file(dir / "Eden.sfc", std::string("template"));
    write_persistent_file(dir / "Brave.sfc", std::string("world"));
    write_persistent_file(dir / "Brave.autosave.1.sfc", std::string("auto"));
    write_persistent_file(dir / "Brave.graveyard.txt", std::string("graves"));
    write_persistent_file(dir / "Brave.family.txt", std::string("norn|1ABC|||1|0|Alma\n"));
    write_persistent_file(dir / "Brave.sfc.wolfling.txt", std::string("run"));
    write_persistent_file(dir / "Brave.sfc.Honey Jars.COB.allowance", std::string("3\n"));
    write_persistent_file(dir / "Brave.autosave.1.graveyard.txt", std::string("old graves"));
    write_persistent_file(dir / "Bravery.sfc", std::string("another world"));
    write_persistent_file(dir / "1ABC.openc1.photos", std::string("OC1P"));
    write_persistent_file(dir / "9ZZZ.openc1.photos", std::string("OC1P"));
    write_persistent_file(dir / "Alma.exp", std::string("exp"));
    write_persistent_file(dir / "Wolfling Snapshots" / "Brave.sfc" / "Run 1" / "hour 00001.sfc", std::string("snap"));
    write_persistent_file(dir / "Worlds" / "Brave" / "Brave.sfc", std::string("already here"));
    EXPECT_GT(organise_world_files(), 0);
    const auto brave = dir / "Worlds" / "Brave";
    EXPECT_TRUE(fs::exists(dir / "Eden.sfc")); // the original's stay
    EXPECT_TRUE(fs::exists(dir / "Brave.sfc")); // never replaces what's there
    EXPECT_TRUE(fs::exists(brave / "Autosaves" / "Brave.autosave.1.sfc"));
    EXPECT_TRUE(fs::exists(brave / "Autosaves" / "Brave.autosave.1.graveyard.txt"));
    for (const char* name : {"Brave.graveyard.txt", "Brave.family.txt", "Brave.sfc.wolfling.txt", "Brave.sfc.Honey Jars.COB.allowance"})
        EXPECT_TRUE(fs::exists(brave / name)) << name;
    EXPECT_TRUE(fs::exists(dir / "Worlds" / "Bravery" / "Bravery.sfc"));
    EXPECT_TRUE(fs::exists(brave / "Photos" / "1ABC.openc1.photos"));
    EXPECT_TRUE(fs::exists(dir / "Photo Albums" / "9ZZZ.openc1.photos"));
    EXPECT_TRUE(fs::exists(dir / "Norns" / "Alma.exp"));
    EXPECT_TRUE(fs::exists(brave / "Wolfling Snapshots" / "Run 1" / "hour 00001.sfc"));
    EXPECT_TRUE(fs::exists(dir / "Backup" / "Before reorganising" / "Brave.graveyard.txt"));
    EXPECT_TRUE(fs::exists(dir / "Backup" / "Before reorganising" / "Wolfling Snapshots" / "Brave.sfc" / "Run 1" / "hour 00001.sfc"));
    EXPECT_EQ(find_world("Bravery.sfc"), dir / "Worlds" / "Bravery" / "Bravery.sfc");
    EXPECT_EQ(find_world("Eden.sfc").filename(), "Eden.sfc");
    const auto worlds = list_world_files();
    EXPECT_NE(std::find(worlds.begin(), worlds.end(), "Brave.autosave.1.sfc"), worlds.end());
    EXPECT_EQ(organise_world_files(), 0); // nothing left to move
}

TEST_F(EngineWorldBatch, CrossoverKeepsFlagsAndOrderAndClonesAreExact) {
    fs::create_directories(dir / "Genetics");
    // 30 mutable genes (no duplicating or leaving out), numbered, with a little data each
    std::vector<std::vector<uint8_t>> genes;
    for (uint8_t id = 0; id < 30; ++id) {
        genes.push_back({'g', 'e', 'n', 'e', 1, 4, id, 0, 0, 1 | 8, static_cast<uint8_t>(id * 3), 200});
    }
    save_genome_bytes("momA", genes);
    save_genome_bytes("dadA", genes);
    for (int n = 0; n < 40; ++n) {
        const auto clone = make_child_genome("momA", "");
        EXPECT_EQ(genome_gene_bytes(clone), genes); // a straight copy, unmutated
        const auto child = genome_gene_bytes(make_child_genome("momA", "dadA"));
        ASSERT_EQ(child.size(), genes.size());
        for (size_t i = 0; i < genes.size(); ++i) {
            EXPECT_EQ(child[i][6], genes[i][6]); // in order, none doubled or left out
            EXPECT_EQ(child[i][9], genes[i][9]); // the flags byte never mutates
        }
    }
}
