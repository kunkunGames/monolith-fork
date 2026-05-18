#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MonolithMcpSessionTracker.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	bool JsonArrayContainsRawString(const TSharedPtr<FJsonObject>& Obj, const FString& Raw)
	{
		if (!Obj.IsValid())
		{
			return false;
		}

		FString Serialized;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Serialized.Contains(Raw);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionTrackerRedactionTest,
	"Monolith.Core.McpSessionTracker.Redaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionTrackerRedactionTest::RunTest(const FString& Parameters)
{
	FMonolithMcpSessionTracker& Tracker = FMonolithMcpSessionTracker::Get();
	Tracker.ResetForTests();

	const FString RawSessionId = TEXT("session_secret_1234567890");
	Tracker.ObserveRequest(RawSessionId, TEXT("2025-03-26"), TEXT("tools/call"), TEXT("blueprint_query"));

	TSharedPtr<FJsonObject> Result = Tracker.ListSessionsJson(10);
	TestFalse(TEXT("Raw session id is not serialized"), JsonArrayContainsRawString(Result, RawSessionId));
	TestFalse(TEXT("Raw ids are not stored"), Result->GetBoolField(TEXT("raw_session_ids_stored")));
	TestEqual(TEXT("Session count"), Result->GetIntegerField(TEXT("session_count")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Sessions = nullptr;
	TestTrue(TEXT("sessions exists"), Result->TryGetArrayField(TEXT("sessions"), Sessions));
	if (Sessions && Sessions->Num() == 1)
	{
		TSharedPtr<FJsonObject> Row = (*Sessions)[0]->AsObject();
		TestTrue(TEXT("session key is hashed"), Row->GetStringField(TEXT("session_key")).StartsWith(TEXT("md5:")));
		TestEqual(TEXT("method captured"), Row->GetStringField(TEXT("last_method")), TEXT("tools/call"));
		TestEqual(TEXT("tool captured"), Row->GetStringField(TEXT("last_tool_name")), TEXT("blueprint_query"));
	}

	Tracker.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionTrackerOrderingAndTerminateTest,
	"Monolith.Core.McpSessionTracker.OrderingAndTerminate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionTrackerOrderingAndTerminateTest::RunTest(const FString& Parameters)
{
	FMonolithMcpSessionTracker& Tracker = FMonolithMcpSessionTracker::Get();
	Tracker.ResetForTests();

	Tracker.ObserveRequest(TEXT("session_a_00000001"), TEXT("2024-11-05"), TEXT("initialize"), TEXT(""));
	FPlatformProcess::Sleep(0.01f);
	Tracker.ObserveRequest(TEXT("session_b_00000002"), TEXT("2025-03-26"), TEXT("tools/call"), TEXT("material_query"));

	TSharedPtr<FJsonObject> Listed = Tracker.ListSessionsJson(1);
	TestEqual(TEXT("Returned limit respected"), Listed->GetIntegerField(TEXT("returned_count")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Sessions = nullptr;
	TestTrue(TEXT("sessions exists"), Listed->TryGetArrayField(TEXT("sessions"), Sessions));
	if (Sessions && Sessions->Num() == 1)
	{
		TSharedPtr<FJsonObject> Row = (*Sessions)[0]->AsObject();
		TestEqual(TEXT("Most recent row first"), Row->GetStringField(TEXT("last_tool_name")), TEXT("material_query"));
	}

	TSharedPtr<FJsonObject> Removed = Tracker.RemoveSessionJson(TEXT("session_b_00000002"));
	TestTrue(TEXT("Observed row removed"), Removed->GetBoolField(TEXT("terminated")));
	TestFalse(TEXT("No in-flight cancellation claim"), Removed->GetBoolField(TEXT("cancelled_in_flight_requests")));
	TestFalse(TEXT("Raw removed session id not echoed"), JsonArrayContainsRawString(Removed, TEXT("session_b_00000002")));

	TSharedPtr<FJsonObject> After = Tracker.ListSessionsJson(10);
	TestEqual(TEXT("One row remains"), After->GetIntegerField(TEXT("session_count")), 1);

	Tracker.ResetForTests();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMcpSessionTrackerBoundedRowsTest,
	"Monolith.Core.McpSessionTracker.BoundedRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMcpSessionTrackerBoundedRowsTest::RunTest(const FString& Parameters)
{
	FMonolithMcpSessionTracker& Tracker = FMonolithMcpSessionTracker::Get();
	Tracker.ResetForTests();

	for (int32 Index = 0; Index < 140; ++Index)
	{
		Tracker.ObserveRequest(
			FString::Printf(TEXT("session_%03d"), Index),
			TEXT("2025-03-26"),
			TEXT("tools/call"),
			TEXT("mesh_query"));
	}

	TSharedPtr<FJsonObject> Result = Tracker.ListSessionsJson(1000);
	TestEqual(TEXT("Session count is capped"), Result->GetIntegerField(TEXT("session_count")), Result->GetIntegerField(TEXT("session_capacity")));
	TestEqual(TEXT("Returned count is capped"), Result->GetIntegerField(TEXT("returned_count")), Result->GetIntegerField(TEXT("session_capacity")));

	Tracker.ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
