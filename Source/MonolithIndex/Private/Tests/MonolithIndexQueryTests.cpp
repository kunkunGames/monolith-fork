#include "Misc/AutomationTest.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "MonolithAsyncJobRegistry.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexReview.h"
#include "MonolithIndexSubsystem.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"


#include "Actions/ProjectSearchAction.h"
#include "Actions/ProjectSearchGameplayTagsAction.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectSearchTagsOffsetRejectsWrongTypeTest, "Monolith.IndexGuard.Project.SearchTagsOffsetRejectsWrongType", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectSearchTagsOffsetRejectsWrongTypeTest::RunTest(const FString& Parameters)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT("Weapon"));
	Params->SetStringField(TEXT("offset"), TEXT("NotANumber")); // Malformed input

	FMonolithActionResult Result = FProjectSearchGameplayTagsAction::Execute(Params);

	TestFalse(TEXT("Search tags action should reject wrong offset type"), Result.bSuccess);
	TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectSearchHandlesEmptyQueryTest, "Monolith.IndexGuard.Project.SearchHandlesEmptyQuery", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectSearchHandlesEmptyQueryTest::RunTest(const FString& Parameters)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("query"), TEXT(""));

	FMonolithActionResult Result = FProjectSearchAction::Execute(Params);

	TestFalse(TEXT("Search action should reject empty query"), Result.bSuccess);
	TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);

	return true;
}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexAsyncJobReflectedStartFunctionsTest, "Monolith.IndexGuard.Project.AsyncJobReflectedStartFunctions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexAsyncJobReflectedStartFunctionsTest::RunTest(const FString& Parameters)
{
	UClass* IndexSubsystemClass = UMonolithIndexSubsystem::StaticClass();
	TestNotNull(TEXT("Full async start function is reflected"),
		IndexSubsystemClass->FindFunctionByName(TEXT("StartFullIndexWithAsyncJob")));
	TestNotNull(TEXT("Incremental async start function is reflected"),
		IndexSubsystemClass->FindFunctionByName(TEXT("StartIncrementalIndexWithAsyncJob")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexAsyncJobCompletesTest, "Monolith.IndexGuard.Project.AsyncJobCompletes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexAsyncJobCompletesTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	UMonolithIndexSubsystem* Subsystem = NewObject<UMonolithIndexSubsystem>();
	TestNotNull(TEXT("Index subsystem test object created"), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const FString JobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	Subsystem->SetActiveAsyncJobForTests(JobId, TEXT("full"));
	Subsystem->CompleteActiveAsyncJobForTests(true);

	TSharedPtr<FJsonObject> Job = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Completed job status"), Job->GetStringField(TEXT("status")), TEXT("completed"));
	TestEqual(TEXT("Completed job progress"), Job->GetObjectField(TEXT("progress"))->GetNumberField(TEXT("percent")), 100.0);
	TestTrue(TEXT("Completed job has result"), Job->HasTypedField<EJson::Object>(TEXT("result")));
	if (Job->HasTypedField<EJson::Object>(TEXT("result")))
	{
		TestEqual(TEXT("Result records full mode"), Job->GetObjectField(TEXT("result"))->GetStringField(TEXT("index_mode")), TEXT("full"));
	}

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexAsyncJobFailsTest, "Monolith.IndexGuard.Project.AsyncJobFails", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexAsyncJobFailsTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	UMonolithIndexSubsystem* Subsystem = NewObject<UMonolithIndexSubsystem>();
	TestNotNull(TEXT("Index subsystem test object created"), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const FString JobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	Subsystem->SetActiveAsyncJobForTests(JobId, TEXT("incremental"));
	Subsystem->CompleteActiveAsyncJobForTests(false);

	TSharedPtr<FJsonObject> Job = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Failed job status"), Job->GetStringField(TEXT("status")), TEXT("failed"));
	TestTrue(TEXT("Failed job records error"), Job->HasTypedField<EJson::String>(TEXT("error")));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexAsyncJobReflectedStartFailsWithoutDatabaseTest, "Monolith.IndexGuard.Project.AsyncJobReflectedStartFailsWithoutDatabase", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexAsyncJobReflectedStartFailsWithoutDatabaseTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	UMonolithIndexSubsystem* Subsystem = NewObject<UMonolithIndexSubsystem>();
	TestNotNull(TEXT("Index subsystem test object created"), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	UFunction* FullFunc = UMonolithIndexSubsystem::StaticClass()->FindFunctionByName(TEXT("StartFullIndexWithAsyncJob"));
	TestNotNull(TEXT("Full async start function exists"), FullFunc);
	if (!FullFunc)
	{
		Registry.ResetForTests();
		return false;
	}

	struct FStartIndexWithAsyncJobParams
	{
		FString JobId;
		bool ReturnValue = true;
	};

	const FString FullJobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	FStartIndexWithAsyncJobParams FullParams;
	FullParams.JobId = FullJobId;
	Subsystem->ProcessEvent(FullFunc, &FullParams);

	TestFalse(TEXT("Full reflected start reports failure without an open database"), FullParams.ReturnValue);
	TSharedPtr<FJsonObject> FullJob = Registry.GetJobJson(FullJobId);
	TestEqual(TEXT("Full reflected start fails the job row"), FullJob->GetStringField(TEXT("status")), TEXT("failed"));

	UFunction* IncrementalFunc = UMonolithIndexSubsystem::StaticClass()->FindFunctionByName(TEXT("StartIncrementalIndexWithAsyncJob"));
	TestNotNull(TEXT("Incremental async start function exists"), IncrementalFunc);
	if (!IncrementalFunc)
	{
		Registry.ResetForTests();
		return false;
	}

	const FString IncrementalJobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	FStartIndexWithAsyncJobParams IncrementalParams;
	IncrementalParams.JobId = IncrementalJobId;
	Subsystem->ProcessEvent(IncrementalFunc, &IncrementalParams);

	TestFalse(TEXT("Incremental reflected start reports failure without an open database"), IncrementalParams.ReturnValue);
	TSharedPtr<FJsonObject> IncrementalJob = Registry.GetJobJson(IncrementalJobId);
	TestEqual(TEXT("Incremental reflected start fails the job row"), IncrementalJob->GetStringField(TEXT("status")), TEXT("failed"));

	Registry.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectIndexAsyncJobDoesNotOverwriteCancelledTest, "Monolith.IndexGuard.Project.AsyncJobDoesNotOverwriteCancelled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProjectIndexAsyncJobDoesNotOverwriteCancelledTest::RunTest(const FString& Parameters)
{
	FMonolithAsyncJobRegistry& Registry = FMonolithAsyncJobRegistry::Get();
	Registry.ResetForTests();

	UMonolithIndexSubsystem* Subsystem = NewObject<UMonolithIndexSubsystem>();
	TestNotNull(TEXT("Index subsystem test object created"), Subsystem);
	if (!Subsystem)
	{
		return false;
	}

	const FString JobId = Registry.SubmitJob(TEXT("project"), TEXT("reindex"));
	Subsystem->SetActiveAsyncJobForTests(JobId, TEXT("full"));
	Registry.RequestCancel(JobId);
	Subsystem->CompleteActiveAsyncJobForTests(true);

	TSharedPtr<FJsonObject> Job = Registry.GetJobJson(JobId);
	TestEqual(TEXT("Cancelled job stays cancelled after late success"), Job->GetStringField(TEXT("status")), TEXT("cancelled"));
	TestFalse(TEXT("Cancelled job has no late success result"), Job->HasField(TEXT("result")));

	Registry.ResetForTests();
	return true;
}

// ============================================================================
// CRG-inspired navigation/review tests (FMonolithIndexReview over a temp DB)
// ============================================================================
namespace
{
	int64 CountProjectRows(FMonolithIndexDatabase& Db, const TCHAR* Sql)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw)
		{
			return -1;
		}
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, Sql))
		{
			return -1;
		}
		int64 Count = -1;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return Count;
	}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectHealthWarnsOnStaleCrgCacheTest, "Monolith.IndexGuard.Project.HealthWarnsOnStaleCrgCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectHealthWarnsOnStaleCrgCacheTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/StaleCrgCacheProbe");
	Asset.AssetName = TEXT("StaleCrgCacheProbe");
	Asset.AssetClass = TEXT("Blueprint");
	TestTrue(TEXT("asset inserted after CRG rebuild"), T.Db.InsertAsset(Asset) > 0);

	TSharedPtr<FJsonObject> Stale = FMonolithIndexReview::Health(T.Db, true);
	TestEqual(TEXT("stale CRG projection yields warning status"), Stale->GetStringField(TEXT("status")), FString(TEXT("warning")));
	const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
	TestTrue(TEXT("stale CRG warning present"), Stale->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings && Warnings->Num() >= 1);

	TSharedPtr<FJsonObject> Repair = FMonolithIndexReview::RepairCrgCache(T.Db, true);
	TestEqual(TEXT("stale CRG repair ok"), Repair->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> Clean = FMonolithIndexReview::Health(T.Db, true);
	TestEqual(TEXT("health is clean after repair"), Clean->GetStringField(TEXT("status")), FString(TEXT("ok")));
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
	TestTrue(TEXT("plan present"), Dry->TryGetArrayField(TEXT("plan"), Plan) && Plan && Plan->Num() == 7);
	TSharedPtr<FJsonObject> Exec = FMonolithIndexReview::RepairFts(T.Db, TEXT("all"), true);
	TestEqual(TEXT("execute ok"), Exec->GetStringField(TEXT("status")), FString(TEXT("ok")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectSearchContentInclusiveFtsTest, "Monolith.IndexGuard.Project.SearchContentInclusiveFts", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectSearchContentInclusiveFtsTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());

	const FString Probe = TEXT("SearchProbeAlpha");

	FIndexedVariable Var;
	Var.AssetId = T.A;
	Var.VarName = TEXT("CooldownSeconds");
	Var.VarType = TEXT("float");
	Var.DefaultValue = Probe;
	TestTrue(TEXT("variable inserted"), T.Db.InsertVariable(Var) > 0);

	FIndexedParameter Param;
	Param.AssetId = T.D;
	Param.ParamName = TEXT("EmissiveLabel");
	Param.ParamType = TEXT("Scalar");
	Param.DefaultValue = Probe;
	Param.Source = TEXT("Material");
	TestTrue(TEXT("parameter inserted"), T.Db.InsertParameter(Param) > 0);

	FIndexedDataTableRow Row;
	Row.AssetId = T.C;
	Row.RowName = Probe;
	Row.RowData = TEXT("{}");
	TestTrue(TEXT("datatable row inserted"), T.Db.InsertDataTableRow(Row) > 0);

	FIndexedActor Actor;
	Actor.AssetId = T.B;
	Actor.ActorName = TEXT("BP_SearchProbeActor_C_0");
	Actor.ActorClass = TEXT("BP_SearchProbeActor_C");
	Actor.ActorLabel = Probe;
	TestTrue(TEXT("actor inserted"), T.Db.InsertActor(Actor) > 0);

	FIndexedSearchValue SearchValue;
	SearchValue.AssetId = T.E;
	SearchValue.SourceKind = TEXT("blueprint_node_comment");
	SearchValue.ObjectName = TEXT("Node With Comment");
	SearchValue.ObjectPath = TEXT("EventGraph.00000000-0000-0000-0000-000000000001");
	SearchValue.ObjectClass = TEXT("K2Node_CallFunction");
	SearchValue.FieldName = TEXT("comment");
	SearchValue.FieldPath = TEXT("EventGraph.comment");
	SearchValue.ValueText = Probe;
	SearchValue.Signal = TEXT("comment");
	TestTrue(TEXT("supplemental search value inserted"), T.Db.InsertAssetSearchValue(SearchValue) > 0);

	const TArray<FSearchResult> AssetOnly = T.Db.FullTextSearch(Probe, 50, FProjectSearchOptions::AssetNodeOnly());
	TestEqual(TEXT("asset/node-only search does not pick content fields"), AssetOnly.Num(), 0);

	const TArray<FSearchResult> Content = T.Db.FullTextSearch(Probe, 50, FProjectSearchOptions::ContentInclusive());
	TSet<FString> Sources;
	for (const FSearchResult& Result : Content)
	{
		Sources.Add(Result.MatchSource);
	}

	TestTrue(TEXT("variable FTS result present"), Sources.Contains(TEXT("variable")));
	TestTrue(TEXT("parameter FTS result present"), Sources.Contains(TEXT("parameter")));
	TestTrue(TEXT("datatable row FTS result present"), Sources.Contains(TEXT("datatable_row")));
	TestTrue(TEXT("actor FTS result present"), Sources.Contains(TEXT("actor")));
	TestTrue(TEXT("supplemental value FTS result present"), Sources.Contains(TEXT("supplemental_value")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRefreshCrgCacheForAssetsScopedTest, "Monolith.IndexGuard.Project.RefreshCrgCacheForAssetsScoped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRefreshCrgCacheForAssetsScopedTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/ScopedRefresh");
	Asset.AssetName = TEXT("ScopedRefresh");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 NewId = T.Db.InsertAsset(Asset);
	TestTrue(TEXT("new asset inserted after full CRG rebuild"), NewId > 0);

	FIndexedDependency Dep;
	Dep.SourceAssetId = NewId;
	Dep.TargetAssetId = T.B;
	Dep.DependencyType = TEXT("Hard");
	TestTrue(TEXT("new dependency inserted after full CRG rebuild"), T.Db.InsertDependency(Dep) > 0);

	TSet<FString> ChangedPaths;
	ChangedPaths.Add(TEXT("/Game/ScopedRefresh"));
	TSharedPtr<FJsonObject> Refresh = FMonolithIndexReview::RefreshCrgCacheForAssets(
		T.Db,
		ChangedPaths,
		TEXT("automation scoped refresh"));
	TestEqual(TEXT("scoped refresh status ok"), Refresh->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("refresh uses scoped mode"), Refresh->GetStringField(TEXT("refresh_mode")), FString(TEXT("scoped_1hop")));
	TSharedPtr<FJsonObject> Counts = Refresh->GetObjectField(TEXT("counts"));
	TestTrue(TEXT("refresh counts present"), Counts.IsValid());
	if (Counts.IsValid())
	{
		TestEqual(TEXT("one changed path bound"), Counts->GetIntegerField(TEXT("changed_paths")), 1);
		TestTrue(TEXT("seed plus neighbor assets affected"), Counts->GetIntegerField(TEXT("affected_assets")) >= 2);
	}

	TestEqual(TEXT("scoped refresh keeps node parity"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='project';")), static_cast<int64>(6));
	TestEqual(TEXT("scoped refresh keeps edge parity without duplicate affected-endpoint edges"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='project';")), static_cast<int64>(5));
	TestEqual(TEXT("scoped refresh keeps metric parity"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain='project';")), static_cast<int64>(6));

	TSharedPtr<FJsonObject> Health = FMonolithIndexReview::Health(T.Db, true);
	TestEqual(TEXT("health remains clean after scoped refresh"), Health->GetStringField(TEXT("status")), FString(TEXT("ok")));

	TSharedPtr<FJsonObject> Risk = FMonolithIndexReview::RiskScore(T.Db, TEXT("/Game/ScopedRefresh"), 10, TEXT("low"));
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	TestTrue(TEXT("risk item present for scoped refreshed asset"), Risk->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
	if (Items && Items->Num() == 1)
	{
		TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
		TestTrue(TEXT("risk item object"), Item.IsValid());
		if (Item.IsValid())
		{
			TSharedPtr<FJsonObject> Cache = Item->GetObjectField(TEXT("cache"));
			TestTrue(TEXT("scoped refreshed asset reads cached risk"), Cache.IsValid());
			if (Cache.IsValid())
			{
				TestEqual(TEXT("risk cache hit"), Cache->GetStringField(TEXT("status")), FString(TEXT("hit")));
			}
		}
	}

	auto InboundCountFor = [&](const TCHAR* PackagePath) -> double
	{
		TSharedPtr<FJsonObject> R = FMonolithIndexReview::RiskScore(T.Db, PackagePath, 10, TEXT("low"));
		const TArray<TSharedPtr<FJsonValue>>* RiskItems = nullptr;
		if (!R->TryGetArrayField(TEXT("items"), RiskItems) || !RiskItems || RiskItems->Num() != 1)
		{
			return -1.0;
		}
		TSharedPtr<FJsonObject> Item = (*RiskItems)[0]->AsObject();
		if (!Item.IsValid())
		{
			return -1.0;
		}
		TSharedPtr<FJsonObject> Raw = Item->GetObjectField(TEXT("raw_counts"));
		if (!Raw.IsValid())
		{
			return -1.0;
		}
		return Raw->GetNumberField(TEXT("inbound"));
	};
	TestEqual(TEXT("neighbor inbound count includes new dependency before delete"), InboundCountFor(TEXT("/Game/B")), 2.0);

	TestTrue(TEXT("scoped asset deleted from authoritative index"), T.Db.DeleteAssetByPath(TEXT("/Game/ScopedRefresh")));
	TSharedPtr<FJsonObject> DeleteRefresh = FMonolithIndexReview::RefreshCrgCacheForAssets(
		T.Db,
		ChangedPaths,
		TEXT("automation scoped delete refresh"));
	TestEqual(TEXT("delete scoped refresh status ok"), DeleteRefresh->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("delete refresh uses scoped mode"), DeleteRefresh->GetStringField(TEXT("refresh_mode")), FString(TEXT("scoped_1hop")));
	TSharedPtr<FJsonObject> DeleteCounts = DeleteRefresh->GetObjectField(TEXT("counts"));
	TestTrue(TEXT("delete refresh counts present"), DeleteCounts.IsValid());
	if (DeleteCounts.IsValid())
	{
		TestEqual(TEXT("one deleted path bound"), DeleteCounts->GetIntegerField(TEXT("changed_paths")), 1);
		TestTrue(TEXT("old CRG edge neighbor is affected on delete"), DeleteCounts->GetIntegerField(TEXT("affected_assets")) >= 1);
	}
	TestEqual(TEXT("delete scoped refresh restores node parity"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='project';")), static_cast<int64>(5));
	TestEqual(TEXT("delete scoped refresh restores edge parity"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='project';")), static_cast<int64>(4));
	TestEqual(TEXT("delete scoped refresh restores metric parity"), CountProjectRows(T.Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain='project';")), static_cast<int64>(5));
	TSharedPtr<FJsonObject> DeleteHealth = FMonolithIndexReview::Health(T.Db, true);
	TestEqual(TEXT("health remains clean after delete scoped refresh"), DeleteHealth->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestEqual(TEXT("neighbor inbound count is recomputed after delete"), InboundCountFor(TEXT("/Game/B")), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectSnapshotDiffTest, "Monolith.IndexGuard.Project.SnapshotDiff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectSnapshotDiffTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());

	TSharedPtr<FJsonObject> Dry = FMonolithIndexReview::Snapshot(T.Db, TEXT("base"), false);
	TestEqual(TEXT("snapshot dry-run ok"), Dry->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestFalse(TEXT("dry-run does not execute"), Dry->GetBoolField(TEXT("executed")));

	TSharedPtr<FJsonObject> Snap = FMonolithIndexReview::Snapshot(T.Db, TEXT("base"), true);
	TestEqual(TEXT("snapshot execute ok"), Snap->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("snapshot executed"), Snap->GetBoolField(TEXT("executed")));
	TestEqual(TEXT("snapshot captures five project nodes"), Snap->GetIntegerField(TEXT("node_count")), 5);
	TestEqual(TEXT("snapshot captures four project edges"), Snap->GetIntegerField(TEXT("edge_count")), 4);

	FIndexedAsset Asset;
	Asset.PackagePath = TEXT("/Game/NewReviewAsset");
	Asset.AssetName = TEXT("NewReviewAsset");
	Asset.AssetClass = TEXT("Blueprint");
	const int64 NewId = T.Db.InsertAsset(Asset);
	TestTrue(TEXT("new asset inserted"), NewId > 0);
	TSharedPtr<FJsonObject> Rebuilt = FMonolithIndexReview::RepairCrgCache(T.Db, true);
	TestEqual(TEXT("crg cache rebuilt after insert"), Rebuilt->GetStringField(TEXT("status")), FString(TEXT("ok")));

	TSharedPtr<FJsonObject> Diff = FMonolithIndexReview::DiffSnapshots(T.Db, TEXT("base"), TEXT("current"), 10);
	TestEqual(TEXT("diff ok"), Diff->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TSharedPtr<FJsonObject> Counts = Diff->GetObjectField(TEXT("summary_counts"));
	TestTrue(TEXT("summary counts present"), Counts.IsValid());
	TestTrue(TEXT("one or more project nodes added"), Counts->GetIntegerField(TEXT("nodes_added")) >= 1);
	const TArray<TSharedPtr<FJsonValue>>* NewNodes = nullptr;
	TestTrue(TEXT("new_nodes sample present"), Diff->TryGetArrayField(TEXT("new_nodes"), NewNodes) && NewNodes && NewNodes->Num() >= 1);
	TestFalse(TEXT("diff not truncated"), Diff->GetBoolField(TEXT("truncated")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectRiskScoreSensitivityTokenBoundaryTest, "Monolith.IndexGuard.Project.RiskScoreSensitivityTokenBoundary", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectRiskScoreSensitivityTokenBoundaryTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	auto AddAsset = [&](const TCHAR* PackagePath, const TCHAR* AssetClass) -> int64
	{
		FIndexedAsset Asset;
		Asset.PackagePath = PackagePath;
		Asset.AssetName = FPaths::GetCleanFilename(PackagePath);
		Asset.AssetClass = AssetClass;
		return T.Db.InsertAsset(Asset);
	};
	TestTrue(TEXT("Design fixture inserted"),
		AddAsset(TEXT("/Game/Design/DataAsset/WorldTable/DA_World_002_Lobby"), TEXT("DataAsset")) > 0);
	TestTrue(TEXT("Assignment fixture inserted"),
		AddAsset(TEXT("/Game/Systems/AssignmentBoard"), TEXT("Blueprint")) > 0);
	TestTrue(TEXT("Signature fixture inserted"),
		AddAsset(TEXT("/Game/Security/SignatureStore"), TEXT("Blueprint")) > 0);
	TestTrue(TEXT("CryptoHash fixture inserted"),
		AddAsset(TEXT("/Game/Security/CryptoHash"), TEXT("Blueprint")) > 0);

	TSharedPtr<FJsonObject> Rebuilt = FMonolithIndexReview::RepairCrgCache(T.Db, true);
	TestEqual(TEXT("crg cache rebuild ok"), Rebuilt->GetStringField(TEXT("status")), FString(TEXT("ok")));

	auto SensitivityFor = [&](const TCHAR* PackagePath, bool& bHasSensitivityReason) -> double
	{
		bHasSensitivityReason = false;
		TSharedPtr<FJsonObject> R = FMonolithIndexReview::RiskScore(T.Db, PackagePath, 10, TEXT("low"));
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		TestTrue(TEXT("risk items present for token-boundary fixture"),
			R->TryGetArrayField(TEXT("items"), Items) && Items && Items->Num() == 1);
		if (!Items || Items->Num() == 0)
		{
			return -1.0;
		}
		TSharedPtr<FJsonObject> Item = (*Items)[0]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
		if (Item.IsValid() && Item->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons)
		{
			for (const TSharedPtr<FJsonValue>& Reason : *Reasons)
			{
				bHasSensitivityReason = bHasSensitivityReason || Reason->AsString().Contains(TEXT("sensitivity:"));
			}
		}
		TSharedPtr<FJsonObject> Raw;
		if (Item.IsValid())
		{
			Raw = Item->GetObjectField(TEXT("raw_counts"));
		}
		double Sensitivity = -1.0;
		if (Raw.IsValid())
		{
			Raw->TryGetNumberField(TEXT("sensitivity"), Sensitivity);
		}
		return Sensitivity;
	};

	bool bDesignReason = false;
	bool bAssignmentReason = false;
	bool bSignatureReason = false;
	bool bHashReason = false;
	TestEqual(TEXT("Design must not match embedded sign"), SensitivityFor(TEXT("/Game/Design/DataAsset/WorldTable/DA_World_002_Lobby"), bDesignReason), 0.0);
	TestFalse(TEXT("Design emits no sensitivity reason"), bDesignReason);
	TestEqual(TEXT("Assignment must not match embedded sign"), SensitivityFor(TEXT("/Game/Systems/AssignmentBoard"), bAssignmentReason), 0.0);
	TestFalse(TEXT("Assignment emits no sensitivity reason"), bAssignmentReason);
	TestTrue(TEXT("Signature still contributes sensitivity"), SensitivityFor(TEXT("/Game/Security/SignatureStore"), bSignatureReason) > 0.0);
	TestTrue(TEXT("Signature emits sensitivity reason"), bSignatureReason);
	TestTrue(TEXT("CryptoHash still contributes sensitivity"), SensitivityFor(TEXT("/Game/Security/CryptoHash"), bHashReason) > 0.0);
	TestTrue(TEXT("CryptoHash emits sensitivity reason"), bHashReason);
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectEscapeFTSPreservesSafeTokensTest, "Monolith.IndexGuard.Project.EscapeFTSPreservesSafeTokens", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectEscapeFTSPreservesSafeTokensTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Simple word is wrapped with quotes and star"), FMonolithIndexDatabase::EscapeFTS(TEXT("Asset")), TEXT("\"Asset\"*"));
	TestEqual(TEXT("Namespaces are converted to spaces and individually wrapped"), FMonolithIndexDatabase::EscapeFTS(TEXT("Core::System::Logic")), TEXT("\"Core\"* \"System\"* \"Logic\"*"));
	TestEqual(TEXT("Punctuation is replaced with spaces and collapsed"), FMonolithIndexDatabase::EscapeFTS(TEXT("UObject*;[]()")), TEXT("\"UObject\"*"));
	TestEqual(TEXT("Multiple spaces are collapsed"), FMonolithIndexDatabase::EscapeFTS(TEXT("Find   Asset   Data")), TEXT("\"Find\"* \"Asset\"* \"Data\"*"));
	TestEqual(TEXT("Empty or fully stripped string returns quoted empty"), FMonolithIndexDatabase::EscapeFTS(TEXT("!@#$")), TEXT("\"\""));
	TestEqual(TEXT("Path is tokenized by slash separator"), FMonolithIndexDatabase::EscapeFTS(TEXT("/Game/Maps/Interactable/BP_Wave")), TEXT("\"Game\"* \"Maps\"* \"Interactable\"* \"BP_Wave\"*"));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectSearchTagsUsesPreparedLikeTest, "Monolith.IndexGuard.Project.TagsUsesPreparedLike", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectSearchTagsUsesPreparedLikeTest::RunTest(const FString& Parameters)
{
	FMonolithIndexDatabase Db;
	const FString DbPath = FPaths::CreateTempFilename(*FPaths::ProjectIntermediateDir(), TEXT("MonolithIdxTags"), TEXT(".sqlite"));
	TestTrue(TEXT("temporary DB opens"), Db.Open(DbPath));

	FIndexedTag Tag1;
	Tag1.TagName = TEXT("Weapon.Melee_Sword");
	Db.InsertTag(Tag1);

	FIndexedTag Tag2;
	Tag2.TagName = TEXT("Weapon.Melee.Axe");
	Db.InsertTag(Tag2);

	FIndexedTag Tag3;
	Tag3.TagName = TEXT("Damage.Physical_Slash");
	Db.InsertTag(Tag3);

	FSQLiteDatabase* RawDB = Db.GetRawDatabase();
	TestTrue(TEXT("raw db valid"), RawDB != nullptr);

	FSQLitePreparedStatement Stmt;
	TestTrue(TEXT("statement created"), Stmt.Create(*RawDB, TEXT("SELECT tag_name FROM tags WHERE tag_name LIKE ? ESCAPE '\\' ORDER BY tag_name LIMIT ?;")));

	// Test case: Query contains underscore which is a wildcard in SQL
	FString Query = TEXT("Melee_S");
	FString EscapedQuery = Query.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
	FString LikePattern = TEXT("%") + EscapedQuery + TEXT("%");

	Stmt.SetBindingValueByIndex(1, LikePattern);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(10));

	TArray<FString> Results;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString TagName;
		Stmt.GetColumnValueByIndex(0, TagName);
		Results.Add(TagName);
	}

	TestEqual(TEXT("escaped wildcard returns exactly 1 tag"), Results.Num(), 1);
	if (Results.Num() > 0)
	{
		TestEqual(TEXT("matched correct tag"), Results[0], TEXT("Weapon.Melee_Sword"));
	}

	Db.Close();
	FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*DbPath);
	return true;
}
