#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "MonolithGameplayMessageActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayMessageRegistryAndValidationTest,
	"Monolith.GameplayMessage.RegistryAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayMessageRegistryAndValidationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	bool bHasStatus = false;
	bool bHasDescribe = false;
	bool bHasValidateStruct = false;
	bool bHasValidateChannel = false;
	bool bHasTraceChannelUsage = false;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("gameplay_message")))
	{
		if (ActionInfo.Action == TEXT("get_status"))
		{
			bHasStatus = true;
		}
		else if (ActionInfo.Action == TEXT("describe_listener_contract"))
		{
			bHasDescribe = true;
		}
		else if (ActionInfo.Action == TEXT("validate_message_struct"))
		{
			bHasValidateStruct = true;
		}
		else if (ActionInfo.Action == TEXT("validate_channel_contract"))
		{
			bHasValidateChannel = true;
		}
		else if (ActionInfo.Action == TEXT("trace_channel_usage"))
		{
			bHasTraceChannelUsage = true;
		}
	}

	TestTrue(TEXT("gameplay_message.get_status registered"), bHasStatus);
	TestTrue(TEXT("gameplay_message.describe_listener_contract registered"), bHasDescribe);
	TestTrue(TEXT("gameplay_message.validate_message_struct registered"), bHasValidateStruct);
	TestTrue(TEXT("gameplay_message.validate_channel_contract registered"), bHasValidateChannel);
	TestTrue(TEXT("gameplay_message.trace_channel_usage registered"), bHasTraceChannelUsage);

	const FMonolithActionResult StatusResult = FMonolithGameplayMessageActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), StatusResult.bSuccess);
	TestTrue(TEXT("get_status result object is valid"), StatusResult.Result.IsValid());

	const FMonolithActionResult ContractResult = FMonolithGameplayMessageActions::DescribeListenerContract(MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_listener_contract succeeds"), ContractResult.bSuccess);
	TestTrue(TEXT("describe_listener_contract result object is valid"), ContractResult.Result.IsValid());

	TSharedPtr<FJsonObject> StructParams = MakeShared<FJsonObject>();
	StructParams->SetStringField(TEXT("message_struct"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"));
	StructParams->SetBoolField(TEXT("require_blueprint_type"), true);
	const FMonolithActionResult StructResult = FMonolithGameplayMessageActions::ValidateMessageStruct(StructParams);
	TestTrue(TEXT("validate_message_struct returns structured result"), StructResult.bSuccess);
	TestTrue(TEXT("valid struct result object is valid"), StructResult.Result.IsValid());
	if (StructResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("valid struct ok field exists"), StructResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("GameplayMessageListenerHandle is accepted as a BlueprintType struct"), bOk);
	}

	TSharedPtr<FJsonObject> ChannelParams = MakeShared<FJsonObject>();
	ChannelParams->SetStringField(TEXT("channel_tag"), TEXT("Monolith.Test.Channel"));
	ChannelParams->SetStringField(TEXT("message_struct"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle"));
	ChannelParams->SetStringField(TEXT("match_type"), TEXT("PartialMatch"));
	ChannelParams->SetBoolField(TEXT("require_registered_tag"), false);
	const FMonolithActionResult ChannelResult = FMonolithGameplayMessageActions::ValidateChannelContract(ChannelParams);
	TestTrue(TEXT("validate_channel_contract returns structured result"), ChannelResult.bSuccess);
	TestTrue(TEXT("valid unregistered channel with require_registered_tag=false has result object"), ChannelResult.Result.IsValid());
	if (ChannelResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("channel ok field exists"), ChannelResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("unregistered channel is allowed when explicitly requested"), bOk);
	}

	TSharedPtr<FJsonObject> BadStructParams = MakeShared<FJsonObject>();
	BadStructParams->SetStringField(TEXT("channel_tag"), TEXT("Monolith.Test.Channel"));
	BadStructParams->SetStringField(TEXT("message_struct"), TEXT("/Script/Engine.Actor"));
	BadStructParams->SetBoolField(TEXT("require_registered_tag"), false);
	const FMonolithActionResult BadStructResult = FMonolithGameplayMessageActions::ValidateChannelContract(BadStructParams);
	TestTrue(TEXT("validate_channel_contract returns structured result for wrong struct object type"), BadStructResult.bSuccess);
	TestTrue(TEXT("wrong struct object type result object is valid"), BadStructResult.Result.IsValid());
	if (BadStructResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("wrong struct object type ok field exists"), BadStructResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("wrong struct object type fails channel contract"), bOk);
	}

	const FString TraceRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MonolithGameplayMessageTrace"), FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TestTrue(TEXT("trace source root directory is created"), IFileManager::Get().MakeDirectory(*TraceRoot, true));
	const FString TraceFile = FPaths::Combine(TraceRoot, TEXT("TraceFixture.cpp"));
	const FString TraceSource = TEXT(
		"void BroadcastFoo() { Subsystem->BroadcastMessage<FMyGameplayPayload>(FGameplayTag::RequestGameplayTag(TEXT(\"Monolith.Test.Channel\")), Payload); }\n"
		"void ListenFoo() { Subsystem->RegisterListener<FMyGameplayPayload>(FGameplayTag::RequestGameplayTag(TEXT(\"Monolith.Test.Channel\")), this, &ThisClass::OnMessage, EGameplayMessageMatch::ExactMatch); }\n"
		"void ListenOther() { Subsystem->RegisterListener<FOtherGameplayPayload>(FGameplayTag::RequestGameplayTag(TEXT(\"Monolith.Test.Channel\")), this, &ThisClass::OnOtherMessage, EGameplayMessageMatch::PartialMatch); }\n");
	TestTrue(TEXT("trace fixture source file is written"), FFileHelper::SaveStringToFile(TraceSource, *TraceFile));

	TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
	TraceParams->SetStringField(TEXT("source_root"), TraceRoot);
	TraceParams->SetNumberField(TEXT("max_files"), 10);
	TraceParams->SetNumberField(TEXT("max_results"), 20);
	const FMonolithActionResult TraceResult = FMonolithGameplayMessageActions::TraceChannelUsage(TraceParams);
	TestTrue(TEXT("trace_channel_usage returns structured result"), TraceResult.bSuccess);
	TestTrue(TEXT("trace_channel_usage result object is valid"), TraceResult.Result.IsValid());
	if (TraceResult.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* Summary = nullptr;
		TestTrue(TEXT("trace_channel_usage includes summary"), TraceResult.Result->TryGetObjectField(TEXT("summary"), Summary) && Summary && Summary->IsValid());
		if (Summary && Summary->IsValid())
		{
			double BroadcasterCount = 0.0;
			double ListenerCount = 0.0;
			double PayloadMismatchCount = 0.0;
			double MatchAmbiguityCount = 0.0;
			(*Summary)->TryGetNumberField(TEXT("broadcaster_count"), BroadcasterCount);
			(*Summary)->TryGetNumberField(TEXT("listener_count"), ListenerCount);
			(*Summary)->TryGetNumberField(TEXT("payload_mismatch_candidate_count"), PayloadMismatchCount);
			(*Summary)->TryGetNumberField(TEXT("match_type_ambiguity_candidate_count"), MatchAmbiguityCount);
			TestEqual(TEXT("trace fixture reports one broadcaster"), static_cast<int32>(BroadcasterCount), 1);
			TestEqual(TEXT("trace fixture reports two listeners"), static_cast<int32>(ListenerCount), 2);
			TestEqual(TEXT("trace fixture reports payload mismatch candidate"), static_cast<int32>(PayloadMismatchCount), 1);
			TestEqual(TEXT("trace fixture reports match ambiguity candidate"), static_cast<int32>(MatchAmbiguityCount), 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* ChannelGraph = nullptr;
		TestTrue(TEXT("trace_channel_usage includes channel graph"), TraceResult.Result->TryGetArrayField(TEXT("channel_graph"), ChannelGraph) && ChannelGraph);
		TestTrue(TEXT("trace_channel_usage found at least one channel row"), ChannelGraph && ChannelGraph->Num() > 0);
	}

	IFileManager::Get().DeleteDirectory(*TraceRoot, false, true);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
