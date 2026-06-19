#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithPCGActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardPCGGraphAssetRejectsUnsafePathTest, "Monolith.ParamGuard.MonolithPCG.GraphAssetRejectsUnsafePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardPCGGraphAssetRejectsUnsafePathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	TestTrue(TEXT("pcg.get_graph_asset action is registered"), Registry.HasAction(TEXT("pcg"), TEXT("get_graph_asset")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PCGGraph.uasset"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
	TestFalse(TEXT("GetGraphAsset rejects out-of-project paths"), Result.bSuccess);
	TestTrue(TEXT("GetGraphAsset reports the /Game boundary"), Result.ErrorMessage.Contains(TEXT("/Game")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardPCGGetGraphAssetParamsTest, "Monolith.ParamGuard.MonolithPCG.GetGraphAssetParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardPCGGetGraphAssetParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPCGActions::RegisterActions(Registry);

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("asset_path"), true);
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects boolean asset_path"), Result.bSuccess);
		TestTrue(TEXT("Error mentions string"), Result.ErrorMessage.Contains(TEXT("string")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/PCG"));
		Params->SetStringField(TEXT("include_tags"), TEXT("true"));
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects string include_tags"), Result.bSuccess);
		TestTrue(TEXT("Error mentions boolean"), Result.ErrorMessage.Contains(TEXT("boolean")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/PCG"));
		Params->SetStringField(TEXT("tag_limit"), TEXT("50"));
		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("pcg"), TEXT("get_graph_asset"), Params);
		TestFalse(TEXT("Rejects string tag_limit"), Result.bSuccess);
		TestTrue(TEXT("Error mentions number"), Result.ErrorMessage.Contains(TEXT("number")));
	}

	return true;
}
