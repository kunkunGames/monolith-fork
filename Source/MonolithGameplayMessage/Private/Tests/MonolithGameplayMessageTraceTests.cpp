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

	TSharedPtr<FJsonObject> FindObjectByStringField(
		const TSharedPtr<FJsonObject>& Result,
		const TCHAR* ArrayField,
		const TCHAR* StringField,
		const FString& Expected)
	{
		if (!Result.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Result->TryGetArrayField(ArrayField, Rows) || !Rows)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (Value.IsValid()
				&& Value->TryGetObject(Row)
				&& Row
				&& Row->IsValid()
				&& (*Row)->GetStringField(StringField).Equals(
					Expected,
					ESearchCase::CaseSensitive))
			{
				return *Row;
			}
		}
		return nullptr;
	}

	bool HasIssueCode(
		const TSharedPtr<FJsonObject>& Result,
		const FString& ExpectedCode)
	{
		return FindObjectByStringField(
			Result,
			TEXT("issues"),
			TEXT("code"),
			ExpectedCode).IsValid();
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
					TestFalse(
						TEXT("function context is absent by default"),
						(*Match)->HasField(TEXT("function_context")));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> ExcerptParams = MakeShared<FJsonObject>();
	ExcerptParams->SetStringField(TEXT("source_root"), FixtureDirectory());
	ExcerptParams->SetBoolField(TEXT("include_monolith_source"), true);
	ExcerptParams->SetBoolField(TEXT("include_line_text"), true);
	ExcerptParams->SetStringField(
		TEXT("channel_tag"),
		TEXT("TAG_Monolith_GameplayMessage_Fixture"));
	ExcerptParams->SetNumberField(TEXT("max_files"), 10);
	ExcerptParams->SetNumberField(TEXT("max_results"), 10);

	FMonolithActionResult Excerpt = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("trace_channel_usage"),
		ExcerptParams);
	TestTrue(TEXT("opt-in source excerpt trace succeeds"), Excerpt.bSuccess);
	TestTrue(
		TEXT("opt-in source excerpt trace returns JSON"),
		Excerpt.Result.IsValid());
	if (Excerpt.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Matches = nullptr;
		TestTrue(
			TEXT("opt-in source excerpt trace returns matches"),
			Excerpt.Result->TryGetArrayField(TEXT("matches"), Matches)
				&& Matches);
		if (Matches)
		{
			for (const TSharedPtr<FJsonValue>& MatchValue : *Matches)
			{
				const TSharedPtr<FJsonObject>* Match = nullptr;
				if (MatchValue.IsValid()
					&& MatchValue->TryGetObject(Match)
					&& Match
					&& Match->IsValid())
				{
					TestTrue(
						TEXT("line text is present only after opt-in"),
						(*Match)->HasField(TEXT("line_text")));
					TestTrue(
						TEXT("function context is present only after opt-in"),
						(*Match)->HasField(TEXT("function_context")));
				}
			}
		}
	}

	TSharedPtr<FJsonObject> EngineSourceParams = MakeShared<FJsonObject>();
	EngineSourceParams->SetStringField(
		TEXT("source_root"),
		FixtureDirectory());
	EngineSourceParams->SetBoolField(TEXT("include_monolith_source"), true);
	EngineSourceParams->SetBoolField(
		TEXT("include_engine_gameplay_message_sources"),
		true);
	EngineSourceParams->SetStringField(
		TEXT("channel_tag"),
		TEXT("TAG_Monolith_GameplayMessage_Fixture"));
	EngineSourceParams->SetNumberField(TEXT("max_files"), 5000);
	EngineSourceParams->SetNumberField(TEXT("max_results"), 10);
	FMonolithActionResult EngineSource = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("trace_channel_usage"),
		EngineSourceParams);
	TestTrue(
		*FString::Printf(
			TEXT("opt-in GameplayMessageRouter source resolves: %s"),
			*EngineSource.ErrorMessage),
		EngineSource.bSuccess);
	if (EngineSource.Result.IsValid())
	{
		TestTrue(
			TEXT("opt-in engine source adds a second canonical root"),
			EngineSource.Result->GetNumberField(TEXT("roots_checked")) >= 2);
	}

	const auto RunFilteredFixtureTrace =
		[&Registry](const FString& Channel)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("source_root"), FixtureDirectory());
			Params->SetBoolField(TEXT("include_monolith_source"), true);
			Params->SetStringField(TEXT("channel_tag"), Channel);
			Params->SetNumberField(TEXT("max_files"), 10);
			Params->SetNumberField(TEXT("max_results"), 10);
			return Registry.ExecuteAction(
				TEXT("gameplay_message"),
				TEXT("trace_channel_usage"),
				Params);
		};

	FMonolithActionResult MultiA = RunFilteredFixtureTrace(
		TEXT("TAG_Monolith_GameplayMessage_MultiA"));
	FMonolithActionResult MultiB = RunFilteredFixtureTrace(
		TEXT("TAG_Monolith_GameplayMessage_MultiB"));
	TestTrue(TEXT("first same-line call trace succeeds"), MultiA.bSuccess);
	TestTrue(TEXT("second same-line call trace succeeds"), MultiB.bSuccess);
	TSharedPtr<FJsonObject> MultiAMatch = FindObjectByStringField(
		MultiA.Result,
		TEXT("matches"),
		TEXT("channel"),
		TEXT("TAG_Monolith_GameplayMessage_MultiA"));
	TSharedPtr<FJsonObject> MultiBMatch = FindObjectByStringField(
		MultiB.Result,
		TEXT("matches"),
		TEXT("channel"),
		TEXT("TAG_Monolith_GameplayMessage_MultiB"));
	TestTrue(TEXT("first same-line call is reported"), MultiAMatch.IsValid());
	TestTrue(TEXT("second same-line call is reported"), MultiBMatch.IsValid());
	if (MultiAMatch.IsValid())
	{
		TestEqual(
			TEXT("first same-line channel binds its own payload"),
			MultiAMatch->GetStringField(TEXT("payload_candidate")),
			FString(TEXT("FMonolithGameplayMessageMultiPayloadA")));
	}
	if (MultiBMatch.IsValid())
	{
		TestEqual(
			TEXT("second same-line channel binds its own payload"),
			MultiBMatch->GetStringField(TEXT("payload_candidate")),
			FString(TEXT("FMonolithGameplayMessageMultiPayloadB")));
	}

	FMonolithActionResult RootTag = RunFilteredFixtureTrace(TEXT("Message"));
	TestTrue(TEXT("single-segment literal channel trace succeeds"), RootTag.bSuccess);
	TSharedPtr<FJsonObject> RootTagMatch = FindObjectByStringField(
		RootTag.Result,
		TEXT("matches"),
		TEXT("channel"),
		TEXT("Message"));
	TestTrue(
		TEXT("single-segment literal channel is reported"),
		RootTagMatch.IsValid());
	if (RootTagMatch.IsValid())
	{
		TestEqual(
			TEXT("single-segment literal channel binds its payload"),
			RootTagMatch->GetStringField(TEXT("payload_candidate")),
			FString(TEXT("FMonolithGameplayMessageRootPayload")));
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

		const TSharedPtr<FJsonObject>* Summary = nullptr;
		TestTrue(
			TEXT("limited fixture trace has a summary"),
			Limited.Result->TryGetObjectField(TEXT("summary"), Summary)
				&& Summary);
		if (Summary && Summary->IsValid())
		{
			TestFalse(
				TEXT("truncated scan marks orphan analysis indeterminate"),
				(*Summary)->GetBoolField(TEXT("orphan_analysis_complete")));
			TestEqual(
				TEXT("truncated scan does not claim orphan broadcasters"),
				static_cast<int32>((*Summary)->GetNumberField(
					TEXT("orphan_broadcaster_candidate_count"))),
				0);
			TestEqual(
				TEXT("truncated scan does not claim orphan listeners"),
				static_cast<int32>((*Summary)->GetNumberField(
					TEXT("orphan_listener_candidate_count"))),
				0);
		}

		const TArray<TSharedPtr<FJsonValue>>* ChannelGraph = nullptr;
		TestTrue(
			TEXT("limited fixture trace has a channel graph"),
			Limited.Result->TryGetArrayField(
				TEXT("channel_graph"),
				ChannelGraph)
				&& ChannelGraph);
		if (ChannelGraph)
		{
			for (const TSharedPtr<FJsonValue>& ChannelValue : *ChannelGraph)
			{
				const TSharedPtr<FJsonObject>* Channel = nullptr;
				if (ChannelValue.IsValid()
					&& ChannelValue->TryGetObject(Channel)
					&& Channel
					&& Channel->IsValid())
				{
					TestFalse(
						TEXT("truncated channel does not claim an orphan broadcaster"),
						(*Channel)->GetBoolField(
							TEXT("orphan_broadcaster_candidate")));
					TestFalse(
						TEXT("truncated channel does not claim an orphan listener"),
						(*Channel)->GetBoolField(
							TEXT("orphan_listener_candidate")));
					TestEqual(
						TEXT("truncated channel reports indeterminate orphan status"),
						(*Channel)->GetStringField(
							TEXT("orphan_analysis_status")),
						FString(TEXT("indeterminate")));
				}
			}
		}

		TestTrue(
			TEXT("truncated scan reports explicit indeterminate absence analysis"),
			HasIssueCode(
				Limited.Result,
				TEXT("absence_analysis_indeterminate")));
		TestFalse(
			TEXT("truncated scan does not emit an orphan broadcaster issue"),
			HasIssueCode(
				Limited.Result,
				TEXT("orphan_broadcaster_candidate")));
		TestFalse(
			TEXT("truncated scan does not emit an orphan listener issue"),
			HasIssueCode(
				Limited.Result,
				TEXT("orphan_listener_candidate")));
	}

	const FString NestedPluginDescriptor = FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("GameFeatures/MonolithNestedTraceFixture/MonolithNestedTraceFixture.uplugin"));
	if (FPaths::FileExists(NestedPluginDescriptor))
	{
		TSharedPtr<FJsonObject> NestedPluginParams = MakeShared<FJsonObject>();
		NestedPluginParams->SetStringField(
			TEXT("channel_tag"),
			TEXT("TAG_Monolith_GameplayMessage_NestedPluginFixture"));
		NestedPluginParams->SetNumberField(TEXT("max_files"), 100);
		NestedPluginParams->SetNumberField(TEXT("max_results"), 10);
		FMonolithActionResult NestedPlugin = Registry.ExecuteAction(
			TEXT("gameplay_message"),
			TEXT("trace_channel_usage"),
			NestedPluginParams);
		TestTrue(
			*FString::Printf(
				TEXT("default nested project plugin trace succeeds: %s"),
				*NestedPlugin.ErrorMessage),
			NestedPlugin.bSuccess);
		TSharedPtr<FJsonObject> NestedMatch = FindObjectByStringField(
			NestedPlugin.Result,
			TEXT("matches"),
			TEXT("channel"),
			TEXT("TAG_Monolith_GameplayMessage_NestedPluginFixture"));
		TestTrue(
			TEXT("default roots discover nested project plugin source"),
			NestedMatch.IsValid());
	}
	else
	{
		AddInfo(
			TEXT("Nested project-plugin fixture is not installed; optional default-root discovery assertion skipped."));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
