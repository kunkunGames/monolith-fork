#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithGameplayMessageActions.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"

namespace
{
	FMonolithToolRegistry& GameplayMessageRegistry()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("gameplay_message"), TEXT("get_status")))
		{
			FMonolithGameplayMessageActions::RegisterActions(Registry);
		}
		return Registry;
	}

	FString FixtureDirectory()
	{
		FString Directory = FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("Monolith/Source/MonolithGameplayMessage/Private/Tests/Fixtures"));
		Directory = FPaths::ConvertRelativePathToFull(Directory);
		FPaths::NormalizeDirectoryName(Directory);
		return Directory;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayMessageTraceTest,
	"Monolith.GameplayMessage.BoundedSourceTrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayMessageTraceTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = GameplayMessageRegistry();

	TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
	TraceParams->SetStringField(TEXT("source_root"), FixtureDirectory());
	TraceParams->SetBoolField(TEXT("include_monolith_source"), true);
	TraceParams->SetStringField(
		TEXT("channel_tag"),
		TEXT("TAG_Monolith_GameplayMessage_Fixture"));
	TraceParams->SetNumberField(TEXT("max_files"), 10);
	TraceParams->SetNumberField(TEXT("max_results"), 10);

	FMonolithActionResult Trace = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("trace_channel_usage"),
		TraceParams);
	TestTrue(TEXT("fixture trace succeeds"), Trace.bSuccess);
	TestTrue(TEXT("fixture trace returns JSON"), Trace.Result.IsValid());
	if (Trace.Result.IsValid())
	{
		TestTrue(TEXT("fixture trace reports ok"), Trace.Result->GetBoolField(TEXT("ok")));
		TestEqual(
			TEXT("fixture trace finds one broadcaster and one listener"),
			static_cast<int32>(Trace.Result->GetNumberField(TEXT("match_count"))),
			2);

		const TSharedPtr<FJsonObject>* Summary = nullptr;
		TestTrue(
			TEXT("fixture trace has a summary"),
			Trace.Result->TryGetObjectField(TEXT("summary"), Summary) && Summary);
		if (Summary && Summary->IsValid())
		{
			TestEqual(
				TEXT("fixture trace groups one channel"),
				static_cast<int32>((*Summary)->GetNumberField(TEXT("channel_count"))),
				1);
			TestEqual(
				TEXT("fixture trace has one broadcaster"),
				static_cast<int32>((*Summary)->GetNumberField(TEXT("broadcaster_count"))),
				1);
			TestEqual(
				TEXT("fixture trace has one listener"),
				static_cast<int32>((*Summary)->GetNumberField(TEXT("listener_count"))),
				1);
			TestEqual(
				TEXT("fixture trace has no payload mismatch"),
				static_cast<int32>(
					(*Summary)->GetNumberField(TEXT("payload_mismatch_candidate_count"))),
				0);
		}

		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		TestTrue(
			TEXT("fixture trace returns matches"),
			Trace.Result->TryGetArrayField(TEXT("matches"), Matches) && Matches);
		if (Matches)
		{
			for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
			{
				const TSharedPtr<FJsonObject>* Match = nullptr;
				TestTrue(
					TEXT("match is an object"),
					MatchValue.IsValid() && MatchValue->TryGetObject(Match) && Match);
				if (Match && Match->IsValid())
				{
					TestFalse(
						TEXT("line text is absent by default"),
						(*Match)->HasField(TEXT("line_text")));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> LimitedParams = MakeShared<FJsonObject>();
	LimitedParams->SetStringField(TEXT("source_root"), FixtureDirectory());
	LimitedParams->SetBoolField(TEXT("include_monolith_source"), true);
	LimitedParams->SetNumberField(TEXT("max_files"), 10);
	LimitedParams->SetNumberField(TEXT("max_results"), 1);

	FMonolithActionResult Limited = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("trace_channel_usage"),
		LimitedParams);
	TestTrue(TEXT("limited fixture trace succeeds"), Limited.bSuccess);
	TestTrue(TEXT("limited fixture trace returns JSON"), Limited.Result.IsValid());
	if (Limited.Result.IsValid())
	{
		TestEqual(
			TEXT("limited fixture trace returns exactly one result"),
			static_cast<int32>(Limited.Result->GetNumberField(TEXT("match_count"))),
			1);
		const TSharedPtr<FJsonObject>* Limits = nullptr;
		TestTrue(
			TEXT("limited fixture trace has limit metadata"),
			Limited.Result->TryGetObjectField(TEXT("limits"), Limits) && Limits);
		if (Limits && Limits->IsValid())
		{
			TestTrue(
				TEXT("limited fixture trace reports result truncation"),
				(*Limits)->GetBoolField(TEXT("result_limit_reached")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
