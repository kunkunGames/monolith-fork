#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithGameplayMessageActions.h"

#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayMessageRegistryTest,
	"Monolith.GameplayMessage.RegistryAndSchemas",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayMessageRegistryTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("gameplay_message"), TEXT("get_status")))
	{
		FMonolithGameplayMessageActions::RegisterActions(Registry);
	}

	const TArray<FString> ExpectedActions =
	{
		TEXT("get_status"),
		TEXT("describe_listener_contract"),
		TEXT("validate_message_struct"),
		TEXT("validate_channel_contract"),
		TEXT("trace_channel_usage")
	};

	for (const FString& Action : ExpectedActions)
	{
		TestTrue(
			*FString::Printf(TEXT("gameplay_message.%s is registered"), *Action),
			Registry.HasAction(TEXT("gameplay_message"), Action));
	}

	TArray<FMonolithActionInfo> Actions = Registry.GetActions(TEXT("gameplay_message"));
	TestEqual(TEXT("gameplay_message registers exactly five actions"), Actions.Num(), 5);

	for (const FMonolithActionInfo& Action : Actions)
	{
		TestTrue(
			*FString::Printf(TEXT("%s has a non-empty description"), *Action.Action),
			!Action.Description.IsEmpty());
		TestTrue(
			*FString::Printf(TEXT("%s has a parameter schema"), *Action.Action),
			Action.ParamSchema.IsValid());
	}

	const FMonolithActionInfo* StructValidation = Actions.FindByPredicate(
		[](const FMonolithActionInfo& Action)
		{
			return Action.Action == TEXT("validate_message_struct");
		});
	TestNotNull(TEXT("validate_message_struct action info exists"), StructValidation);
	if (StructValidation && StructValidation->ParamSchema.IsValid())
	{
		const TSharedPtr<FJsonObject>* MessageStructSchema = nullptr;
		TestTrue(
			TEXT("message_struct schema exists"),
			StructValidation->ParamSchema->TryGetObjectField(
				TEXT("message_struct"),
				MessageStructSchema)
				&& MessageStructSchema
				&& MessageStructSchema->IsValid());
		if (MessageStructSchema && MessageStructSchema->IsValid())
		{
			bool bRequired = false;
			(*MessageStructSchema)->TryGetBoolField(TEXT("required"), bRequired);
			TestTrue(TEXT("message_struct is required"), bRequired);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
