#include "Misc/AutomationTest.h"
#include "MonolithSourceBridgeHelpers.h"
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
		int64 FileId = 0;
		int64 Sa = 0, Sb = 0, Sc = 0, Sd = 0, Se = 0;

		bool Build()
		{
			Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcReview"), TEXT(".sqlite"));
			if (!Db.OpenForWriting(Path)) return false;
			if (!Db.CreateTablesIfNeeded()) return false;
			const int64 Mod = Db.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
			FileId = Db.InsertFile(TEXT("/tmp/M/M.cpp"), Mod, TEXT("cpp"), 200, 0.0);
			Sa = Db.InsertSymbol(TEXT("Alpha"), TEXT("M::Alpha"), TEXT("function"), FileId, 1, 5, 0, TEXT("public"), TEXT("void Alpha()"), TEXT(""), false);
			Sb = Db.InsertSymbol(TEXT("Beta"), TEXT("M::Beta"), TEXT("function"), FileId, 6, 10, 0, TEXT("public"), TEXT("void Beta()"), TEXT(""), true);
			Sc = Db.InsertSymbol(TEXT("Gamma"), TEXT("M::Gamma"), TEXT("class"), FileId, 11, 20, 0, TEXT("public"), TEXT(""), TEXT(""), false);
			Sd = Db.InsertSymbol(TEXT("ServerSaveGame"), TEXT("M::ServerSaveGame"), TEXT("function"), FileId, 21, 160, 0, TEXT("public"), TEXT("UFUNCTION(Server) void ServerSaveGame()"), TEXT(""), false);
			Se = Db.InsertSymbol(TEXT("UnusedUtility"), TEXT("M::UnusedUtility"), TEXT("function"), FileId, 161, 170, 0, TEXT("public"), TEXT("void UnusedUtility()"), TEXT(""), false);
			// Beta -> Gamma -> Alpha -> Beta  (reference cycle), plus inheritance
			Db.InsertReference(Sb, Sc, TEXT("call"), FileId, 7);
			Db.InsertReference(Sc, Sa, TEXT("type"), FileId, 12);
			Db.InsertReference(Sa, Sb, TEXT("call"), FileId, 2);
			Db.InsertInheritance(Sc, Sa);
			Db.SetMeta(TEXT("schema_version"), TEXT("1"));
			TSharedPtr<FJsonObject> Crg = Db.RepairCrgCache(true);
			return Sa > 0 && Sb > 0 && Sc > 0 && Sd > 0 && Se > 0
				&& Crg.IsValid() && Crg->GetStringField(TEXT("status")) == TEXT("ok");
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceImpactRadiusFiltersRefKindTest, "Monolith.IndexGuard.Source.ImpactRadiusFiltersRefKind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceImpactRadiusFiltersRefKindTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TSharedPtr<FJsonObject> CallOnly = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("call"), TEXT("both"), 1, 200);
	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	TestTrue(TEXT("call edges present"), CallOnly->TryGetArrayField(TEXT("edges"), Edges) && Edges != nullptr);
	for (const TSharedPtr<FJsonValue>& EdgeValue : *Edges)
	{
		const TSharedPtr<FJsonObject> Edge = EdgeValue->AsObject();
		TestTrue(TEXT("edge object valid"), Edge.IsValid());
		TestEqual(TEXT("call-only excludes type references"), Edge->GetStringField(TEXT("kind")), FString(TEXT("call")));
	}

	TSharedPtr<FJsonObject> TypeOnly = FMonolithSourceReview::ImpactRadius(T.Db, TEXT("Beta"), TEXT("type"), TEXT("both"), 1, 200);
	const TArray<TSharedPtr<FJsonValue>>* TypeImp = nullptr;
	TestTrue(TEXT("type impacted_symbols present"), TypeOnly->TryGetArrayField(TEXT("impacted_symbols"), TypeImp) && TypeImp != nullptr);
	TestEqual(TEXT("Beta has no direct type references"), TypeImp->Num(), 0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceHealthWarnsOnOrphanReferenceTest, "Monolith.IndexGuard.Source.HealthWarnsOnOrphanReference", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceHealthWarnsOnOrphanReferenceTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	T.Db.InsertReference(T.Sa, 999999, TEXT("call"), T.FileId, 22);

	TSharedPtr<FJsonObject> R = T.Db.ComputeHealth(false);
	TestEqual(TEXT("orphan reference yields warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings present"), R->TryGetArrayField(TEXT("warnings"), W) && W && W->Num() >= 1);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRepairCrgCacheTest, "Monolith.IndexGuard.Source.RepairCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRepairCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> Dry = T.Db.RepairCrgCache(false);
	TestEqual(TEXT("dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
	TestTrue(TEXT("plan present"), Dry->TryGetArrayField(TEXT("plan"), Plan) && Plan && Plan->Num() >= 3);

	TSharedPtr<FJsonObject> Exec = T.Db.RepairCrgCache(true);
	TestEqual(TEXT("execute ok"), Exec->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> After = Exec->GetObjectField(TEXT("after"));
	TestTrue(TEXT("after counts present"), After.IsValid());
	TestEqual(TEXT("one CRG node per symbol"), After->GetIntegerField(TEXT("crg_nodes")), 5);
	TestEqual(TEXT("reference + inheritance edges"), After->GetIntegerField(TEXT("crg_edges")), 4);
	TestEqual(TEXT("one metric per CRG node"), After->GetIntegerField(TEXT("crg_node_metrics")), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSnapshotDiffTest, "Monolith.IndexGuard.Source.SnapshotDiff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceSnapshotDiffTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TSharedPtr<FJsonObject> Dry = T.Db.Snapshot(TEXT("base"), false);
	TestEqual(TEXT("snapshot dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestFalse(TEXT("dry-run does not execute"), Dry->GetBoolField(TEXT("executed")));

	TSharedPtr<FJsonObject> Snap = T.Db.Snapshot(TEXT("base"), true);
	TestEqual(TEXT("snapshot execute ok"), Snap->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("snapshot executed"), Snap->GetBoolField(TEXT("executed")));
	TestEqual(TEXT("snapshot captures five source nodes"), Snap->GetIntegerField(TEXT("node_count")), 5);
	TestEqual(TEXT("snapshot captures four source edges"), Snap->GetIntegerField(TEXT("edge_count")), 4);

	const int64 NewId = T.Db.InsertSymbol(
		TEXT("NewReviewSymbol"),
		TEXT("M::NewReviewSymbol"),
		TEXT("function"),
		T.FileId,
		171,
		172,
		0,
		TEXT("public"),
		TEXT("void NewReviewSymbol()"),
		TEXT(""),
		false);
	TestTrue(TEXT("new source symbol inserted"), NewId > 0);
	TSharedPtr<FJsonObject> Rebuilt = T.Db.RepairCrgCache(true);
	TestEqual(TEXT("crg cache rebuilt after insert"), Rebuilt->GetStringField(TEXT("status")), FString(TEXT("ok")));

	TSharedPtr<FJsonObject> Diff = T.Db.DiffSnapshots(TEXT("base"), TEXT("current"), 10);
	TestEqual(TEXT("diff ok"), Diff->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> Counts = Diff->GetObjectField(TEXT("summary_counts"));
	TestTrue(TEXT("summary counts present"), Counts.IsValid());
	TestTrue(TEXT("one or more source nodes added"), Counts->GetIntegerField(TEXT("nodes_added")) >= 1);
	const TArray<TSharedPtr<FJsonValue>>* NewNodes = nullptr;
	TestTrue(TEXT("new_nodes sample present"), Diff->TryGetArrayField(TEXT("new_nodes"), NewNodes) && NewNodes && NewNodes->Num() >= 1);
	TestFalse(TEXT("diff not truncated"), Diff->GetBoolField(TEXT("truncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreUsesCrgCacheTest, "Monolith.IndexGuard.Source.RiskScoreUsesCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRiskScoreUsesCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::RiskScore(T.Db, TEXT("Beta"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() >= 1);
	TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
	TestTrue(TEXT("item object"), Item.IsValid());
	TSharedPtr<FJsonObject> Cache = Item->GetObjectField(TEXT("cache"));
	TestTrue(TEXT("cache object"), Cache.IsValid());
	TestEqual(TEXT("cache hit"), Cache->GetStringField(TEXT("status")), FString(TEXT("hit")));
	double Score = 0.0;
	TestTrue(TEXT("risk score present"), Item->TryGetNumberField(TEXT("score"), Score));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreSensitivityTest, "Monolith.IndexGuard.Source.RiskScoreSensitivity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceRiskScoreSensitivityTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::RiskScore(T.Db, TEXT("ServerSaveGame"), 10, TEXT("low"));
	TestEqual(TEXT("root scoring version is v3"), R->GetStringField(TEXT("scoring_version")), FString(TEXT("3")));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
	TestTrue(TEXT("item object"), Item.IsValid());
	TSharedPtr<FJsonObject> Raw = Item->GetObjectField(TEXT("raw_counts"));
	TestTrue(TEXT("raw counts present"), Raw.IsValid());
	double Sensitivity = 0.0;
	TestTrue(TEXT("sensitivity raw count present"), Raw->TryGetNumberField(TEXT("sensitivity"), Sensitivity));
	TestTrue(TEXT("sensitivity contributes"), Sensitivity > 0.0);
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("reasons present"), Item->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons);
	bool bFound = false;
	for (const TSharedPtr<FJsonValue>& Reason : *Reasons)
	{
		bFound = bFound || Reason->AsString().Contains(TEXT("sensitivity:"));
	}
	TestTrue(TEXT("sensitivity reason present"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesMinimalTest, "Monolith.IndexGuard.Source.DetectChangesMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesMinimalTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("/tmp/M/M.cpp") }, 10, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("all file symbols changed"), R->GetIntegerField(TEXT("changed_entity_count")), 5);
	TestTrue(TEXT("heuristic test gaps present"), R->GetIntegerField(TEXT("test_gap_count")) >= 1);
	TestFalse(TEXT("minimal omits full changed_entities"), R->HasField(TEXT("changed_entities")));
	const TArray<TSharedPtr<FJsonValue>>* Priorities = nullptr;
	TestTrue(TEXT("priorities present"), R->TryGetArrayField(TEXT("review_priorities"), Priorities) && Priorities && Priorities->Num() >= 1);
	TestTrue(TEXT("scoring version set"), R->GetStringField(TEXT("scoring_version")) == TEXT("3"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesStandardTest, "Monolith.IndexGuard.Source.DetectChangesStandard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesStandardTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 10, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 5);
	const TArray<TSharedPtr<FJsonValue>>* Gaps = nullptr;
	TestTrue(TEXT("test_gaps present"), R->TryGetArrayField(TEXT("test_gaps"), Gaps) && Gaps && Gaps->Num() >= 1);
	TSharedPtr<FJsonObject> Impact = R->GetObjectField(TEXT("impact"));
	TestTrue(TEXT("impact present"), Impact.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesEscapesPathWildcardsTest, "Monolith.IndexGuard.Source.DetectChangesEscapesPathWildcards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesEscapesPathWildcardsTest::RunTest(const FString& Parameters)
{
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithSrcDetectWildcards"), TEXT(".sqlite"));
	FMonolithSourceDatabase DB;

	TestTrue(TEXT("temporary DB opens for writing"), DB.OpenForWriting(DbPath));
	TestTrue(TEXT("temporary DB creates schema"), DB.CreateTablesIfNeeded());

	const int64 ModuleId = DB.InsertModule(TEXT("M"), TEXT("/tmp/M"), TEXT("Runtime"));
	const int64 UnderFileId = DB.InsertFile(TEXT("/tmp/M/Foo_Bar.cpp"), ModuleId, TEXT("cpp"), 12, 0.0);
	const int64 PlainFileId = DB.InsertFile(TEXT("/tmp/M/FooXBar.cpp"), ModuleId, TEXT("cpp"), 12, 0.0);
	const int64 UnderSymbolId = DB.InsertSymbol(TEXT("FooUnderSymbol"), TEXT("M::FooUnderSymbol"), TEXT("function"), UnderFileId, 1, 3, 0, TEXT("public"), TEXT("void FooUnderSymbol()"), TEXT(""), false);
	const int64 PlainSymbolId = DB.InsertSymbol(TEXT("FooXSymbol"), TEXT("M::FooXSymbol"), TEXT("function"), PlainFileId, 1, 3, 0, TEXT("public"), TEXT("void FooXSymbol()"), TEXT(""), false);
	TestTrue(TEXT("wildcard fixture inserted"), ModuleId > 0 && UnderFileId > 0 && PlainFileId > 0 && UnderSymbolId > 0 && PlainSymbolId > 0);

	TSharedPtr<FJsonObject> R = DB.DetectChanges({ TEXT("Foo_Bar.cpp") }, 10, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("underscore path is treated literally"), R->GetIntegerField(TEXT("changed_entity_count")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 1);
	if (Changed && Changed->Num() == 1)
	{
		TSharedPtr<FJsonObject> First = (*Changed)[0]->AsObject();
		TestTrue(TEXT("first changed object"), First.IsValid());
		if (First.IsValid())
		{
			TestEqual(TEXT("literal underscore file matched"), First->GetStringField(TEXT("file")), FString(TEXT("/tmp/M/Foo_Bar.cpp")));
			TestEqual(TEXT("overmatching file excluded"), First->GetStringField(TEXT("name")), FString(TEXT("FooUnderSymbol")));
		}
	}

	DB.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePreMergeCheckWarnsOnTestGaps, "Monolith.IndexGuard.Source.PreMergeCheckWarnsOnTestGaps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePreMergeCheckWarnsOnTestGaps::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.PreMergeCheck({ TEXT("/tmp/M/M.cpp") }, 10, 5, TEXT("minimal"), false);
	TestEqual(TEXT("warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	TestEqual(TEXT("decision warn"), R->GetStringField(TEXT("decision")), FString(TEXT("warn")));
	TestEqual(TEXT("all file symbols changed"), R->GetIntegerField(TEXT("changed_entity_count")), 5);
	TestTrue(TEXT("heuristic test gaps carried into gate"), R->GetIntegerField(TEXT("test_gap_count")) >= 1);
	TestFalse(TEXT("minimal omits nested change analysis"), R->HasField(TEXT("change_analysis")));
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	TestTrue(TEXT("checks present"), R->TryGetArrayField(TEXT("checks"), Checks) && Checks && Checks->Num() >= 2);
	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	TestTrue(TEXT("findings present"), R->TryGetArrayField(TEXT("findings"), Findings) && Findings && Findings->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourcePreMergeCheckStandardIncludesNestedPayloads, "Monolith.IndexGuard.Source.PreMergeCheckStandardIncludesNestedPayloads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourcePreMergeCheckStandardIncludesNestedPayloads::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.PreMergeCheck({ TEXT("M.cpp") }, 10, 5, TEXT("standard"), true);
	TestEqual(TEXT("decision warn"), R->GetStringField(TEXT("decision")), FString(TEXT("warn")));
	TestTrue(TEXT("unused sample surfaced"), R->GetIntegerField(TEXT("unused_count")) >= 1);
	TestTrue(TEXT("standard includes health payload"), R->HasField(TEXT("health")));
	TestTrue(TEXT("standard includes change analysis"), R->HasField(TEXT("change_analysis")));
	TestTrue(TEXT("standard includes unused payload"), R->HasField(TEXT("unused")));
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	TestTrue(TEXT("health/detect/unused checks present"), R->TryGetArrayField(TEXT("checks"), Checks) && Checks && Checks->Num() >= 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewHotspotsLargeTest, "Monolith.IndexGuard.Source.ReviewHotspotsLarge", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceReviewHotspotsLargeTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithSourceReview::ReviewHotspots(T.Db, TEXT("large"), 5, 100, true);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Hotspots = nullptr;
	TestTrue(TEXT("hotspots present"), R->TryGetArrayField(TEXT("hotspots"), Hotspots) && Hotspots && Hotspots->Num() >= 1);
	TSharedPtr<FJsonObject> First = (*Hotspots)[0]->AsObject();
	TestTrue(TEXT("first hotspot object"), First.IsValid());
	TestEqual(TEXT("large hotspot picks ServerSaveGame"), First->GetStringField(TEXT("name")), FString(TEXT("ServerSaveGame")));
	TestTrue(TEXT("signals field present"), First->HasField(TEXT("signals")));
	TSharedPtr<FJsonObject> Signals = First->GetObjectField(TEXT("signals"));
	TestTrue(TEXT("signals object valid"), Signals.IsValid());
	TestTrue(TEXT("signals include lines"), Signals->HasField(TEXT("lines")));
	const TArray<TSharedPtr<FJsonValue>>* Questions = nullptr;
	TestTrue(TEXT("questions present"), R->TryGetArrayField(TEXT("questions"), Questions) && Questions && Questions->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceFindUnusedAdvisoryTest, "Monolith.IndexGuard.Source.FindUnusedAdvisory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceFindUnusedAdvisoryTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TSharedPtr<FJsonObject> R = T.Db.FindUnused(TEXT("function"), 5, TEXT("medium"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> First = (*Items)[0]->AsObject();
	TestTrue(TEXT("first candidate object"), First.IsValid());
	TestEqual(TEXT("unused candidate is non-reflected function"), First->GetStringField(TEXT("name")), FString(TEXT("UnusedUtility")));
	TestEqual(TEXT("unused candidate is medium confidence"), First->GetStringField(TEXT("confidence")), FString(TEXT("medium")));
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("reasons present"), First->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons && Reasons->Num() >= 3);

	TSharedPtr<FJsonObject> High = T.Db.FindUnused(TEXT("function"), 5, TEXT("high"));
	const TArray<TSharedPtr<FJsonValue>>* HighItems = nullptr;
	TestTrue(TEXT("high-confidence filter returns array"), High->TryGetArrayField(TEXT("items"), HighItems) && HighItems != nullptr);
	TestEqual(TEXT("find_unused never reports high confidence"), HighItems->Num(), 0);
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
	TestTrue(TEXT("has limits"), R->HasField(TEXT("limits")));
	TestTrue(TEXT("has top risks"), R->HasField(TEXT("top_risks")));
	TestTrue(TEXT("has compact context"), R->HasField(TEXT("context")));
	TestTrue(TEXT("has next_actions"), R->HasField(TEXT("next_actions")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceBridgeCandidateNormalizationTest, "Monolith.IndexGuard.Source.BridgeCandidateNormalization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceBridgeCandidateNormalizationTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("BP_/GA_ prefixes and _C suffix normalize to source-style class seed"),
		MonolithSourceBridge::NormalizeBridgeName(TEXT("/Game/Abilities/BP_GA_Fireball_C")),
		FString(TEXT("Fireball")));
	TestTrue(TEXT("UObject-style prefix matches normalized asset seed"),
		MonolithSourceBridge::NamesMatchNormalized(TEXT("BP_PlayerCharacter"), TEXT("APlayerCharacter")));

	const TArray<FString> AssetCandidates = MonolithSourceBridge::BuildAssetSymbolCandidates(
		TEXT("/Game/UI/WBP_Inventory"),
		TEXT("WBP_Inventory_C"),
		TEXT("WidgetBlueprint"));
	TestTrue(TEXT("asset candidates keep raw asset name"), AssetCandidates.Contains(TEXT("WBP_Inventory")));
	TestTrue(TEXT("asset candidates include normalized class seed"), AssetCandidates.Contains(TEXT("Inventory")));
	TestTrue(TEXT("asset candidates include U-prefixed source class seed"), AssetCandidates.Contains(TEXT("UInventory")));

	const TArray<FString> SymbolCandidates = MonolithSourceBridge::BuildSymbolAssetCandidates(TEXT("UGameplayInventory"), TEXT("Go::UGameplayInventory"));
	TestTrue(TEXT("symbol candidates include normalized asset token"), SymbolCandidates.Contains(TEXT("GameplayInventory")));
	TestTrue(TEXT("symbol candidates include Blueprint-prefixed token"), SymbolCandidates.Contains(TEXT("BP_GameplayInventory")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceEscapeFTSPreservesSafeTokensTest, "Monolith.IndexGuard.Source.EscapeFTSPreservesSafeTokens", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceEscapeFTSPreservesSafeTokensTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Simple word is wrapped with quotes and star"), FMonolithSourceDatabase::EscapeFTS(TEXT("Actor")), TEXT("\"Actor\"*"));
	TestEqual(TEXT("Namespaces are converted to spaces and individually wrapped"), FMonolithSourceDatabase::EscapeFTS(TEXT("UE::Math::Vector")), TEXT("\"UE\"* \"Math\"* \"Vector\"*"));
	TestEqual(TEXT("Punctuation is stripped"), FMonolithSourceDatabase::EscapeFTS(TEXT("FString*;[]()")), TEXT("\"FString\"*"));
	TestEqual(TEXT("Multiple spaces are collapsed"), FMonolithSourceDatabase::EscapeFTS(TEXT("Get   Actor   Location")), TEXT("\"Get\"* \"Actor\"* \"Location\"*"));
	TestEqual(TEXT("Empty or fully stripped string returns quoted empty"), FMonolithSourceDatabase::EscapeFTS(TEXT("!@#$")), TEXT("\"\""));

	return true;
}

// ============================================================================
// RX-1.1 detect_changes line-range precision
// Fixture symbols (all in /tmp/M/M.cpp): Alpha 1-5, Beta 6-10, Gamma 11-20,
// ServerSaveGame 21-160, UnusedUtility 161-170.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesLinePrecisionTest, "Monolith.IndexGuard.Source.DetectChangesLinePrecision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesLinePrecisionTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	TMap<FString, TArray<TPair<int32, int32>>> Ranges;
	Ranges.Add(TEXT("M.cpp"), { TPair<int32, int32>(7, 8) });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"), Ranges);

	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("precision is line"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("line")));
	TestEqual(TEXT("range_paths is 1"), (int32)(*In)->GetNumberField(TEXT("range_paths")), 1);
	TestEqual(TEXT("only the overlapping symbol (Beta 6-10) is changed"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Ents = nullptr;
	TestTrue(TEXT("changed_entities present in standard"), R->TryGetArrayField(TEXT("changed_entities"), Ents) && Ents);
	if (Ents && Ents->Num() == 1)
	{
		TestEqual(TEXT("changed symbol is Beta"), (*Ents)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Beta")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesNoRangeRegressionTest, "Monolith.IndexGuard.Source.DetectChangesNoRangeRegression", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesNoRangeRegressionTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	// No ranges supplied -> original file-level behavior (all 5 symbols in M.cpp).
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("precision is file when no ranges"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("file")));
	TestEqual(TEXT("all file-level symbols returned (regression)"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesDiffParseTest, "Monolith.IndexGuard.Source.DetectChangesDiffParse", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesDiffParseTest::RunTest(const FString& Parameters)
{
	// Normal hunk (count given) + pure-deletion hunk (count==0 -> end=start).
	const FString Diff =
		TEXT("--- a/M.cpp\n")
		TEXT("+++ b/M.cpp\n")
		TEXT("@@ -6,0 +7,2 @@ void Beta()\n")
		TEXT("+added\n+added\n")
		TEXT("@@ -20,3 +25,0 @@ void Gone()\n");
	TMap<FString, TArray<TPair<int32, int32>>> Parsed = FMonolithSourceDatabase::ParseUnifiedDiffRanges(Diff);
	const TArray<TPair<int32, int32>>* M = Parsed.Find(TEXT("M.cpp"));
	TestTrue(TEXT("M.cpp parsed"), M != nullptr && M->Num() == 2);
	if (M && M->Num() == 2)
	{
		TestEqual(TEXT("hunk +7,2 -> start 7"), (*M)[0].Key, 7);
		TestEqual(TEXT("hunk +7,2 -> end 8"), (*M)[0].Value, 8);
		TestEqual(TEXT("deletion +25,0 -> start 25"), (*M)[1].Key, 25);
		TestEqual(TEXT("deletion +25,0 -> end 25"), (*M)[1].Value, 25);
	}
	TestEqual(TEXT("empty diff -> empty map"), FMonolithSourceDatabase::ParseUnifiedDiffRanges(TEXT("")).Num(), 0);
	TestEqual(TEXT("garbage diff -> empty map"), FMonolithSourceDatabase::ParseUnifiedDiffRanges(TEXT("not a diff\nrandom")).Num(), 0);

	// End-to-end: parsed diff drives precision selection (Beta only).
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());
	TMap<FString, TArray<TPair<int32, int32>>> Only78;
	Only78.Add(TEXT("M.cpp"), { (*M)[0] });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges(TArray<FString>{}, 200, TEXT("minimal"), Only78);
	TestEqual(TEXT("range-only path is treated as a changed path"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceDetectChangesRobustnessTest, "Monolith.IndexGuard.Source.DetectChangesRobustness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FSourceDetectChangesRobustnessTest::RunTest(const FString& Parameters)
{
	FTempSourceDb T;
	TestTrue(TEXT("temp source db built"), T.Build());

	// Only malformed ranges (start>end, negative) -> sanitized away -> the
	// path degrades to file-level, never crashes.
	TMap<FString, TArray<TPair<int32, int32>>> Bad;
	Bad.Add(TEXT("M.cpp"), { TPair<int32, int32>(9, 2), TPair<int32, int32>(-4, -1) });
	TSharedPtr<FJsonObject> R = T.Db.DetectChanges({ TEXT("M.cpp") }, 200, TEXT("standard"), Bad);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TSharedPtr<FJsonObject>* In = nullptr;
	TestTrue(TEXT("input present"), R->TryGetObjectField(TEXT("input"), In) && In);
	TestEqual(TEXT("malformed ranges -> file-level fallback"), (*In)->GetStringField(TEXT("precision")), FString(TEXT("file")));
	TestEqual(TEXT("file-level symbol count preserved"), (int32)R->GetNumberField(TEXT("changed_entity_count")), 5);
	return true;
}
