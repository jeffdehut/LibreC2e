#include "CreatureGenetics.h"
#include "EngineContext.h"
#include "ImageManager.h"
#include "PathManager.h"
#include "SFCSerialization.h"
#include "SaveAndShare.h"
#include "WorldState.h"
#include "common/Ascii.h"
#include "common/backend/Backend.h"
#include "common/backend/NullBackend.h"
#include "common/io/FileReader.h"
#include "common/io/FileWriteFailureTest.h"
#include "fileformats/sfc/Body.h"
#include "fileformats/sfc/CBiochemistry.h"
#include "fileformats/sfc/CBrain.h"
#include "fileformats/sfc/CGallery.h"
#include "fileformats/sfc/CGenome.h"
#include "fileformats/sfc/Creature.h"
#include "fileformats/sfc/EXPFile.h"
#include "objects/Creature.h"
#include "objects/ObjectManager.h"
#include "objects/SimpleObject.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
class ExportTest : public testing::Test {
protected:
	fs::path root;
	ObjectHandle handle;
	Creature* creature;
	void SetUp() override {
		g_engine_context.reset();
		root = fs::temp_directory_path() / ("openc1-export-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		fs::create_directories(root / "Genetics");
		g_engine_context.paths->set_main_directory(root);
		handle = g_engine_context.objects->add<Creature>();
		creature = g_engine_context.objects->try_get(handle)->as_creature();
		creature->moniker = "test";
		creature->body = std::make_unique<Body>();
		creature->biochemistry = std::make_shared<sfc::CBiochemistryV1>();
		creature->brain = std::make_shared<sfc::CBrainV1>();
		std::ofstream(root / "Genetics/test.gen", std::ios::binary) << "gene";
		g_engine_context.world->current_norn = handle;
	}
	void TearDown() override { g_engine_context.reset(); fs::remove_all(root); }
};
TEST_F(ExportTest, FailedOpenPreservesCreatureAndSelection) {
	EXPECT_THROW(export_creature(*creature, true, (root / "missing/export.exp").string()), std::exception);
	g_engine_context.objects->destroy_pending();
	EXPECT_NE(g_engine_context.objects->try_get(handle), nullptr);
	EXPECT_EQ(g_engine_context.world->current_norn, handle);
}
#ifndef _WIN32
TEST_F(ExportTest, FailedFlushPreservesCreatureAndSelection) {
	FileWriteFailure failure;
	if (!failure) GTEST_SKIP() << "Unable to set file-size limit";
	EXPECT_THROW(export_creature(*creature, true, (root / "failed.exp").string()), std::exception);
	g_engine_context.objects->destroy_pending();
	EXPECT_NE(g_engine_context.objects->try_get(handle), nullptr);
	EXPECT_EQ(g_engine_context.world->current_norn, handle);
}
#endif
TEST_F(ExportTest, SuccessfulClosedExportPrecedesRemoval) {
	auto file = root / "export.exp";
	export_creature(*creature, true, file.string());
	g_engine_context.objects->destroy_pending();
	EXPECT_EQ(g_engine_context.objects->try_get(handle), nullptr);
	auto exported = sfc::read_exp_v1_file(file.string());
	ASSERT_TRUE(exported.creature);
	EXPECT_EQ(exported.creature->moniker, "test");
}

// Original assets must come from a caller-created, marked scratch copy.
class ShareData : public testing::Test {
protected:
	fs::path data, file;
	Creature* creature = nullptr;
	void SetUp() override {
		const char* input = std::getenv("OPENC1_SHARE_TEST_DATA");
		if (!input) GTEST_SKIP() << "Requires marked scratch C1 data";
		data = fs::canonical(input);
		ASSERT_TRUE(fs::is_regular_file(data / ".share-test-scratch"));
		g_engine_context.reset();
		if (!get_backend()) set_backend(NullBackend::get_instance());
		g_engine_context.paths->set_main_directory(data);
		g_engine_context.images->load_default_palette();
		fs::copy_file(data / "Genetics/mum1.gen", data / "Genetics/sImp.gen", fs::copy_options::overwrite_existing);
		creature = g_engine_context.objects->try_get(create_creature("sImp", 2))->as_creature();
		creature->history_name = "Share test";
		creature->gamete = creature->moniker;
		creature->move_ground_to(2591, 927);
		file = data / "share-test.exp";
	}
	void TearDown() override { g_engine_context.reset(); }
	std::vector<uint8_t> bytes(const fs::path& path) { FileReader in(path); return in.read_to_end(); }
	int creatures() {
		int count = 0;
		for (auto* obj : *g_engine_context.objects) count += obj->as_creature() != nullptr;
		return count;
	}
};

TEST_F(ShareData, DuplicateImportUsesItsOwnGenomeWhenBreeding) {
	export_creature(*creature, false, file.string());
	const auto source = bytes(file);
	auto* imported = import_creature(file.string(), 2591, 927);
	ASSERT_NE(imported, nullptr);
	EXPECT_NE(imported->moniker, creature->moniker);
	EXPECT_EQ(imported->history_moniker, imported->moniker);
	EXPECT_EQ(imported->gamete, imported->moniker); // F049
	EXPECT_EQ(imported->body->gallery().name, to_ascii_lowercase(imported->moniker));
	EXPECT_TRUE(fs::exists(data / "Images" / (imported->moniker + ".spr")));
	EXPECT_EQ(bytes(data / "Genetics" / (imported->moniker + ".gen")), bytes(data / "Genetics/sImp.gen"));
	EXPECT_EQ(bytes(file), source);
}

TEST_F(ShareData, ExportImportRoundTripPreservesPregnancyAndLearnedState) {
	creature->zygote = make_child_genome(creature->moniker, "dad1");
	const auto child = creature->zygote;
	const auto child_data = bytes(data / "Genetics" / (child + ".gen"));
	creature->biochemistry->chemicals[66].concentration = 177;
	creature->vocabulary[0].in = "hello";
	creature->asleep = 1;
	const auto uid = creature->uid;
	export_creature(*creature, true, file.string());
	g_engine_context.objects->destroy_pending();
	EXPECT_EQ(g_engine_context.objects->try_get(uid), nullptr);
	fs::remove(data / "Genetics" / (child + ".gen"));
	auto* imported = import_creature(file.string(), 2591, 927);
	ASSERT_NE(imported, nullptr);
	EXPECT_EQ(imported->moniker, "sImp");
	EXPECT_EQ(imported->zygote, child);
	EXPECT_EQ(imported->biochemistry->chemicals[66].concentration, 177);
	EXPECT_EQ(imported->vocabulary[0].in, "hello");
	EXPECT_EQ(imported->asleep, 1);
	EXPECT_EQ(bytes(data / "Genetics" / (child + ".gen")), child_data);
	EXPECT_TRUE(fs::exists(file));
}

TEST_F(ShareData, ConflictingStoredGenomeIsPreservedAndImportRenamed) {
	export_creature(*creature, true, file.string());
	g_engine_context.objects->destroy_pending();
	fs::copy_file(data / "Genetics/dad1.gen", data / "Genetics/sImp.gen", fs::copy_options::overwrite_existing);
	const auto existing = bytes(data / "Genetics/sImp.gen");
	auto* imported = import_creature(file.string(), 2591, 927);
	ASSERT_NE(imported, nullptr);
	EXPECT_NE(imported->moniker, "sImp");
	EXPECT_EQ(imported->gamete, imported->moniker);
	EXPECT_EQ(bytes(data / "Genetics/sImp.gen"), existing);
	EXPECT_EQ(bytes(data / "Genetics" / (imported->moniker + ".gen")), bytes(data / "Genetics/mum1.gen"));
}

TEST_F(ShareData, UnbornIdentityDoesNotCollideWithAnExistingEgg) {
	creature->zygote = make_child_genome(creature->moniker, "dad1");
	const auto child = creature->zygote;
	export_creature(*creature, true, file.string());
	g_engine_context.objects->destroy_pending();
	auto* egg = g_engine_context.objects->try_get(g_engine_context.objects->add<SimpleObject>());
	egg->family = 2; egg->genus = 5; egg->species = 2;
	egg->obv0 = moniker_to_integer(child);
	auto* imported = import_creature(file.string(), 2591, 927);
	ASSERT_NE(imported, nullptr);
	EXPECT_NE(imported->zygote, child);
	EXPECT_EQ(egg->obv0, moniker_to_integer(child));
}

TEST_F(ShareData, MissingImagesRejectImportWithoutLeavingHalfACreature) {
	export_creature(*creature, false, file.string());
	auto exp = sfc::read_exp_v1_file(file.string());
	exp.creature->body->gallery->filename = "none";
	exp.creature->body->gallery->images.clear();
	sfc::write_exp_v1_file(file.string(), exp);
	const int before = creatures();
	EXPECT_THROW(import_creature(file.string(), 2591, 927, true), std::exception);
	EXPECT_EQ(creatures(), before);
	EXPECT_TRUE(fs::exists(file));
}

TEST_F(ShareData, InvalidGenomeRejectsBeforeAddingCreatureOrDeletingSource) {
	export_creature(*creature, false, file.string());
	auto exp = sfc::read_exp_v1_file(file.string());
	exp.genome->data = {'b', 'a', 'd'};
	sfc::write_exp_v1_file(file.string(), exp);
	const int before = creatures();
	EXPECT_THROW(import_creature(file.string(), 2591, 927, true), std::exception);
	EXPECT_EQ(creatures(), before);
	EXPECT_TRUE(fs::exists(file));
}

TEST_F(ExportTest, NullCreatureExportIsRejected) {
	const auto file = root / "empty.exp";
	std::ofstream(file, std::ios::binary).write("\0\0\0\0", 4);
	EXPECT_THROW(import_creature(file.string(), 0, 0, true), std::exception);
	EXPECT_TRUE(fs::exists(file));
	EXPECT_NE(g_engine_context.objects->try_get(handle), nullptr);
}

#ifndef _WIN32
TEST_F(ShareData, FailedImportWriteKeepsSourceAndDoesNotAddCreature) {
	export_creature(*creature, false, file.string());
	const int before = creatures();
	{
		FileWriteFailure failure;
		if (!failure) GTEST_SKIP() << "Unable to set file-size limit";
		EXPECT_THROW(import_creature(file.string(), 2591, 927, true), std::exception);
	}
	EXPECT_EQ(creatures(), before);
	EXPECT_TRUE(fs::exists(file));
}
#endif

TEST_F(ShareData, ClassicImportConsumesOnlySuccessfullyImportedFile) {
	export_creature(*creature, false, file.string());
	ASSERT_NE(import_creature(file.string(), 2591, 927, true), nullptr);
	EXPECT_FALSE(fs::exists(file));
}

TEST_F(ShareData, OriginalExportsCanBeImportedAndReexported) {
	int count = 0;
	// loose exports are moved into Norns/ at start-up
	std::vector<fs::directory_entry> entries;
	for (const auto& folder : {data, data / "Norns"}) {
		if (fs::is_directory(folder)) entries.insert(entries.end(), fs::directory_iterator(folder), fs::directory_iterator());
	}
	for (const auto& entry : entries) {
		if (to_ascii_lowercase(entry.path().extension().string()) != ".exp" || entry.path().filename() == "share-test.exp") continue;
		const auto source = bytes(entry.path());
		auto* imported = import_creature(entry.path().string(), 2591, 927);
		ASSERT_NE(imported, nullptr);
		export_creature(*imported, true, file.string());
		g_engine_context.objects->destroy_pending();
		const auto exported = sfc::read_exp_v1_file(file.string());
		ASSERT_TRUE(exported.genome);
		EXPECT_EQ(bytes(entry.path()), source);
		++count;
	}
	EXPECT_GT(count, 0);
}

TEST_F(ExportTest, CancelledDragKeepsCreatureAndSelectionAndCleansTemporaryFile) {
	fs::path temporary;
	{
		CreatureExportTransfer transfer(*creature);
		temporary = transfer.file();
		EXPECT_TRUE(fs::exists(temporary));
		EXPECT_FALSE(transfer.complete(false));
		g_engine_context.objects->destroy_pending();
		EXPECT_NE(g_engine_context.objects->try_get(handle), nullptr);
		EXPECT_EQ(g_engine_context.world->current_norn, handle);
	}
	EXPECT_FALSE(fs::exists(temporary.parent_path()));
}

TEST_F(ExportTest, AcceptedDragRemovesOnlyAfterReceiverHasTheClosedExport) {
	CreatureExportTransfer transfer(*creature);
	const auto destination = root / "received.exp";
	fs::copy_file(transfer.file(), destination);
	EXPECT_TRUE(sfc::read_exp_v1_file(destination.string()).creature);
	EXPECT_TRUE(transfer.complete(true));
	EXPECT_FALSE(transfer.complete(true));
	g_engine_context.objects->destroy_pending();
	EXPECT_EQ(g_engine_context.objects->try_get(handle), nullptr);
	EXPECT_EQ(g_engine_context.world->current_norn, ObjectHandle{});
	EXPECT_TRUE(fs::exists(destination));
}

TEST_F(ExportTest, ConcurrentExportsWithTheSameNameUseDifferentTemporaryFiles) {
	CreatureExportTransfer first(*creature), second(*creature);
	EXPECT_NE(first.file(), second.file());
	EXPECT_TRUE(fs::exists(first.file()));
	EXPECT_TRUE(fs::exists(second.file()));
}

TEST_F(ExportTest, OldDragCannotRemoveACreatureFromANewWorld) {
	CreatureExportTransfer transfer(*creature);
	g_engine_context.reset();
	const auto replacement = g_engine_context.objects->add<Creature>();
	g_engine_context.objects->try_get(replacement)->as_creature()->moniker = "test";
	EXPECT_FALSE(transfer.complete(true));
	g_engine_context.objects->destroy_pending();
	EXPECT_NE(g_engine_context.objects->try_get(replacement), nullptr);
}

TEST_F(ShareData, FailedCreatureLoadRollsBackOnlyItsOwnAllocation) {
	auto saved = sfc_dump_creature(*creature);
	saved->gallery->filename = "none";
	saved->gallery->images.clear();
	const int before = creatures();
	EXPECT_THROW(sfc_load_creature(saved.get()), std::exception);
	EXPECT_EQ(creatures(), before);
	EXPECT_NE(g_engine_context.objects->try_get(creature->uid), nullptr);
}

TEST_F(ExportTest, PersistentRecoverySurvivesSuccessfulOrFailedDrag) {
	for (bool accepted : {false, true}) {
		fs::path recovery;
		{
			CreatureExportTransfer transfer(*creature, root / "Export Recovery");
			recovery = transfer.file();
			EXPECT_EQ(sfc::read_exp_v1_file(recovery.string()).creature->moniker, "test");
			EXPECT_EQ(transfer.complete(accepted), accepted);
		}
		// The only export copy must not vanish when the drag callback finishes.
		EXPECT_TRUE(fs::exists(recovery));
		EXPECT_EQ(sfc::read_exp_v1_file(recovery.string()).creature->moniker, "test");
	}
}

TEST_F(ExportTest, FailedRecoveryWritePreventsDragAndPreservesCreature) {
	const auto blocked = root / "blocked";
	std::ofstream(blocked) << "a file cannot hold recovery exports";
	EXPECT_THROW(CreatureExportTransfer(*creature, blocked / "Export Recovery"), std::exception);
	g_engine_context.objects->destroy_pending();
	EXPECT_NE(g_engine_context.objects->try_get(handle), nullptr);
	EXPECT_EQ(g_engine_context.world->current_norn, handle);
}

TEST(Genetics, RegisterMonikersAreWrittenInTheOriginalsHex) {
	// as Jeff's original exports have them: hex of the moniker's 32-bit value, "0" for none
	EXPECT_EQ(register_moniker_text("1EAV"), "56414531");
	EXPECT_EQ(register_moniker_text(""), "0");
	EXPECT_EQ(register_moniker("79626f74"), "toby");
	EXPECT_EQ(register_moniker("0"), "");
	EXPECT_EQ(register_moniker("toby"), "toby"); // plain, as we used to write them
}

TEST_F(ShareData, OriginalExportsKeepTheirParentsTheRightWayRound) {
	// Foxy's register says father 0 then mother "toby" (the file's third and fourth strings)
	auto* imported = import_creature((data / "Norns/Foxy.exp").string(), 2591, 927);
	ASSERT_NE(imported, nullptr);
	EXPECT_EQ(imported->history_moms_moniker, "toby");
	EXPECT_EQ(imported->history_dads_moniker, "");
	EXPECT_EQ(imported->history_moniker, imported->moniker);
	export_creature(*imported, true, file.string());
	const auto exported = sfc::read_exp_v1_file(file.string());
	EXPECT_EQ(exported.creature->history_moms_moniker, "79626f74");
	EXPECT_EQ(exported.creature->history_dads_moniker, "0");
	EXPECT_EQ(register_moniker(exported.creature->history_moniker), exported.creature->moniker);
}
