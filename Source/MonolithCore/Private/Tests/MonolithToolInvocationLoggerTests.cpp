#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "MonolithSettings.h"
#include "MonolithToolInvocationLogger.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool ParseLastJsonLine(const FString& Contents, TSharedPtr<FJsonObject>& OutRecord)
	{
		TArray<FString> Lines;
		Contents.ParseIntoArrayLines(Lines, false);
		for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
		{
			const FString Trimmed = Lines[Index].TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				continue;
			}
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
			return FJsonSerializer::Deserialize(Reader, OutRecord) && OutRecord.IsValid();
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolInvocationLoggerDailyLogTest,
	"Monolith.Core.ToolInvocationLogger.DailyLogOptInRedaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolInvocationLoggerDailyLogTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalDailyLog = Settings->bEnableDailyLog;
	const FString OriginalLogDir = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_DIR"));
	const FString OriginalMaxFieldBytes = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES"));
	const FString TempLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithToolInvocationLoggerTest"));
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	IFileManager::Get().MakeDirectory(*TempLogDir, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *TempLogDir);

	const FString DailyActionLogPath = FPaths::Combine(TempLogDir, FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT("action.jsonl"));
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("token"), TEXT("super-secret-token"));
	Params->SetStringField(TEXT("query"), TEXT("UObject"));

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("status"), TEXT("ok"));
	FMonolithActionResult Result = FMonolithActionResult::Success(ResultObj);

	Settings->bEnableDailyLog = false;
	FMonolithToolInvocationLogger::RecordAction(
		TEXT("source"),
		TEXT("search_source"),
		Params,
		Result,
		TEXT("dispatch"),
		FMonolithToolInvocationLogger::NowIso8601WithOffset(),
		FMonolithToolInvocationLogger::NowSeconds());
	TestFalse(TEXT("Disabled daily action log does not create a file"), FPaths::FileExists(DailyActionLogPath));

	Settings->bEnableDailyLog = true;
	FMonolithToolInvocationLogger::RecordAction(
		TEXT("source"),
		TEXT("search_source"),
		Params,
		Result,
		TEXT("dispatch"),
		FMonolithToolInvocationLogger::NowIso8601WithOffset(),
		FMonolithToolInvocationLogger::NowSeconds());
	TestTrue(TEXT("Enabled daily action log creates a file"), FPaths::FileExists(DailyActionLogPath));

	FString Contents;
	TestTrue(TEXT("Daily action log can be read"), FFileHelper::LoadFileToString(Contents, *DailyActionLogPath));
	TestFalse(TEXT("Sensitive value is redacted"), Contents.Contains(TEXT("super-secret-token")));
	TestTrue(TEXT("Redacted marker is present"), Contents.Contains(TEXT("[REDACTED]")));

	TSharedPtr<FJsonObject> Record;
	if (TestTrue(TEXT("Daily log line parses as JSON"), ParseLastJsonLine(Contents, Record)))
	{
		if (Record.IsValid())
		{
			TestEqual(TEXT("Format version is v3"), static_cast<int32>(Record->GetNumberField(TEXT("format_version"))), 3);
			TestEqual(TEXT("Surface is action"), Record->GetStringField(TEXT("surface")), TEXT("action"));
			TestFalse(TEXT("Record id is present"), Record->GetStringField(TEXT("record_id")).IsEmpty());
			TestFalse(TEXT("Trace id is present"), Record->GetStringField(TEXT("trace_id")).IsEmpty());
			TestFalse(TEXT("Span id is present"), Record->GetStringField(TEXT("span_id")).IsEmpty());
			TestFalse(TEXT("Process instance id is present"), Record->GetStringField(TEXT("process_instance_id")).IsEmpty());
			const TSharedPtr<FJsonObject>* RoutingContext = nullptr;
			TestTrue(TEXT("routing_context exists"), Record->TryGetObjectField(TEXT("routing_context"), RoutingContext));
			const TSharedPtr<FJsonObject>* Workflow = nullptr;
			TestTrue(TEXT("workflow exists"), Record->TryGetObjectField(TEXT("workflow"), Workflow));
			const TSharedPtr<FJsonObject>* PhaseTiming = nullptr;
			TestTrue(TEXT("phase_timing exists"), Record->TryGetObjectField(TEXT("phase_timing"), PhaseTiming));
			const TSharedPtr<FJsonObject>* Environment = nullptr;
			TestTrue(TEXT("environment exists"), Record->TryGetObjectField(TEXT("environment"), Environment));
			const TSharedPtr<FJsonObject>* ReturnSummary = nullptr;
			if (TestTrue(TEXT("return_summary exists"), Record->TryGetObjectField(TEXT("return_summary"), ReturnSummary)) && ReturnSummary && ReturnSummary->IsValid())
			{
				TestEqual(TEXT("return_summary has result_shape"), (*ReturnSummary)->GetStringField(TEXT("result_shape")), TEXT("object"));
			}
			const TSharedPtr<FJsonObject>* AgentSignal = nullptr;
			TestTrue(TEXT("agent_signal exists"), Record->TryGetObjectField(TEXT("agent_signal"), AgentSignal));
			if (AgentSignal && AgentSignal->IsValid())
			{
				TestEqual(TEXT("Outcome is success"), (*AgentSignal)->GetStringField(TEXT("outcome")), TEXT("success"));
				TestFalse(TEXT("Agent signal does not duplicate retry signature"), (*AgentSignal)->HasField(TEXT("retry_signature")));
				TestFalse(TEXT("Agent signal does not duplicate result bytes"), (*AgentSignal)->HasField(TEXT("result_bytes")));
			}
		}
	}

	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES"), TEXT("4096"));

	TSharedPtr<FJsonObject> LargeParams = MakeShared<FJsonObject>();
	LargeParams->SetStringField(TEXT("query"), FString::ChrN(300000, TCHAR('x')));

	TSharedPtr<FJsonObject> LargeResultObj = MakeShared<FJsonObject>();
	LargeResultObj->SetStringField(TEXT("payload"), FString::ChrN(300000, TCHAR('y')));
	const FMonolithActionResult LargeResult = FMonolithActionResult::Success(LargeResultObj);

	FMonolithToolInvocationLogger::RecordAction(
		TEXT("source"),
		TEXT("large_payload_test"),
		LargeParams,
		LargeResult,
		TEXT("dispatch"),
		FMonolithToolInvocationLogger::NowIso8601WithOffset(),
		FMonolithToolInvocationLogger::NowSeconds());

	if (TestTrue(TEXT("Daily action log with large payload can be read"), FFileHelper::LoadFileToString(Contents, *DailyActionLogPath)))
	{
		TSharedPtr<FJsonObject> LargeRecord;
		if (TestTrue(TEXT("Large daily log line parses as JSON"), ParseLastJsonLine(Contents, LargeRecord)) && LargeRecord.IsValid())
		{
			const TSharedPtr<FJsonObject>* Redaction = nullptr;
			if (TestTrue(TEXT("Large record redaction object exists"), LargeRecord->TryGetObjectField(TEXT("redaction"), Redaction)) && Redaction && Redaction->IsValid())
			{
				TestTrue(TEXT("Large record is marked truncated"), (*Redaction)->GetBoolField(TEXT("truncated")));
				TestTrue(TEXT("Large record has argument SHA-256"), (*Redaction)->GetStringField(TEXT("argument_sha256")).StartsWith(TEXT("sha256:")));
				TestTrue(TEXT("Large record has result SHA-256"), (*Redaction)->GetStringField(TEXT("result_sha256")).StartsWith(TEXT("sha256:")));
			}
			const TSharedPtr<FJsonObject>* ReturnSummary = nullptr;
			if (TestTrue(TEXT("Large record return_summary exists"), LargeRecord->TryGetObjectField(TEXT("return_summary"), ReturnSummary)) && ReturnSummary && ReturnSummary->IsValid())
			{
				TestTrue(TEXT("Large return summary is marked truncated"), (*ReturnSummary)->GetBoolField(TEXT("truncated")));
			}

			const TSharedPtr<FJsonObject>* Call = nullptr;
			if (TestTrue(TEXT("Large record call object exists"), LargeRecord->TryGetObjectField(TEXT("call"), Call)) && Call && Call->IsValid())
			{
				const TSharedPtr<FJsonObject>* Arguments = nullptr;
				if (TestTrue(TEXT("Large record bounded arguments exist"), (*Call)->TryGetObjectField(TEXT("arguments"), Arguments)) && Arguments && Arguments->IsValid())
				{
					TestTrue(TEXT("Large arguments are replaced by bounded envelope"), (*Arguments)->GetBoolField(TEXT("truncated")));
					TestTrue(TEXT("Large arguments honor env max preview size"), (*Arguments)->GetStringField(TEXT("preview")).Len() <= 4096);
				}
			}

			const TSharedPtr<FJsonObject>* Return = nullptr;
			if (TestTrue(TEXT("Large record bounded return exists"), LargeRecord->TryGetObjectField(TEXT("return"), Return)) && Return && Return->IsValid())
			{
				TestTrue(TEXT("Large return is replaced by bounded envelope"), (*Return)->GetBoolField(TEXT("truncated")));
				TestTrue(TEXT("Large return honors env max preview size"), (*Return)->GetStringField(TEXT("preview")).Len() <= 4096);
			}
		}
	}

	Settings->bEnableDailyLog = bOriginalDailyLog;
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *OriginalLogDir);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_MAX_FIELD_BYTES"), *OriginalMaxFieldBytes);
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolInvocationLoggerLookupFailureTest,
	"Monolith.Core.ToolInvocationLogger.PreDispatchFailureLogged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolInvocationLoggerLookupFailureTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalDailyLog = Settings->bEnableDailyLog;
	const FString OriginalLogDir = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_DIR"));
	const FString TempLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithToolInvocationLookupFailureTest"));
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	IFileManager::Get().MakeDirectory(*TempLogDir, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *TempLogDir);
	Settings->bEnableDailyLog = true;

	const TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("missing_namespace_for_log_test"),
		TEXT("missing_action_for_log_test"),
		Params);
	TestFalse(TEXT("Missing action returns an error"), Result.bSuccess);

	const FString DailyActionLogPath = FPaths::Combine(TempLogDir, FDateTime::Now().ToString(TEXT("%Y%m%d")), TEXT("action.jsonl"));
	TestTrue(TEXT("Lookup failure action log exists"), FPaths::FileExists(DailyActionLogPath));

	FString Contents;
	if (TestTrue(TEXT("Lookup failure action log can be read"), FFileHelper::LoadFileToString(Contents, *DailyActionLogPath)))
	{
		TSharedPtr<FJsonObject> Record;
		if (TestTrue(TEXT("Lookup failure log line parses as JSON"), ParseLastJsonLine(Contents, Record)) && Record.IsValid())
		{
			TestEqual(TEXT("Lookup failure status"), Record->GetStringField(TEXT("status")), TEXT("error"));
			TestEqual(TEXT("Lookup failure format version is v3"), static_cast<int32>(Record->GetNumberField(TEXT("format_version"))), 3);
			TestFalse(TEXT("Lookup failure trace id is present"), Record->GetStringField(TEXT("trace_id")).IsEmpty());

			const TSharedPtr<FJsonObject>* Call = nullptr;
			if (TestTrue(TEXT("Lookup failure call object exists"), Record->TryGetObjectField(TEXT("call"), Call)) && Call && Call->IsValid())
			{
				TestEqual(TEXT("Lookup failure namespace"), (*Call)->GetStringField(TEXT("namespace")), TEXT("missing_namespace_for_log_test"));
				TestEqual(TEXT("Lookup failure action"), (*Call)->GetStringField(TEXT("action")), TEXT("missing_action_for_log_test"));
				TestEqual(TEXT("Lookup failure validation phase"), (*Call)->GetStringField(TEXT("validation_phase")), TEXT("lookup"));
			}

			const TSharedPtr<FJsonObject>* AgentSignal = nullptr;
			if (TestTrue(TEXT("Lookup failure agent signal exists"), Record->TryGetObjectField(TEXT("agent_signal"), AgentSignal)) && AgentSignal && AgentSignal->IsValid())
			{
				TestEqual(TEXT("Lookup failure outcome"), (*AgentSignal)->GetStringField(TEXT("outcome")), TEXT("tool_error"));
				TestEqual(TEXT("Lookup failure error class"), (*AgentSignal)->GetStringField(TEXT("error_class")), TEXT("unknown_action"));
			}
		}
	}

	Settings->bEnableDailyLog = bOriginalDailyLog;
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *OriginalLogDir);
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolInvocationLoggerDualSurfaceTest,
	"Monolith.Core.ToolInvocationLogger.SourceChildQueryDualSurface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolInvocationLoggerDualSurfaceTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalDailyLog = Settings->bEnableDailyLog;
	const FString OriginalLogDir = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_DIR"));
	const FString OriginalToolLogEnabled = FPlatformMisc::GetEnvironmentVariable(TEXT("MONOLITH_TOOL_LOG_ENABLED"));
	const FString TempLogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithToolInvocationDualSurfaceTest"));
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	IFileManager::Get().MakeDirectory(*TempLogDir, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *TempLogDir);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_ENABLED"), TEXT("1"));
	Settings->bEnableDailyLog = true;

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	FMonolithToolRegistry::Get().ExecuteAction(TEXT("source"), TEXT("crg_graph_health"), Params);

	const FString DailyFolder = FPaths::Combine(TempLogDir, FDateTime::Now().ToString(TEXT("%Y%m%d")));
	const FString ActionLogPath = FPaths::Combine(DailyFolder, TEXT("action.jsonl"));
	const FString QueryLogPath = FPaths::Combine(DailyFolder, TEXT("query.jsonl"));
	TestTrue(TEXT("Live source action emits action log"), FPaths::FileExists(ActionLogPath));
	TestTrue(TEXT("Live source action child monolith_query emits query log"), FPaths::FileExists(QueryLogPath));

	FString ActionContents;
	FString ActionTraceId;
	FString ActionSpanId;
	if (TestTrue(TEXT("Action log can be read"), FFileHelper::LoadFileToString(ActionContents, *ActionLogPath)))
	{
		TSharedPtr<FJsonObject> ActionRecord;
		if (TestTrue(TEXT("Action log has valid JSONL"), ParseLastJsonLine(ActionContents, ActionRecord)) && ActionRecord.IsValid())
		{
			TestEqual(TEXT("Action surface"), ActionRecord->GetStringField(TEXT("surface")), TEXT("action"));
			TestEqual(TEXT("Action format version is v3"), static_cast<int32>(ActionRecord->GetNumberField(TEXT("format_version"))), 3);
			ActionTraceId = ActionRecord->GetStringField(TEXT("trace_id"));
			TestFalse(TEXT("Action trace id is present"), ActionTraceId.IsEmpty());
			ActionSpanId = ActionRecord->GetStringField(TEXT("span_id"));
			TestFalse(TEXT("Action span id is present"), ActionSpanId.IsEmpty());
			const TSharedPtr<FJsonObject>* ChildProcess = nullptr;
			TestTrue(TEXT("Action child_process exists"), ActionRecord->TryGetObjectField(TEXT("child_process"), ChildProcess));
			const TSharedPtr<FJsonObject>* Call = nullptr;
			if (TestTrue(TEXT("Action call object exists"), ActionRecord->TryGetObjectField(TEXT("call"), Call)) && Call && Call->IsValid())
			{
				TestEqual(TEXT("Action namespace"), (*Call)->GetStringField(TEXT("namespace")), TEXT("source"));
				TestEqual(TEXT("Action name"), (*Call)->GetStringField(TEXT("action")), TEXT("crg_graph_health"));
			}
		}
	}

	FString QueryContents;
	if (TestTrue(TEXT("Query log can be read"), FFileHelper::LoadFileToString(QueryContents, *QueryLogPath)))
	{
		TSharedPtr<FJsonObject> QueryRecord;
		if (TestTrue(TEXT("Query log has valid JSONL"), ParseLastJsonLine(QueryContents, QueryRecord)) && QueryRecord.IsValid())
		{
			TestEqual(TEXT("Query surface"), QueryRecord->GetStringField(TEXT("surface")), TEXT("query"));
			TestEqual(TEXT("Query format version is v3"), static_cast<int32>(QueryRecord->GetNumberField(TEXT("format_version"))), 3);
			TestEqual(TEXT("Query inherits action trace id"), QueryRecord->GetStringField(TEXT("trace_id")), ActionTraceId);
			TestEqual(TEXT("Query parent span id is action span"), QueryRecord->GetStringField(TEXT("parent_span_id")), ActionSpanId);
			TestFalse(TEXT("Query span id is present"), QueryRecord->GetStringField(TEXT("span_id")).IsEmpty());
			const TSharedPtr<FJsonObject>* ReturnSummary = nullptr;
			TestTrue(TEXT("Query return_summary exists"), QueryRecord->TryGetObjectField(TEXT("return_summary"), ReturnSummary));
			const TSharedPtr<FJsonObject>* Call = nullptr;
			if (TestTrue(TEXT("Query call object exists"), QueryRecord->TryGetObjectField(TEXT("call"), Call)) && Call && Call->IsValid())
			{
				TestEqual(TEXT("Query namespace"), (*Call)->GetStringField(TEXT("namespace")), TEXT("source"));
				TestEqual(TEXT("Query action"), (*Call)->GetStringField(TEXT("action")), TEXT("crg_graph_health"));
			}
		}
	}

	Settings->bEnableDailyLog = bOriginalDailyLog;
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_DIR"), *OriginalLogDir);
	FPlatformMisc::SetEnvironmentVar(TEXT("MONOLITH_TOOL_LOG_ENABLED"), *OriginalToolLogEnabled);
	IFileManager::Get().DeleteDirectory(*TempLogDir, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
