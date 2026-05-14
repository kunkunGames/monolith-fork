#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithActionExecutionGuard.h"
#include "MonolithSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	bool RecordArrayContainsStatus(const TArray<TSharedPtr<FJsonValue>>* Records, const FString& ExpectedStatus)
	{
		if (!Records)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Records)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid())
			{
				FString Status;
				if ((*Obj)->TryGetStringField(TEXT("status"), Status) && Status == ExpectedStatus)
				{
					return true;
				}
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolCallRecordsDisabledTest,
	"Monolith.Core.ToolCallRecords.DisabledByDefault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolCallRecordsDisabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalRecords = Settings->bEnableAdvancedToolCallRecords;
	Settings->bEnableAdvancedToolCallRecords = false;

	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();
	Guard.RecordRejectedToolCall(
		TEXT("blueprint_query"),
		TEXT("blueprint"),
		TEXT("compile_blueprint"),
		TEXT("profile_blocked"),
		-32600,
		TEXT("blocked"));

	TSharedPtr<FJsonObject> Records = Guard.GetToolCallRecordsJson(10, FString(), FString());
	TestTrue(TEXT("Disabled result object exists"), Records.IsValid());
	if (Records.IsValid())
	{
		TestFalse(TEXT("Advanced records disabled"), Records->GetBoolField(TEXT("enabled")));
		TestEqual(TEXT("No rows are returned while disabled"), Records->GetIntegerField(TEXT("returned_count")), 0);
	}

	Settings->bEnableAdvancedToolCallRecords = bOriginalRecords;
	Guard.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithToolCallRecordsEnabledTest,
	"Monolith.Core.ToolCallRecords.EnabledRedactedAnalysis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithToolCallRecordsEnabledTest::RunTest(const FString& Parameters)
{
	UMonolithSettings* Settings = GetMutableDefault<UMonolithSettings>();
	TestNotNull(TEXT("Monolith settings are available"), Settings);
	if (!Settings)
	{
		return false;
	}

	const bool bOriginalRecords = Settings->bEnableAdvancedToolCallRecords;
	Settings->bEnableAdvancedToolCallRecords = true;

	FMonolithActionExecutionGuard& Guard = FMonolithActionExecutionGuard::Get();
	Guard.ResetForTests();

	Guard.RecordRejectedToolCall(
		TEXT("blueprint_query"),
		TEXT("blueprint"),
		TEXT("compile_blueprint"),
		TEXT("profile_blocked"),
		-32600,
		TEXT("blocked by profile"));
	Guard.RecordRejectedToolCall(
		TEXT("material_query"),
		TEXT("material"),
		TEXT("missing"),
		TEXT("malformed_dispatch"),
		-32602,
		TEXT("missing required params"));
	Guard.RecordRejectedToolCall(
		TEXT("material_query"),
		TEXT("material"),
		TEXT("missing"),
		TEXT("malformed_dispatch"),
		-32602,
		TEXT("missing required params"));

	FMonolithActionExecutionGuard::FExecutionScope Scope = Guard.BeginAction(TEXT("recordtest"), TEXT("success_action"));
	TSharedPtr<FJsonObject> SuccessPayload = MakeShared<FJsonObject>();
	SuccessPayload->SetBoolField(TEXT("ok"), true);
	Guard.SetActionOutcome(Scope, true, 0, SuccessPayload, FString());
	Guard.EndAction(Scope);

	TSharedPtr<FJsonObject> Records = Guard.GetToolCallRecordsJson(10, FString(), FString());
	TestTrue(TEXT("Enabled result object exists"), Records.IsValid());
	FString FirstRecordId;
	if (Records.IsValid())
	{
		TestTrue(TEXT("Advanced records enabled"), Records->GetBoolField(TEXT("enabled")));
		TestEqual(TEXT("Four rows are returned"), Records->GetIntegerField(TEXT("returned_count")), 4);

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		TestTrue(TEXT("Records array exists"), Records->TryGetArrayField(TEXT("records"), Rows));
		TestTrue(TEXT("Rows include success"), RecordArrayContainsStatus(Rows, TEXT("success")));
		TestTrue(TEXT("Rows include profile block"), RecordArrayContainsStatus(Rows, TEXT("profile_blocked")));

		if (Rows && Rows->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* First = nullptr;
			if ((*Rows)[0].IsValid() && (*Rows)[0]->TryGetObject(First) && First && First->IsValid())
			{
				(*First)->TryGetStringField(TEXT("id"), FirstRecordId);
				TestFalse(TEXT("Raw payload logging is false"), (*First)->GetBoolField(TEXT("raw_payload_logging")));
				TestFalse(TEXT("Params are not stored"), (*First)->HasField(TEXT("params")));
			}
		}
	}

	TestFalse(TEXT("First record id captured"), FirstRecordId.IsEmpty());
	TSharedPtr<FJsonObject> OneRecord = Guard.GetToolCallRecordJson(FirstRecordId);
	TestTrue(TEXT("Single record result exists"), OneRecord.IsValid());
	if (OneRecord.IsValid())
	{
		TestTrue(TEXT("Single record found"), OneRecord->GetBoolField(TEXT("found")));
	}

	TSharedPtr<FJsonObject> Analysis = Guard.AnalyzeToolCallRecordsJson(10);
	TestTrue(TEXT("Analysis result exists"), Analysis.IsValid());
	if (Analysis.IsValid())
	{
		const TSharedPtr<FJsonObject>* Summary = nullptr;
		TestTrue(TEXT("Summary exists"), Analysis->TryGetObjectField(TEXT("summary"), Summary));
		if (Summary && Summary->IsValid())
		{
			TestEqual(TEXT("Profile block count"), (*Summary)->GetIntegerField(TEXT("profile_blocks")), 1);
			TestEqual(TEXT("Repeated failure count"), (*Summary)->GetIntegerField(TEXT("repeated_failures")), 1);
		}
	}

	Settings->bEnableAdvancedToolCallRecords = bOriginalRecords;
	Guard.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
