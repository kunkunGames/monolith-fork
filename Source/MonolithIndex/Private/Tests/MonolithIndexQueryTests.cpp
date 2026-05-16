#include "Misc/AutomationTest.h"
#include "Actions/ProjectFindByTypeAction.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexReview.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
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
		int64 A = 0, B = 0, C = 0, D = 0;

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
			Db.WriteMeta(TEXT("schema_version"), TEXT("2"));
			return A > 0 && B > 0 && C > 0 && D > 0;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectReviewContextMinimalTest, "Monolith.IndexGuard.Project.ReviewContextMinimal", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProjectReviewContextMinimalTest::RunTest(const FString& Parameters)
{
	FTempIndexDb T;
	TestTrue(TEXT("temp index db built"), T.Build());
	TSharedPtr<FJsonObject> R = FMonolithIndexReview::ReviewContext(T.Db, TEXT("/Game/B"), TEXT("both"), 2, 200, TEXT("minimal"));
	TestEqual(TEXT("status ok"), R->GetStringField(TEXT("status")), FString(TEXT("ok")));
	TestTrue(TEXT("has risk"), R->HasField(TEXT("risk")));
	TestTrue(TEXT("has impact"), R->HasField(TEXT("impact")));
	TestTrue(TEXT("has next_actions"), R->HasField(TEXT("next_actions")));
	TestFalse(TEXT("minimal omits full details"), R->HasField(TEXT("details")));
	return true;
}

