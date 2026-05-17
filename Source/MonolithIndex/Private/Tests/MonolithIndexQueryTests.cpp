#include "Misc/AutomationTest.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexReview.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectFindByTypeClampsLimitTest, "Monolith.IndexGuard.Project.FindByTypeClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectFindByTypeClampsLimitTest::RunTest(const FString& Parameters)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
	Params->SetNumberField(TEXT("limit"), 50000); // Exceeds clamp
	Params->SetNumberField(TEXT("offset"), -10);   // Below 0

	FMonolithActionResult Result = FProjectFindByTypeAction::Execute(Params);

	if (Result.bSuccess && Result.Result.IsValid())
	{
		int32 RetLimit = Result.Result->GetIntegerField(TEXT("limit"));
		int32 RetOffset = Result.Result->GetIntegerField(TEXT("offset"));

		TestEqual(TEXT("Limit is clamped to 1000"), RetLimit, 1000);
		TestEqual(TEXT("Offset is clamped to 0"), RetOffset, 0);
	}

	return true;
}

// ============================================================================
// CRG-inspired navigation/review tests (FMonolithIndexReview over a temp DB)
// ============================================================================
namespace
{
	struct FTempIndexDb
	{
		FMonolithIndexDatabase Db;
		FString Path;
		int64 A = 0, B = 0, C = 0, D = 0, E = 0;

		bool Build()
		{
			Path = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithIdxReview"), TEXT(".sqlite"));
			if (!Db.Open(Path)) return false;
			auto Mk = [&](const TCHAR* P, const TCHAR* Cls) -> int64
			{
				FIndexedAsset Asset;
				Asset.PackagePath = P;
				Asset.AssetName = FPaths::GetCleanFilename(P);
				Asset.AssetClass = Cls;
				return Db.InsertAsset(Asset);
			};
			A = Mk(TEXT("/Game/A"), TEXT("Blueprint"));
			B = Mk(TEXT("/Game/B"), TEXT("Blueprint"));
			C = Mk(TEXT("/Game/C"), TEXT("WidgetBlueprint"));
			D = Mk(TEXT("/Game/D"), TEXT("Material"));
			E = Mk(TEXT("/Game/Systems/SaveSession"), TEXT("Blueprint"));
			auto Dep = [&](int64 S, int64 T, const TCHAR* K)
			{
				FIndexedDependency Dp; Dp.SourceAssetId = S; Dp.TargetAssetId = T; Dp.DependencyType = K;
				Db.InsertDependency(Dp);
			};
			// A -> B -> C -> A (cycle) and B -> D
			Dep(A, B, TEXT("Hard"));
			Dep(B, C, TEXT("Hard"));
			Dep(C, A, TEXT("Soft"));
			Dep(B, D, TEXT("Hard"));
			for (int32 Index = 0; Index < 120; ++Index)
			{
				FIndexedNode Node;
				Node.AssetId = E;
				Node.NodeType = TEXT("Function");
				Node.NodeName = FString::Printf(TEXT("SaveNode%d"), Index);
				Node.NodeClass = TEXT("K2Node_CallFunction");
				Db.InsertNode(Node);
			}
			Db.WriteMeta(TEXT("schema_version"), TEXT("2"));
			TSharedPtr<FJsonObject> Crg = FMonolithIndexReview::RepairCrgCache(Db, true);
			return A > 0 && B > 0 && C > 0 && D > 0 && E > 0
				&& Crg.IsValid() && Crg->GetStringField(TEXT("status")) == TEXT("ok");
		}
		~FTempIndexDb()
		{
			Db.Close();
			if (!Path.IsEmpty()) FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*Path);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectImpactRadiusCycleSafeTest, "Monolith.IndexGuard.Project.ImpactRadiusCycleSafe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectImpactRadiusCycleSafeTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());

