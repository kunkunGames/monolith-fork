#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithSQLitePragmaPolicy.h"
#include "MonolithSQLiteSearchText.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSQLitePragmaPresetTest,
	"Monolith.SQLite.PragmaPresetSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSQLitePragmaPresetTest::RunTest(const FString& Parameters)
{
	const FMonolithSQLitePragmaPreset High64 = SelectMonolithSQLitePragmaPreset(65536, true);
	TestEqual(TEXT("64GB 64-bit mmap"), High64.MmapSizeBytes, int64(2147483648));
	TestEqual(TEXT("64GB 64-bit cache"), High64.CacheSizeKiB, int64(-512000));
	TestTrue(TEXT("64GB 64-bit temp_store memory"), High64.bUseMemoryTempStore);

	const FMonolithSQLitePragmaPreset Low64 = SelectMonolithSQLitePragmaPreset(4096, true);
	TestEqual(TEXT("<8GB 64-bit mmap"), Low64.MmapSizeBytes, int64(67108864));
	TestEqual(TEXT("<8GB 64-bit cache"), Low64.CacheSizeKiB, int64(-16000));
	TestFalse(TEXT("<8GB 64-bit keeps temp_store default"), Low64.bUseMemoryTempStore);

	const FMonolithSQLitePragmaPreset Any32 = SelectMonolithSQLitePragmaPreset(65536, false);
	TestEqual(TEXT("32-bit mmap"), Any32.MmapSizeBytes, int64(33554432));
	TestEqual(TEXT("32-bit cache"), Any32.CacheSizeKiB, int64(-8000));
	TestFalse(TEXT("32-bit keeps temp_store default"), Any32.bUseMemoryTempStore);

	const FMonolithSQLitePragmaPreset Capped64 = SelectMonolithSQLitePragmaPreset(65536, true, 1024);
	TestTrue(TEXT("available memory caps mmap"), Capped64.MmapSizeBytes <= int64(134217728));
	TestTrue(TEXT("available memory caps cache"), Capped64.CacheSizeKiB >= int64(-65536));
	TestFalse(TEXT("low available memory disables temp_store memory"), Capped64.bUseMemoryTempStore);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSQLiteOpenModeTest,
	"Monolith.SQLite.OpenModeFromIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSQLiteOpenModeTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("QueryOnly uses ReadOnly"),
		GetMonolithSQLiteOpenMode(EMonolithSQLiteIntent::QueryOnly),
		ESQLiteDatabaseOpenMode::ReadOnly);
	TestEqual(TEXT("UpdateExisting uses ReadWrite"),
		GetMonolithSQLiteOpenMode(EMonolithSQLiteIntent::UpdateExisting),
		ESQLiteDatabaseOpenMode::ReadWrite);
	TestEqual(TEXT("CreateOrRebuild uses ReadWriteCreate"),
		GetMonolithSQLiteOpenMode(EMonolithSQLiteIntent::CreateOrRebuild),
		ESQLiteDatabaseOpenMode::ReadWriteCreate);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSQLiteSearchTextTest,
	"Monolith.SQLite.SearchTextExpansion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSQLiteSearchTextTest::RunTest(const FString& Parameters)
{
	const FString Expanded = BuildMonolithSQLiteSearchText(TEXT("WBP_InventoryItem MyPlayerCharacter"));
	TestTrue(TEXT("splits InventoryItem"), Expanded.Contains(TEXT("Inventory")));
	TestTrue(TEXT("splits InventoryItem suffix"), Expanded.Contains(TEXT("Item")));
	TestTrue(TEXT("splits PlayerCharacter"), Expanded.Contains(TEXT("Player")));
	TestTrue(TEXT("splits PlayerCharacter suffix"), Expanded.Contains(TEXT("Character")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
