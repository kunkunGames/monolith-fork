#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithPaper2DActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithPaper2DAssetRejectsUnsafePathTest, "Monolith.ParamGuard.MonolithPaper2D.AssetRejectsUnsafePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithPaper2DAssetRejectsUnsafePathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithPaper2DActions::RegisterActions(Registry);

	TestTrue(TEXT("paper2d.get_asset should be registered"), Registry.HasAction(TEXT("paper2d"), TEXT("get_asset")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("D:/OutsideProject/PaperSprite.uasset"));

	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("paper2d"), TEXT("get_asset"), Params);
	TestFalse(TEXT("Unsafe filesystem paths should be rejected"), Result.bSuccess);
	TestTrue(TEXT("Error should name the /Game guard"), Result.ErrorMessage.Contains(TEXT("/Game")));

	return true;
}