	// Depth 5 over a 3-node cycle must terminate and not exceed the node set.
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::ImpactRadius(T.Db, TEXT("/Game/B"), TEXT("both"), 5, 200, FString());
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Imp = nullptr;
	TestTrue(TEXT("impacted_assets present"), R->TryGetArrayField(TEXT("impacted_assets"), Imp) && Imp != nullptr);
	// A, C, D reachable from B; cycle back to B must not re-emit B.
	TestTrue(TEXT("cycle-safe finite result (<=3 unique impacted)"), Imp->Num() >= 1 && Imp->Num() <= 3);
	TestFalse(TEXT("not truncated at 200"), R->GetBoolField(TEXT("truncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectImpactRadiusTruncatesTest, "Monolith.IndexGuard.Project.ImpactRadiusTruncates", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectImpactRadiusTruncatesTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::ImpactRadius(T.Db, TEXT("/Game/B"), TEXT("both"), 5, 1, FString());
	TestTrue(TEXT("truncated when max_results=1"), R->GetBoolField(TEXT("truncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectHealthHealthyTest, "Monolith.IndexGuard.Project.HealthHealthy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectHealthHealthyTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::Health(T.Db, true);
	TestEqual(TEXT("fresh consistent DB is healthy"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings array present"), R->TryGetArrayField(TEXT("warnings"), W) && W != nullptr);
	TestEqual(TEXT("no health warnings"), W->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectHealthWarnsOnOrphanDependencyTest, "Monolith.IndexGuard.Project.HealthWarnsOnOrphanDependency", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectHealthWarnsOnOrphanDependencyTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	FIndexedDependency Orphan;
	Orphan.SourceAssetId = T.B;
	Orphan.TargetAssetId = 999999;
	Orphan.DependencyType = TEXT("Hard");
	FSQLiteDatabase* Raw = T.Db.GetRawDatabase();
	TestTrue(TEXT("raw DB available"), Raw != nullptr);
	TestTrue(TEXT("foreign keys disabled for corrupt fixture"), Raw && Raw->Execute(TEXT("PRAGMA foreign_keys=OFF;")));
	TestTrue(TEXT("orphan dependency inserted"), T.Db.InsertDependency(Orphan) > 0);
	TestTrue(TEXT("foreign keys re-enabled after corrupt fixture"), Raw && Raw->Execute(TEXT("PRAGMA foreign_keys=ON;")));

	TSharedPtr<FJsonObject> R = FMonolithIndexReview::Health(T.Db, false);
	TestEqual(TEXT("orphan dependency yields warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	const TArray<TSharedPtr<FJsonValue>>* W = nullptr;
	TestTrue(TEXT("warnings array present"), R->TryGetArrayField(TEXT("warnings"), W) && W && W->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRepairFtsDryRunTest, "Monolith.IndexGuard.Project.RepairFtsDryRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRepairFtsDryRunTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> Dry = FMonolithIndexReview::RepairFts(T.Db, TEXT("all"), false);
	TestEqual(TEXT("dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
	TestTrue(TEXT("plan present"), Dry->TryGetArrayField(TEXT("plan"), Plan) && Plan && Plan->Num() == 2);
	TSharedPtr<FJsonObject> Exec = FMonolithIndexReview::RepairFts(T.Db, TEXT("all"), true);
	TestEqual(TEXT("execute ok"), Exec->GetStringField(TEXT("status")), FString(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRepairCrgCacheTest, "Monolith.IndexGuard.Project.RepairCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRepairCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> Dry = FMonolithIndexReview::RepairCrgCache(T.Db, false);
	TestEqual(TEXT("dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Plan = nullptr;
	TestTrue(TEXT("plan present"), Dry->TryGetArrayField(TEXT("plan"), Plan) && Plan && Plan->Num() >= 3);

	TSharedPtr<FJsonObject> Exec = FMonolithIndexReview::RepairCrgCache(T.Db, true);
	TestEqual(TEXT("execute ok"), Exec->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> After = Exec->GetObjectField(TEXT("after"));
	TestTrue(TEXT("after counts present"), After.IsValid());
	TestEqual(TEXT("one CRG node per asset"), After->GetIntegerField(TEXT("crg_nodes")), 5);
	TestEqual(TEXT("one CRG edge per dependency"), After->GetIntegerField(TEXT("crg_edges")), 4);
	TestEqual(TEXT("one metric per CRG node"), After->GetIntegerField(TEXT("crg_node_metrics")), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRiskScoreUsesCrgCacheTest, "Monolith.IndexGuard.Project.RiskScoreUsesCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRiskScoreUsesCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::RiskScore(T.Db, TEXT("/Game/B"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
	TestTrue(TEXT("item object"), Item.IsValid());
	TSharedPtr<FJsonObject> Cache = Item->GetObjectField(TEXT("cache"));
	TestTrue(TEXT("cache object"), Cache.IsValid());
	TestEqual(TEXT("cache hit"), Cache->GetStringField(TEXT("status")), FString(TEXT("hit")));
	double Score = 0.0;
	TestTrue(TEXT("risk score present"), Item->TryGetNumberField(TEXT("score"), Score));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRiskScoreSensitivityTest, "Monolith.IndexGuard.Project.RiskScoreSensitivity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRiskScoreSensitivityTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::RiskScore(T.Db, TEXT("/Game/Systems/SaveSession"), 10, TEXT("low"));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectDetectChangesMinimalTest, "Monolith.IndexGuard.Project.DetectChangesMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectDetectChangesMinimalTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::DetectChanges(T.Db, { TEXT("Content/A.uasset") }, 5, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("one changed asset"), R->GetIntegerField(TEXT("changed_entity_count")), 1);
	TestEqual(TEXT("one direct impacted referencer"), R->GetIntegerField(TEXT("impacted_count")), 1);
	TestEqual(TEXT("no project test gaps"), R->GetIntegerField(TEXT("test_gap_count")), 0);
	TestFalse(TEXT("minimal omits full changed_entities"), R->HasField(TEXT("changed_entities")));
	const TArray<TSharedPtr<FJsonValue>>* Priorities = nullptr;
	TestTrue(TEXT("priorities present"), R->TryGetArrayField(TEXT("review_priorities"), Priorities) && Priorities && Priorities->Num() == 1);
	TestEqual(TEXT("priority names changed asset"), (*Priorities)[0]->AsString(), FString(TEXT("A")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectDetectChangesStandardTest, "Monolith.IndexGuard.Project.DetectChangesStandard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectDetectChangesStandardTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::DetectChanges(T.Db, { TEXT("/Game/B.umap") }, 5, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 1);
	TSharedPtr<FJsonObject> First = (*Changed)[0]->AsObject();
	TestTrue(TEXT("first changed object"), First.IsValid());
	TestEqual(TEXT("asset name B"), First->GetStringField(TEXT("asset_name")), FString(TEXT("B")));
	TSharedPtr<FJsonObject> Impact = R->GetObjectField(TEXT("impact"));
	TestTrue(TEXT("impact present"), Impact.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* Impacted = nullptr;
	TestTrue(TEXT("standard impact has impacted_entities"), Impact->TryGetArrayField(TEXT("impacted_entities"), Impacted) && Impacted);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectDetectChangesEscapesPathWildcardsTest, "Monolith.IndexGuard.Project.DetectChangesEscapesPathWildcards", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectDetectChangesEscapesPathWildcardsTest::RunTest(const FString& Parameters)
{
	FMonolithIndexDatabase Db;
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithIdxDetectWildcards"), TEXT(".sqlite"));
	TestTrue(TEXT("temporary DB opens"), Db.Open(DbPath));

	auto Mk = [&](const TCHAR* PackagePath) -> int64
	{
		FIndexedAsset Asset;
		Asset.PackagePath = PackagePath;
		Asset.AssetName = FPaths::GetCleanFilename(PackagePath);
		Asset.AssetClass = TEXT("Blueprint");
		return Db.InsertAsset(Asset);
	};
	const int64 UnderId = Mk(TEXT("/Game/BP_Player_Controller"));
	const int64 PlainId = Mk(TEXT("/Game/BP_PlayerXController"));
	TestTrue(TEXT("wildcard fixture inserted"), UnderId > 0 && PlainId > 0);

	TSharedPtr<FJsonObject> R = FMonolithIndexReview::DetectChanges(Db, { TEXT("Content/BP_Player_Controller.uasset") }, 10, TEXT("standard"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("underscore asset path is treated literally"), R->GetIntegerField(TEXT("changed_entity_count")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Changed = nullptr;
	TestTrue(TEXT("changed_entities present"), R->TryGetArrayField(TEXT("changed_entities"), Changed) && Changed && Changed->Num() == 1);
	if (Changed && Changed->Num() == 1)
	{
		TSharedPtr<FJsonObject> First = (*Changed)[0]->AsObject();
		TestTrue(TEXT("first changed object"), First.IsValid());
		if (First.IsValid())
		{
			TestEqual(TEXT("literal underscore asset matched"), First->GetStringField(TEXT("asset_path")), FString(TEXT("/Game/BP_Player_Controller")));
			TestEqual(TEXT("overmatching asset excluded"), First->GetStringField(TEXT("asset_name")), FString(TEXT("BP_Player_Controller")));
		}
	}

	Db.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectPreMergeCheckPassTest, "Monolith.IndexGuard.Project.PreMergeCheckPass", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectPreMergeCheckPassTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::PreMergeCheck(T.Db, { TEXT("Content/A.uasset") }, 5, 5, TEXT("minimal"), false);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("decision pass"), R->GetStringField(TEXT("decision")), FString(TEXT("pass")));
	TestEqual(TEXT("one changed asset"), R->GetIntegerField(TEXT("changed_entity_count")), 1);
	TestEqual(TEXT("no sampled unused candidates"), R->GetIntegerField(TEXT("unused_count")), 0);
	TestFalse(TEXT("minimal omits nested change analysis"), R->HasField(TEXT("change_analysis")));
	const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
	TestTrue(TEXT("checks present"), R->TryGetArrayField(TEXT("checks"), Checks) && Checks && Checks->Num() >= 2);
	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	TestTrue(TEXT("findings present"), R->TryGetArrayField(TEXT("findings"), Findings) && Findings && Findings->Num() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectPreMergeCheckWarnsOnUnusedSampleTest, "Monolith.IndexGuard.Project.PreMergeCheckWarnsOnUnusedSample", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectPreMergeCheckWarnsOnUnusedSampleTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::PreMergeCheck(T.Db, { TEXT("/Game/Systems/SaveSession.uasset") }, 5, 5, TEXT("standard"), true);
	TestEqual(TEXT("warning status"), R->GetStringField(TEXT("status")), FString(TEXT("warning")));
	TestEqual(TEXT("decision warn"), R->GetStringField(TEXT("decision")), FString(TEXT("warn")));
	TestTrue(TEXT("unused sample surfaced"), R->GetIntegerField(TEXT("unused_count")) >= 1);
	TestTrue(TEXT("standard includes health payload"), R->HasField(TEXT("health")));
	TestTrue(TEXT("standard includes change analysis"), R->HasField(TEXT("change_analysis")));
	TestTrue(TEXT("standard includes unused payload"), R->HasField(TEXT("unused")));
	const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
	TestTrue(TEXT("warning finding present"), R->TryGetArrayField(TEXT("findings"), Findings) && Findings && Findings->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectReviewHotspotsLargeTest, "Monolith.IndexGuard.Project.ReviewHotspotsLarge", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectReviewHotspotsLargeTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::ReviewHotspots(T.Db, TEXT("large"), 5, 100, true);
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Hotspots = nullptr;
	TestTrue(TEXT("hotspots present"), R->TryGetArrayField(TEXT("hotspots"), Hotspots) && Hotspots && Hotspots->Num() >= 1);
	TSharedPtr<FJsonObject> First = (*Hotspots)[0]->AsObject();
	TestTrue(TEXT("first hotspot object"), First.IsValid());
	TestEqual(TEXT("large hotspot picks SaveSession"), First->GetStringField(TEXT("asset_path")), FString(TEXT("/Game/Systems/SaveSession")));
	TestTrue(TEXT("signals field present"), First->HasField(TEXT("signals")));
	TSharedPtr<FJsonObject> Signals = First->GetObjectField(TEXT("signals"));
	TestTrue(TEXT("signals object valid"), Signals.IsValid());
	TestTrue(TEXT("signals include size"), Signals->HasField(TEXT("size_signal")));
	const TArray<TSharedPtr<FJsonValue>>* Questions = nullptr;
	TestTrue(TEXT("questions present"), R->TryGetArrayField(TEXT("questions"), Questions) && Questions && Questions->Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectFindUnusedAdvisoryTest, "Monolith.IndexGuard.Project.FindUnusedAdvisory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectFindUnusedAdvisoryTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::FindUnused(T.Db, TEXT("Blueprint"), 5, TEXT("medium"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("items present"), R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	TSharedPtr<FJsonObject> First = (*Items)[0]->AsObject();
	TestTrue(TEXT("first candidate object"), First.IsValid());
	TestEqual(TEXT("unused candidate is unreferenced Blueprint"), First->GetStringField(TEXT("asset_path")), FString(TEXT("/Game/Systems/SaveSession")));
	TestEqual(TEXT("unused candidate is medium confidence"), First->GetStringField(TEXT("confidence")), FString(TEXT("medium")));
	const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
	TestTrue(TEXT("reasons present"), First->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons && Reasons->Num() >= 2);

	TSharedPtr<FJsonObject> High = FMonolithIndexReview::FindUnused(T.Db, TEXT("Blueprint"), 5, TEXT("high"));
	const TArray<TSharedPtr<FJsonValue>>* HighItems = nullptr;
	TestTrue(TEXT("high-confidence filter returns array"), High->TryGetArrayField(TEXT("items"), HighItems) && HighItems != nullptr);
	TestEqual(TEXT("find_unused never reports high confidence"), HighItems->Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectReviewContextMinimalTest, "Monolith.IndexGuard.Project.ReviewContextMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectReviewContextMinimalTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::ReviewContext(T.Db, TEXT("/Game/B"), TEXT("both"), 2, 200, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("has risk"), R->HasField(TEXT("risk")));
	TestTrue(TEXT("has impact"), R->HasField(TEXT("impact")));
	TestTrue(TEXT("has limits"), R->HasField(TEXT("limits")));
	TestTrue(TEXT("has top risks"), R->HasField(TEXT("top_risks")));
	TestTrue(TEXT("has compact context"), R->HasField(TEXT("context")));
	TestTrue(TEXT("has next_actions"), R->HasField(TEXT("next_actions")));
	TestFalse(TEXT("minimal omits full details"), R->HasField(TEXT("details")));
	return true;
}
