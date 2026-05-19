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
