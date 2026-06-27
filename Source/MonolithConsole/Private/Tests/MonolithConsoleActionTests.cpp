#include "MonolithConsoleActions.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void MonolithConsoleTestCommand()
	{
		UE_LOG(LogTemp, Log, TEXT("Monolith.ConsoleTest.Command executed"));
	}

	int32 GMonolithConsoleTestCVar = 1;

	void MonolithConsoleTestCVarCommand()
	{
		UE_LOG(LogTemp, Log, TEXT("Monolith.ConsoleTest.CVar value=%d"), GMonolithConsoleTestCVar);
	}

	FString MonolithConsoleTestScreenshotPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots/WindowsEditor/MonolithConsoleActionTest.png"));
	}

	void MonolithConsoleTestOverwriteScreenshot()
	{
		const FString Path = MonolithConsoleTestScreenshotPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FFileHelper::SaveStringToFile(TEXT("Monolith console action overwritten screenshot payload with changed size."), *Path);
		UE_LOG(LogTemp, Log, TEXT("Monolith.ConsoleTest.OverwriteScreenshot wrote %s"), *Path);
	}

	FAutoConsoleCommand GMonolithConsoleActionTestCommand(
		TEXT("Monolith.ConsoleTest.Command"),
		TEXT("Monolith console action automation test command."),
		FConsoleCommandDelegate::CreateStatic(&MonolithConsoleTestCommand));

	FAutoConsoleVariableRef GMonolithConsoleActionTestCVar(
		TEXT("Monolith.ConsoleTest.CVar"),
		GMonolithConsoleTestCVar,
		TEXT("Monolith console action automation test variable."));

	FAutoConsoleCommand GMonolithConsoleActionTestCVarCommand(
		TEXT("Monolith.ConsoleTest.CVarCommand"),
		TEXT("Monolith console action automation test CVar logging command."),
		FConsoleCommandDelegate::CreateStatic(&MonolithConsoleTestCVarCommand));

	FAutoConsoleCommand GMonolithConsoleActionTestOverwriteScreenshotCommand(
		TEXT("Monolith.ConsoleTest.OverwriteScreenshot"),
		TEXT("Overwrite a deterministic Saved/Screenshots PNG path for console capture detection tests."),
		FConsoleCommandDelegate::CreateStatic(&MonolithConsoleTestOverwriteScreenshot));

	TSharedPtr<FJsonObject> ParamsWithCommand(const FString& Command)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("command"), Command);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithConsoleExecuteValidationTest,
	"Monolith.Console.Actions.ExecuteValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConsoleExecuteValidationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> InvalidTarget = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Command"));
	InvalidTarget->SetStringField(TEXT("target_world"), TEXT("invalid"));
	FMonolithActionResult InvalidTargetResult = FMonolithConsoleActions::Execute(InvalidTarget);
	TestFalse(TEXT("Invalid target_world is rejected"), InvalidTargetResult.bSuccess);

	TSharedPtr<FJsonObject> MissingObject = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Missing"));
	MissingObject->SetBoolField(TEXT("require_known_object"), true);
	FMonolithActionResult MissingObjectResult = FMonolithConsoleActions::Execute(MissingObject);
	TestFalse(TEXT("require_known_object rejects unknown first token"), MissingObjectResult.bSuccess);

	TSharedPtr<FJsonObject> DryRun = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Command"));
	DryRun->SetBoolField(TEXT("dry_run"), true);
	DryRun->SetBoolField(TEXT("require_known_object"), true);
	FMonolithActionResult DryRunResult = FMonolithConsoleActions::Execute(DryRun);
	TestTrue(TEXT("Known command dry-run succeeds"), DryRunResult.bSuccess);
	if (DryRunResult.Result.IsValid())
	{
		TestEqual(TEXT("Dry-run status"), DryRunResult.Result->GetStringField(TEXT("status")), FString(TEXT("validated")));
		TestTrue(TEXT("Known object true"), DryRunResult.Result->GetBoolField(TEXT("known_object")));
	}

	TSharedPtr<FJsonObject> Resolve = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Command"));
	FMonolithActionResult ResolveResult = FMonolithConsoleActions::ResolveCommand(Resolve);
	TestTrue(TEXT("resolve_command succeeds"), ResolveResult.bSuccess);
	if (ResolveResult.Result.IsValid())
	{
		TestTrue(TEXT("resolve_command known object"), ResolveResult.Result->GetBoolField(TEXT("known_object")));
		TestEqual(TEXT("resolve_command first token"), ResolveResult.Result->GetStringField(TEXT("first_token")), FString(TEXT("Monolith.ConsoleTest.Command")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithConsoleSearchObjectsParamGuardTest,
	"Monolith.Console.Actions.SearchObjectsParamGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConsoleSearchObjectsParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> BadDetail = MakeShared<FJsonObject>();
	BadDetail->SetStringField(TEXT("query"), TEXT("r.Monolith"));
	BadDetail->SetStringField(TEXT("detail"), TEXT("true"));
	FMonolithActionResult BadDetailResult = FMonolithConsoleActions::SearchObjects(BadDetail);
	TestFalse(TEXT("search_objects rejects string detail before DB access"), BadDetailResult.bSuccess);
	TestTrue(TEXT("detail error explains bool requirement"), BadDetailResult.ErrorMessage.Contains(TEXT("detail")) && BadDetailResult.ErrorMessage.Contains(TEXT("bool")));

	TSharedPtr<FJsonObject> BadProjectionType = MakeShared<FJsonObject>();
	BadProjectionType->SetStringField(TEXT("query"), TEXT("r.Monolith"));
	BadProjectionType->SetNumberField(TEXT("projection"), 1.0);
	FMonolithActionResult BadProjectionTypeResult = FMonolithConsoleActions::SearchObjects(BadProjectionType);
	TestFalse(TEXT("search_objects rejects non-string projection before DB access"), BadProjectionTypeResult.bSuccess);
	TestTrue(TEXT("projection type error explains string requirement"), BadProjectionTypeResult.ErrorMessage.Contains(TEXT("projection")) && BadProjectionTypeResult.ErrorMessage.Contains(TEXT("string")));

	TSharedPtr<FJsonObject> BadProjectionValue = MakeShared<FJsonObject>();
	BadProjectionValue->SetStringField(TEXT("query"), TEXT("r.Monolith"));
	BadProjectionValue->SetStringField(TEXT("projection"), TEXT("wide"));
	FMonolithActionResult BadProjectionValueResult = FMonolithConsoleActions::SearchObjects(BadProjectionValue);
	TestFalse(TEXT("search_objects rejects unsupported projection before DB access"), BadProjectionValueResult.bSuccess);
	TestTrue(TEXT("projection value error lists accepted values"), BadProjectionValueResult.ErrorMessage.Contains(TEXT("compact")) && BadProjectionValueResult.ErrorMessage.Contains(TEXT("full")));

	TSharedPtr<FJsonObject> BadOffset = MakeShared<FJsonObject>();
	BadOffset->SetStringField(TEXT("query"), TEXT("r.Monolith"));
	BadOffset->SetStringField(TEXT("offset"), TEXT("1"));
	FMonolithActionResult BadOffsetResult = FMonolithConsoleActions::SearchObjects(BadOffset);
	TestFalse(TEXT("search_objects rejects non-numeric offset before DB access"), BadOffsetResult.bSuccess);
	TestTrue(TEXT("offset error explains numeric requirement"), BadOffsetResult.ErrorMessage.Contains(TEXT("offset")) && BadOffsetResult.ErrorMessage.Contains(TEXT("number")));

	TSharedPtr<FJsonObject> BadCursor = MakeShared<FJsonObject>();
	BadCursor->SetStringField(TEXT("query"), TEXT("r.Monolith"));
	BadCursor->SetStringField(TEXT("cursor"), TEXT("page-two"));
	FMonolithActionResult BadCursorResult = FMonolithConsoleActions::SearchObjects(BadCursor);
	TestFalse(TEXT("search_objects rejects non-numeric cursor before DB access"), BadCursorResult.bSuccess);
	TestTrue(TEXT("cursor error explains numeric requirement"), BadCursorResult.ErrorMessage.Contains(TEXT("cursor")) && BadCursorResult.ErrorMessage.Contains(TEXT("numeric")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithConsoleSequenceValidationTest,
	"Monolith.Console.Actions.SequenceValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConsoleSequenceValidationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> MissingCursor = MakeShared<FJsonObject>();
	FMonolithActionResult MissingCursorResult = FMonolithConsoleActions::SearchLogsSince(MissingCursor);
	TestFalse(TEXT("search_logs_since requires cursor"), MissingCursorResult.bSuccess);

	TSharedPtr<FJsonObject> Sequence = MakeShared<FJsonObject>();
	Sequence->SetBoolField(TEXT("dry_run"), true);
	Sequence->SetBoolField(TEXT("require_known_object"), true);

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Add(MakeShared<FJsonValueString>(TEXT("Monolith.ConsoleTest.Command")));
	Sequence->SetArrayField(TEXT("commands"), Commands);

	FMonolithActionResult SequenceResult = FMonolithConsoleActions::RunSequence(Sequence);
	TestTrue(TEXT("Dry-run sequence succeeds"), SequenceResult.bSuccess);
	if (SequenceResult.Result.IsValid())
	{
		TestTrue(TEXT("Dry-run sequence passed"), SequenceResult.Result->GetBoolField(TEXT("passed")));
		TestEqual(TEXT("Dry-run sequence executed one command"), static_cast<int32>(SequenceResult.Result->GetIntegerField(TEXT("executed_count"))), 1);
	}

	TSharedPtr<FJsonObject> CursorParams = MakeShared<FJsonObject>();
	FMonolithActionResult CursorResult = FMonolithConsoleActions::GetLogCursor(CursorParams);
	TestTrue(TEXT("get_log_cursor succeeds"), CursorResult.bSuccess);
	if (CursorResult.Result.IsValid())
	{
		const int64 Cursor = static_cast<int64>(CursorResult.Result->GetNumberField(TEXT("cursor")));
		const FString WaitMarker = TEXT("Monolith.ConsoleTest.WaitForLog marker");
		UE_LOG(LogTemp, Log, TEXT("%s"), *WaitMarker);

		TSharedPtr<FJsonObject> WaitParams = MakeShared<FJsonObject>();
		WaitParams->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
		WaitParams->SetStringField(TEXT("pattern"), WaitMarker);
		WaitParams->SetStringField(TEXT("category"), TEXT("LogTemp"));
		WaitParams->SetNumberField(TEXT("timeout_ms"), 1000);
		FMonolithActionResult WaitResult = FMonolithConsoleActions::WaitForLog(WaitParams);
		TestTrue(TEXT("wait_for_log succeeds"), WaitResult.bSuccess);
		if (WaitResult.Result.IsValid())
		{
			TestTrue(TEXT("wait_for_log passed"), WaitResult.Result->GetBoolField(TEXT("passed")));
			TestEqual(TEXT("wait_for_log status"), WaitResult.Result->GetStringField(TEXT("status")), FString(TEXT("matched")));
		}

		TSharedPtr<FJsonObject> RejectOnlyWithoutMode = MakeShared<FJsonObject>();
		RejectOnlyWithoutMode->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
		RejectOnlyWithoutMode->SetStringField(TEXT("reject_log"), TEXT("Monolith.ConsoleTest.NeverEmitted"));
		FMonolithActionResult RejectOnlyWithoutModeResult = FMonolithConsoleActions::WaitForLog(RejectOnlyWithoutMode);
		TestFalse(TEXT("wait_for_log reject-only requires assert_absent mode"), RejectOnlyWithoutModeResult.bSuccess);

		TSharedPtr<FJsonObject> AssertAbsent = MakeShared<FJsonObject>();
		AssertAbsent->SetNumberField(TEXT("cursor"), static_cast<double>(Cursor));
		AssertAbsent->SetStringField(TEXT("mode"), TEXT("assert_absent"));
		AssertAbsent->SetStringField(TEXT("reject_log"), TEXT("Monolith.ConsoleTest.NeverEmitted"));
		AssertAbsent->SetNumberField(TEXT("timeout_ms"), 1);
		FMonolithActionResult AssertAbsentResult = FMonolithConsoleActions::WaitForLog(AssertAbsent);
		TestTrue(TEXT("wait_for_log assert_absent succeeds"), AssertAbsentResult.bSuccess);
		if (AssertAbsentResult.Result.IsValid())
		{
			TestTrue(TEXT("wait_for_log assert_absent passed"), AssertAbsentResult.Result->GetBoolField(TEXT("passed")));
			TestEqual(TEXT("wait_for_log assert_absent status"), AssertAbsentResult.Result->GetStringField(TEXT("status")), FString(TEXT("absent")));
			TestTrue(TEXT("wait_for_log assert_absent full window"), AssertAbsentResult.Result->GetBoolField(TEXT("observed_full_window")));
		}

		FMonolithActionResult RejectCursorResult = FMonolithConsoleActions::GetLogCursor(CursorParams);
		const int64 RejectCursor = RejectCursorResult.Result.IsValid()
			? static_cast<int64>(RejectCursorResult.Result->GetNumberField(TEXT("cursor")))
			: Cursor;
		UE_LOG(LogTemp, Log, TEXT("Monolith.ConsoleTest.Rejected marker"));
		TSharedPtr<FJsonObject> RejectedWait = MakeShared<FJsonObject>();
		RejectedWait->SetNumberField(TEXT("cursor"), static_cast<double>(RejectCursor));
		RejectedWait->SetStringField(TEXT("expect_log"), TEXT("Monolith.ConsoleTest.NotExpected"));
		RejectedWait->SetStringField(TEXT("reject_log"), TEXT("Monolith.ConsoleTest.Rejected marker"));
		RejectedWait->SetNumberField(TEXT("timeout_ms"), 1000);
		FMonolithActionResult RejectedWaitResult = FMonolithConsoleActions::WaitForLog(RejectedWait);
		TestTrue(TEXT("wait_for_log rejected wait succeeds as result data"), RejectedWaitResult.bSuccess);
		if (RejectedWaitResult.Result.IsValid())
		{
			TestFalse(TEXT("wait_for_log rejected wait not passed"), RejectedWaitResult.Result->GetBoolField(TEXT("passed")));
			TestEqual(TEXT("wait_for_log rejected status"), RejectedWaitResult.Result->GetStringField(TEXT("status")), FString(TEXT("rejected")));
			TestTrue(TEXT("wait_for_log rejected flag"), RejectedWaitResult.Result->GetBoolField(TEXT("rejected_matched")));
		}
	}

	TSharedPtr<FJsonObject> ArtifactSequence = MakeShared<FJsonObject>();
	ArtifactSequence->SetBoolField(TEXT("dry_run"), true);
	ArtifactSequence->SetBoolField(TEXT("require_known_object"), true);
	ArtifactSequence->SetStringField(TEXT("artifact_dir"), TEXT("Saved/Automation/MonolithConsoleActionTests/SequenceArtifact"));
	TArray<TSharedPtr<FJsonValue>> ArtifactCommands;
	ArtifactCommands.Add(MakeShared<FJsonValueString>(TEXT("Monolith.ConsoleTest.Command")));
	ArtifactSequence->SetArrayField(TEXT("commands"), ArtifactCommands);
	FMonolithActionResult ArtifactResult = FMonolithConsoleActions::RunSequence(ArtifactSequence);
	TestTrue(TEXT("artifact sequence succeeds"), ArtifactResult.bSuccess);
	if (ArtifactResult.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* ArtifactPtr = nullptr;
		TestTrue(TEXT("artifact object exists"), ArtifactResult.Result->TryGetObjectField(TEXT("artifact"), ArtifactPtr) && ArtifactPtr && (*ArtifactPtr).IsValid());
		if (ArtifactPtr && (*ArtifactPtr).IsValid())
		{
			TestTrue(TEXT("artifact written"), (*ArtifactPtr)->GetBoolField(TEXT("written")));
			TestTrue(TEXT("manifest exists"), IFileManager::Get().FileExists(*(*ArtifactPtr)->GetStringField(TEXT("manifest_path"))));
			TestTrue(TEXT("logs jsonl exists"), IFileManager::Get().FileExists(*(*ArtifactPtr)->GetStringField(TEXT("logs_jsonl_path"))));
		}
	}

	const FString ExistingScreenshotPath = MonolithConsoleTestScreenshotPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExistingScreenshotPath), /*Tree=*/true);
	FFileHelper::SaveStringToFile(TEXT("old"), *ExistingScreenshotPath);
	const FString CaptureCopyPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Automation/MonolithConsoleActionTests/CopiedOverwriteScreenshot.png"));
	IFileManager::Get().Delete(*CaptureCopyPath);

	TSharedPtr<FJsonObject> CaptureParams = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Command"));
	CaptureParams->SetStringField(TEXT("capture_command"), TEXT("Monolith.ConsoleTest.OverwriteScreenshot"));
	CaptureParams->SetStringField(TEXT("output_path"), CaptureCopyPath);
	CaptureParams->SetNumberField(TEXT("settle_ms"), 0);
	CaptureParams->SetNumberField(TEXT("capture_wait_ms"), 1000);
	FMonolithActionResult CaptureResult = FMonolithConsoleActions::ExecuteAndCapture(CaptureParams);
	TestTrue(TEXT("execute_and_capture succeeds for overwritten screenshot path"), CaptureResult.bSuccess);
	if (CaptureResult.Result.IsValid())
	{
		TestTrue(TEXT("execute_and_capture passed for changed screenshot path"), CaptureResult.Result->GetBoolField(TEXT("passed")));
		TestEqual(TEXT("execute_and_capture status captured"), CaptureResult.Result->GetStringField(TEXT("status")), FString(TEXT("captured")));
		FString ReportedCapturePath = CaptureResult.Result->GetStringField(TEXT("capture_path"));
		FPaths::NormalizeFilename(ReportedCapturePath);
		FString NormalizedExistingPath = ExistingScreenshotPath;
		FPaths::NormalizeFilename(NormalizedExistingPath);
		TestEqual(TEXT("execute_and_capture reports overwritten screenshot path"), ReportedCapturePath, NormalizedExistingPath);
		TestTrue(TEXT("execute_and_capture copies changed screenshot to output_path"), IFileManager::Get().FileExists(*CaptureCopyPath));
	}

	TSharedPtr<FJsonObject> PendingCaptureParams = ParamsWithCommand(TEXT("Monolith.ConsoleTest.Command"));
	PendingCaptureParams->SetStringField(TEXT("capture_command"), TEXT("Monolith.ConsoleTest.Command"));
	PendingCaptureParams->SetNumberField(TEXT("settle_ms"), 0);
	PendingCaptureParams->SetNumberField(TEXT("capture_wait_ms"), 1);
	FMonolithActionResult PendingCaptureResult = FMonolithConsoleActions::ExecuteAndCapture(PendingCaptureParams);
	TestTrue(TEXT("execute_and_capture pending-path call succeeds"), PendingCaptureResult.bSuccess);
	FString PendingCaptureId;
	if (PendingCaptureResult.Result.IsValid())
	{
		TestFalse(TEXT("execute_and_capture pending result is not captured yet"), PendingCaptureResult.Result->GetBoolField(TEXT("passed")));
		TestEqual(TEXT("execute_and_capture returns capture_pending for deferred game-thread capture"), PendingCaptureResult.Result->GetStringField(TEXT("status")), FString(TEXT("capture_pending")));
		TestTrue(TEXT("execute_and_capture pending result includes capture id"), PendingCaptureResult.Result->TryGetStringField(TEXT("capture_id"), PendingCaptureId) && !PendingCaptureId.IsEmpty());
	}
	if (!PendingCaptureId.IsEmpty())
	{
		FPlatformProcess::Sleep(0.5f);
		TSharedPtr<FJsonObject> PollParams = MakeShared<FJsonObject>();
		PollParams->SetStringField(TEXT("capture_id"), PendingCaptureId);
		PollParams->SetBoolField(TEXT("consume"), true);
		FMonolithActionResult PollResult = FMonolithConsoleActions::PollCapture(PollParams);
		TestTrue(TEXT("poll_capture succeeds for pending id"), PollResult.bSuccess);
		if (PollResult.Result.IsValid())
		{
			TestTrue(TEXT("poll_capture completed after timeout"), PollResult.Result->GetBoolField(TEXT("completed")));
			TestFalse(TEXT("poll_capture reports failed timeout"), PollResult.Result->GetBoolField(TEXT("passed")));
			TestEqual(TEXT("poll_capture timeout status"), PollResult.Result->GetStringField(TEXT("status")), FString(TEXT("capture_not_found")));
			TestTrue(TEXT("poll_capture consumed completed record"), PollResult.Result->GetBoolField(TEXT("consumed")));
		}
	}

	GMonolithConsoleTestCVar = 1;
	TSharedPtr<FJsonObject> Scoped = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Cvars = MakeShared<FJsonObject>();
	Cvars->SetNumberField(TEXT("Monolith.ConsoleTest.CVar"), 7);
	Scoped->SetObjectField(TEXT("cvars"), Cvars);
	Scoped->SetBoolField(TEXT("require_known_object"), true);
	TArray<TSharedPtr<FJsonValue>> ScopedCommands;
	TSharedPtr<FJsonObject> ScopedStep = MakeShared<FJsonObject>();
	ScopedStep->SetStringField(TEXT("command"), TEXT("Monolith.ConsoleTest.CVarCommand"));
	ScopedStep->SetStringField(TEXT("expect_log"), TEXT("Monolith.ConsoleTest.CVar value=7"));
	ScopedCommands.Add(MakeShared<FJsonValueObject>(ScopedStep));
	Scoped->SetArrayField(TEXT("commands"), ScopedCommands);
	FMonolithActionResult ScopedResult = FMonolithConsoleActions::SetCvarScoped(Scoped);
	TestTrue(TEXT("set_cvar_scoped succeeds"), ScopedResult.bSuccess);
	if (ScopedResult.Result.IsValid())
	{
		TestTrue(TEXT("set_cvar_scoped passed"), ScopedResult.Result->GetBoolField(TEXT("passed")));
		TestTrue(TEXT("set_cvar_scoped restored"), ScopedResult.Result->GetBoolField(TEXT("restored")));
		const TArray<TSharedPtr<FJsonValue>>* CvarReports = nullptr;
		TestTrue(TEXT("set_cvar_scoped cvar reports"), ScopedResult.Result->TryGetArrayField(TEXT("cvars"), CvarReports) && CvarReports && CvarReports->Num() == 1);
		if (CvarReports && CvarReports->Num() == 1)
		{
			const TSharedPtr<FJsonObject> CvarReport = (*CvarReports)[0]->AsObject();
			TestTrue(TEXT("set_cvar_scoped cvar restored"), CvarReport->GetBoolField(TEXT("restored")));
			TestEqual(TEXT("set_cvar_scoped set-by restored"),
				CvarReport->GetStringField(TEXT("restored_set_by")),
				CvarReport->GetStringField(TEXT("original_set_by")));
		}
	}
	TestEqual(TEXT("CVar restored to original"), GMonolithConsoleTestCVar, 1);

	TSharedPtr<FJsonObject> InvalidScoped = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> InvalidCvars = MakeShared<FJsonObject>();
	InvalidCvars->SetNumberField(TEXT("Monolith.ConsoleTest.CVar"), 9);
	InvalidCvars->SetNumberField(TEXT("Monolith.ConsoleTest.Missing"), 3);
	InvalidScoped->SetObjectField(TEXT("cvars"), InvalidCvars);
	TArray<TSharedPtr<FJsonValue>> InvalidScopedCommands;
	InvalidScopedCommands.Add(MakeShared<FJsonValueString>(TEXT("Monolith.ConsoleTest.CVarCommand")));
	InvalidScoped->SetArrayField(TEXT("commands"), InvalidScopedCommands);
	FMonolithActionResult InvalidScopedResult = FMonolithConsoleActions::SetCvarScoped(InvalidScoped);
	TestTrue(TEXT("set_cvar_scoped invalid cvar returns structured result"), InvalidScopedResult.bSuccess);
	if (InvalidScopedResult.Result.IsValid())
	{
		TestFalse(TEXT("set_cvar_scoped invalid cvar not passed"), InvalidScopedResult.Result->GetBoolField(TEXT("passed")));
		TestEqual(TEXT("set_cvar_scoped invalid status"), InvalidScopedResult.Result->GetStringField(TEXT("status")), FString(TEXT("validation_failed")));
	}
	TestEqual(TEXT("Invalid scoped CVar preflight does not mutate valid CVar"), GMonolithConsoleTestCVar, 1);

	TSharedPtr<FJsonObject> DiagnoseParams = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> TimedOut = MakeShared<FJsonObject>();
	TimedOut->SetBoolField(TEXT("timed_out"), true);
	DiagnoseParams->SetObjectField(TEXT("result"), TimedOut);
	FMonolithActionResult DiagnoseResult = FMonolithConsoleActions::DiagnoseFailure(DiagnoseParams);
	TestTrue(TEXT("diagnose_failure succeeds"), DiagnoseResult.bSuccess);
	if (DiagnoseResult.Result.IsValid())
	{
		TestTrue(TEXT("diagnose_failure detects failure"), DiagnoseResult.Result->GetBoolField(TEXT("failure_detected")));
	}

	TSharedPtr<FJsonObject> ArtifactDiagnoseParams = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ArtifactFailure = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("status"), TEXT("write_failed"));
	Artifact->SetStringField(TEXT("manifest_error"), TEXT("permission denied"));
	ArtifactFailure->SetBoolField(TEXT("passed"), false);
	ArtifactFailure->SetObjectField(TEXT("artifact"), Artifact);
	ArtifactDiagnoseParams->SetObjectField(TEXT("result"), ArtifactFailure);
	FMonolithActionResult ArtifactDiagnoseResult = FMonolithConsoleActions::DiagnoseFailure(ArtifactDiagnoseParams);
	TestTrue(TEXT("diagnose_failure artifact succeeds"), ArtifactDiagnoseResult.bSuccess);
	if (ArtifactDiagnoseResult.Result.IsValid())
	{
		TestTrue(TEXT("diagnose_failure detects artifact write failure"), ArtifactDiagnoseResult.Result->GetBoolField(TEXT("failure_detected")));
	}

	return true;
}

#endif
