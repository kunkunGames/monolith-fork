#include "Misc/AutomationTest.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceReview.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSymbolsClampsLimitTest, "Monolith.IndexGuard.Source.SearchSymbolsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceSearchSymbolsClampsLimitTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSourceQuery"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("Temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("Temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("TestModule"), TEXT("/tmp/TestModule"), TEXT("Runtime"));
	const int64 FileId = DB.InsertFile(TEXT("/tmp/TestModule/Test.cpp"), ModuleId, TEXT("cpp"), 1, 0.0);
	TestTrue(TEXT("Test module inserted"), ModuleId != 0);
	TestTrue(TEXT("Test file inserted"), FileId != 0);

	for (int32 Index = 0; Index < 1100; ++Index)
	{
		const FString QualifiedName = FString::Printf(TEXT("TestModule::TestSymbol%d"), Index);
		DB.InsertSymbol(TEXT("TestSymbol"), QualifiedName, TEXT("function"), FileId, Index + 1, Index + 1, 0, TEXT("public"), TEXT("void TestSymbol()"), TEXT(""), false);
	}

	TArray<FMonolithSourceSymbol> Results = DB.SearchSymbolsFTS(TEXT("TestSymbol"), 50000);

	TestEqual(TEXT("Huge FTS limit is clamped to 1000"), Results.Num(), 1000);

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);

	return true;
}

// ============================================================================
// CRG-inspired navigation/review tests
// ============================================================================
namespace
{
	struct FTempSourceDb
	{
		FMonolithSourceDatabase Db;
		FString Path;
		int64 Sa = 0, Sb = 0, Sc = 0;

		bool Build()
		{
			Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcReview"), TEXT(".sqlite"));
			if (!Db.OpenForWriting(Path)) return false;
			if (!Db.CreateTablesIfNeeded()) return false;
			const int64 Mod = Db.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
			const int64 File = Db.InsertFile(TEXT("/tmp/M/M.cpp"), Mod, TEXT("cpp"), 100, 0.0);
			Sa = Db.InsertSymbol(TEXT("Alpha"), TEXT("M::Alpha"), TEXT("function"), File, 1, 5, 0, TEXT("public"), TEXT("void Alpha()"), TEXT(""), false);
			Sb = Db.InsertSymbol(TEXT("Beta"), TEXT("M::Beta"), TEXT("function"), File, 6, 10, 0, TEXT("public"), TEXT("void Beta()"), TEXT(""), true);
			Sc = Db.InsertSymbol(TEXT("Gamma"), TEXT("M::Gamma"), TEXT("class"), File, 11, 20, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			// Beta -> Gamma -> Alpha -> Beta  (reference cycle), plus inheritance
			Db.InsertReference(Sb, Sc, TEXT("call"), File, 7);
			Db.InsertReference(Sc, Sa, TEXT("type"), File, 12);
			Db.InsertReference(Sa, Sb, TEXT("call"), File, 2);
			Db.InsertInheritance(Sc, Sa);
			Db.SetMeta(TEXT("schema_version"), TEXT("1"));
			return Sa > 0 && Sb > 0 && Sc > 0;
		}
		~FTempSourceDb()
		{
			Db.Close();
			if (!Path.IsEmpty()) FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceImpactRadiusCycleSafeTest, "Monolith.IndexGuard.Source.ImpactRadiusCycleSafe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceImpactRadiusCycleSafeTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("call|type|inheritance"), TEXT("both"), 5, 200);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Imp = nullptr;
	TestTrue(TEXT("impacted_symbols present"), R->TryGetArrayField(TEXT("impacted_symbols"), Imp) && Imp != nullptr);
	TestTrue(TEXT("cycle-safe finite (<=2 other symbols)"), Imp->Num() >= 1 && Imp->Num() <= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceHealthHealthyTest, "Monolith.IndexGuard.Source.HealthHealthy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceHealthHealthyTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.ComputeHealth(true);
	TestEqual(TEXT("fresh consistent source DB is healthy"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings present"), R->TryGetArrayField(TEXT("warnings"), W) && W != nullptr);
	TestEqual(TEXT("no warnings"), W->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRepairFtsSourceDegradesTest, "Monolith.IndexGuard.Source.RepairFtsSourceDegrades", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRepairFtsSourceDegradesTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	// target=source must NOT rebuild (plain fts5) — always reindex guidance.
	TSharedPtr<FJsonObject> Src = T.Db.RepairFts(TEXT("source"), true);
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("source target yields reindex warning"), Src->TryGetArrayField(TEXT("warnings"), W) && W && W->Num() >= 1);
	// target=symbols dry-run does not mutate.
	TSharedPtr<FJsonObject> Dry = T.Db.RepairFts(TEXT("symbols"), false);
	TestEqual(TEXT("symbols dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewContextMinimalTest, "Monolith.IndexGuard.Source.ReviewContextMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceReviewContextMinimalTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewContext(T.Db, TEXT("Beta"), TEXT("both"), 2, 200, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("has risk"), R->HasField(TEXT("risk")));
	TestTrue(TEXT("has impact"), R->HasField(TEXT("impact")));
	TestTrue(TEXT("has next_actions"), R->HasField(TEXT("next_actions")));
	return true;
}
