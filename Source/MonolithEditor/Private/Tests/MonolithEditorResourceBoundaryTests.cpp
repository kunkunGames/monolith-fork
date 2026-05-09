#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorDeleteAssetsRejectsOversizedArray, "Monolith.LimitGuard.MonolithEditor.DeleteAssetsRejectsOversizedArray", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorDeleteAssetsRejectsOversizedArray::RunTest(const FString& Parameters)
{
	// Create params with > 200 items
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetPaths;
	for (int32 i = 0; i < 201; ++i)
	{
		AssetPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Test/Asset")));
	}
	Params->SetArrayField(TEXT("asset_paths"), AssetPaths);

	// Dispatch
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("delete_assets"), Params);

	TestFalse(TEXT("Should fail on oversized array"), Result.bSuccess);
	TestTrue(TEXT("Should complain about exceeding maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}
