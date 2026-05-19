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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorCaptureSequenceFramesRejectsOversizedTimestamps, "Monolith.LimitGuard.MonolithEditor.CaptureSequenceFramesRejectsOversizedTimestamps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorCaptureSequenceFramesRejectsOversizedTimestamps::RunTest(const FString& Parameters)
{
	// Create params with > 1000 items
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Test/Asset"));
	Params->SetStringField(TEXT("asset_type"), TEXT("niagara"));

	TArray<TSharedPtr<FJsonValue>> Timestamps;
	for (int32 i = 0; i < 1001; ++i)
	{
		Timestamps.Add(MakeShared<FJsonValueNumber>(1.0));
	}
	Params->SetArrayField(TEXT("timestamps"), Timestamps);

	// Dispatch
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("capture_sequence_frames"), Params);

	TestFalse(TEXT("Should fail on oversized timestamps array"), Result.bSuccess);
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorAutomationStatusShape, "Monolith.Editor.Automation.StatusShape", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorAutomationStatusShape::RunTest(const FString& Parameters)
{
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("get_automation_run_status"), MakeShared<FJsonObject>());

	TestTrue(TEXT("get_automation_run_status should succeed"), Result.bSuccess);
	if (!Result.Result.IsValid())
	{
		AddError(TEXT("Status result JSON object is invalid"));
		return true;
	}

	TestTrue(TEXT("Status exposes active"), Result.Result->HasField(TEXT("active")));
	TestTrue(TEXT("Status exposes can_stop"), Result.Result->HasField(TEXT("can_stop")));
	TestFalse(TEXT("Synchronous runner cannot stop"), Result.Result->GetBoolField(TEXT("can_stop")));
	TestEqual(TEXT("Stop contract is explicit"), Result.Result->GetStringField(TEXT("stop_status")), FString(TEXT("unsupported_cancel")));
	TestTrue(TEXT("Status exposes history capacity"), Result.Result->HasField(TEXT("history_capacity")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorAutomationStopUnsupported, "Monolith.Editor.Automation.StopUnsupported", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorAutomationStopUnsupported::RunTest(const FString& Parameters)
{
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("stop_automation_tests"), MakeShared<FJsonObject>());

	TestTrue(TEXT("stop_automation_tests should return a structured success payload"), Result.bSuccess);
	if (!Result.Result.IsValid())
	{
		AddError(TEXT("Stop result JSON object is invalid"));
		return true;
	}

	TestFalse(TEXT("No in-flight run is stopped"), Result.Result->GetBoolField(TEXT("stopped")));
	TestFalse(TEXT("Synchronous runner cannot stop"), Result.Result->GetBoolField(TEXT("can_stop")));
	TestEqual(TEXT("Stop status reports unsupported_cancel"), Result.Result->GetStringField(TEXT("stop_status")), FString(TEXT("unsupported_cancel")));

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorAutomationHistoryNoMatchRun, "Monolith.Editor.Automation.HistoryNoMatchRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorAutomationHistoryNoMatchRun::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> RunParams = MakeShared<FJsonObject>();
	RunParams->SetStringField(TEXT("prefix"), TEXT("Monolith.Editor.Automation.DoesNotExist"));
	RunParams->SetNumberField(TEXT("max_tests"), 1.0);

	FMonolithActionResult RunResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("run_automation_tests"), RunParams);
	TestTrue(TEXT("No-match automation run should return a structured result"), RunResult.bSuccess);
	if (!RunResult.Result.IsValid())
	{
		AddError(TEXT("Run result JSON object is invalid"));
		return true;
	}

	const FString RunId = RunResult.Result->GetStringField(TEXT("run_id"));
	TestFalse(TEXT("Run id should not be empty"), RunId.IsEmpty());
	TestEqual(TEXT("No-match run state completes"), RunResult.Result->GetStringField(TEXT("state")), FString(TEXT("completed")));
	TestEqual(TEXT("No-match completion reason is explicit"), RunResult.Result->GetStringField(TEXT("completion_reason")), FString(TEXT("no_matching_tests")));
	TestEqual(TEXT("No-match run progress is complete"), RunResult.Result->GetNumberField(TEXT("progress")), 1.0);

	TSharedPtr<FJsonObject> HistoryParams = MakeShared<FJsonObject>();
	HistoryParams->SetNumberField(TEXT("max_results"), 1.0);
	FMonolithActionResult HistoryResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("list_automation_history"), HistoryParams);
	TestTrue(TEXT("list_automation_history should succeed"), HistoryResult.bSuccess);
	if (!HistoryResult.Result.IsValid())
	{
		AddError(TEXT("History result JSON object is invalid"));
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* Runs = nullptr;
	if (!HistoryResult.Result->TryGetArrayField(TEXT("runs"), Runs) || Runs->Num() == 0)
	{
		AddError(TEXT("History should include the just-recorded no-match run"));
		return true;
	}

	const TSharedPtr<FJsonObject> FirstRun = (*Runs)[0]->AsObject();
	TestTrue(TEXT("History row is an object"), FirstRun.IsValid());
	if (FirstRun.IsValid())
	{
		TestEqual(TEXT("Newest history row matches run id"), FirstRun->GetStringField(TEXT("run_id")), RunId);
		TestEqual(TEXT("History row is compact completed state"), FirstRun->GetStringField(TEXT("state")), FString(TEXT("completed")));
	}

	return true;
}
