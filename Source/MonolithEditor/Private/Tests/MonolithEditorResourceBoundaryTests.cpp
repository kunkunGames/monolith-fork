#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetDeleteAssetsRejectsOversizedArray, "Monolith.LimitGuard.MonolithAsset.DeleteAssetsRejectsOversizedArray", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetDeleteAssetsRejectsOversizedArray::RunTest(const FString& Parameters)
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
	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("asset"), TEXT("delete_assets"), Params);

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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorTailLogClampsCountTest, "Monolith.LimitGuard.MonolithEditor.TailLogClampsCountLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorTailLogClampsCountTest::RunTest(const FString& Parameters)
{
	// Seed 501 logs to ensure we have enough to test the clamp
	for (int32 i = 0; i < 501; ++i)
	{
		UE_LOG(LogTemp, Log, TEXT("MonolithEditorTailLogClampsCountTest Seed Log %d"), i);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("count"), 10000.0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("tail_log"), Params);

	TestTrue(TEXT("Should succeed since it executes the action"), Result.bSuccess);
	if (Result.Result.IsValid())
	{
		double CountOut = 0.0;
		TestTrue(TEXT("Result JSON should contain the count field"), Result.Result->TryGetNumberField(TEXT("count"), CountOut));
		TestEqual(TEXT("Should clamp count to exactly 500"), CountOut, 500.0);

		const TArray<TSharedPtr<FJsonValue>>* LinesArray = nullptr;
		TestTrue(TEXT("Result JSON should contain the lines array"), Result.Result->TryGetArrayField(TEXT("lines"), LinesArray));
		if (LinesArray)
		{
			TestEqual(TEXT("Lines array should have exactly 500 elements"), LinesArray->Num(), 500);
		}
	}
	else
	{
		AddError(TEXT("Result JSON object is invalid"));
	}

	// Test that an invalid type string returns an error
	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("count"), TEXT("10000"));

	FMonolithActionResult InvalidResult = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("tail_log"), InvalidParams);
	TestFalse(TEXT("Should fail on string count type"), InvalidResult.bSuccess);
	TestTrue(TEXT("Should return an invalid param error"), InvalidResult.ErrorMessage.Contains(TEXT("Invalid param")));

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
		double MaxTestsOut = 0.0;
		TestTrue(TEXT("Result JSON should contain the clamped max_tests field"), Result.Result->TryGetNumberField(TEXT("max_tests"), MaxTestsOut));
		TestEqual(TEXT("Should clamp max_tests to 1000"), MaxTestsOut, 1000.0);
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

	bool bCanStop = true;
	TestTrue(TEXT("Status exposes can_stop"), Result.Result->TryGetBoolField(TEXT("can_stop"), bCanStop));
	TestFalse(TEXT("Synchronous runner cannot stop"), bCanStop);

	FString StopStatus;
	TestTrue(TEXT("Status exposes stop_status"), Result.Result->TryGetStringField(TEXT("stop_status"), StopStatus));
	TestEqual(TEXT("Stop contract is explicit"), StopStatus, FString(TEXT("unsupported_cancel")));
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

	bool bStopped = true;
	TestTrue(TEXT("Result exposes stopped flag"), Result.Result->TryGetBoolField(TEXT("stopped"), bStopped));
	TestFalse(TEXT("No in-flight run is stopped"), bStopped);

	bool bCanStop = true;
	TestTrue(TEXT("Result exposes can_stop flag"), Result.Result->TryGetBoolField(TEXT("can_stop"), bCanStop));
	TestFalse(TEXT("Synchronous runner cannot stop"), bCanStop);

	FString StopStatus;
	TestTrue(TEXT("Result exposes stop_status flag"), Result.Result->TryGetStringField(TEXT("stop_status"), StopStatus));
	TestEqual(TEXT("Stop status reports unsupported_cancel"), StopStatus, FString(TEXT("unsupported_cancel")));

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

	FString RunId;
	TestTrue(TEXT("Result exposes run_id"), RunResult.Result->TryGetStringField(TEXT("run_id"), RunId));
	TestFalse(TEXT("Run id should not be empty"), RunId.IsEmpty());

	FString RunState;
	TestTrue(TEXT("Result exposes state"), RunResult.Result->TryGetStringField(TEXT("state"), RunState));
	TestEqual(TEXT("No-match run state completes"), RunState, FString(TEXT("completed")));

	FString CompletionReason;
	TestTrue(TEXT("Result exposes completion_reason"), RunResult.Result->TryGetStringField(TEXT("completion_reason"), CompletionReason));
	TestEqual(TEXT("No-match completion reason is explicit"), CompletionReason, FString(TEXT("no_matching_tests")));

	double ProgressOut = 0.0;
	TestTrue(TEXT("Result exposes progress"), RunResult.Result->TryGetNumberField(TEXT("progress"), ProgressOut));
	TestEqual(TEXT("No-match run progress is complete"), ProgressOut, 1.0);

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
		FString HistRunId;
		TestTrue(TEXT("History row exposes run_id"), FirstRun->TryGetStringField(TEXT("run_id"), HistRunId));
		TestEqual(TEXT("Newest history row matches run id"), HistRunId, RunId);

		FString HistState;
		TestTrue(TEXT("History row exposes state"), FirstRun->TryGetStringField(TEXT("state"), HistState));
		TestEqual(TEXT("History row is compact completed state"), HistState, FString(TEXT("completed")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorGetRecentLogsClampsCount, "Monolith.LimitGuard.MonolithEditor.GetRecentLogsClampsCount", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorGetRecentLogsClampsCount::RunTest(const FString& Parameters)
{
	for (int32 Index = 0; Index < 1001; ++Index)
	{
		UE_LOG(LogTemp, Log, TEXT("Monolith get_recent_logs clamp seed %d"), Index);
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("count"), 5000);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("get_recent_logs"), Params);

	TestTrue(TEXT("Should succeed even with extreme count value"), Result.bSuccess);
	if (!Result.Result.IsValid())
	{
		AddError(TEXT("Result JSON object is invalid"));
		return true;
	}

	double CountVal = 0.0;
	if (Result.Result->TryGetNumberField(TEXT("count"), CountVal))
	{
		TestEqual(TEXT("Returned entries count should equal the maximum clamped limit of 1000"), CountVal, 1000.0);
	}
	else
	{
		AddError(TEXT("Result should expose count"));
	}

	return true;
}
