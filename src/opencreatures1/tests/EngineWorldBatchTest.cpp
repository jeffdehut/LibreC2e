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
    c.gait_animations[7] = "00R";
    EXPECT_NO_THROW(c.walk());
    EXPECT_EQ(c.pose_animation, "00R");
    c.gait_animations[7].clear();
    EXPECT_NO_THROW(c.walk()); EXPECT_TRUE(c.pose_animation.empty());
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
