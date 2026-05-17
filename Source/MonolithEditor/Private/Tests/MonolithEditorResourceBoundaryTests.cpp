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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorSearchBuildOutputClampsLimit, "Monolith.LimitGuard.MonolithEditor.SearchBuildOutputClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSearchBuildOutputClampsLimit::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("pattern"), TEXT("TestPattern"));
	Params->SetNumberField(TEXT("limit"), 10000.0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("search_build_output"), Params);

	TestTrue(TEXT("Should succeed since it executes the action"), Result.bSuccess);

	// Test that an invalid type string returns an error
	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("pattern"), TEXT("TestPattern"));
	InvalidParams->SetStringField(TEXT("limit"), TEXT("10000"));

	FMonolithActionResult InvalidResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("search_build_output"), InvalidParams);
	TestFalse(TEXT("Should fail on string limit type"), InvalidResult.bSuccess);
	TestTrue(TEXT("Should return an invalid param error"), InvalidResult.ErrorMessage.Contains(TEXT("Invalid param")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorRunAutomationTestsClampsLimit, "Monolith.LimitGuard.MonolithEditor.RunAutomationTestsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorRunAutomationTestsClampsLimit::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("prefix"), TEXT("Dummy.Prefix.That.Does.Not.Exist"));
	Params->SetNumberField(TEXT("max_tests"), 10000.0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("run_automation_tests"), Params);

	TestTrue(TEXT("Should succeed since it executes the action and returns 0 matching tests"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		TestTrue(TEXT("Result JSON should contain the clamped max_tests field"), Result.Result->HasField(TEXT("max_tests")));
		TestEqual(TEXT("Should clamp max_tests to 1000"), Result.Result->GetNumberField(TEXT("max_tests")), 1000.0);
	}
	else
	{
		AddError(TEXT("Result JSON object is invalid"));
	}

	return true;
}
