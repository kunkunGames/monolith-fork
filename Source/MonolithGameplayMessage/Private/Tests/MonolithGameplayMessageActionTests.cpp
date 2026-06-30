#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

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
	}

	TestTrue(TEXT("gameplay_message.get_status registered"), bHasStatus);
	TestTrue(TEXT("gameplay_message.describe_listener_contract registered"), bHasDescribe);
	TestTrue(TEXT("gameplay_message.validate_message_struct registered"), bHasValidateStruct);
	TestTrue(TEXT("gameplay_message.validate_channel_contract registered"), bHasValidateChannel);

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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
