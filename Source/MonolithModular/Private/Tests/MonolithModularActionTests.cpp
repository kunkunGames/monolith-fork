#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithModularActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithModularRegistryAndValidationTest,
	"Monolith.Modular.RegistryAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithModularRegistryAndValidationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	bool bHasStatus = false;
	bool bHasLifecycle = false;
	bool bHasValidateTargets = false;
	bool bHasTraceEvents = false;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("modular")))
	{
		if (ActionInfo.Action == TEXT("get_status"))
		{
			bHasStatus = true;
		}
		else if (ActionInfo.Action == TEXT("describe_extension_receiver_lifecycle"))
		{
			bHasLifecycle = true;
		}
		else if (ActionInfo.Action == TEXT("validate_add_component_targets"))
		{
			bHasValidateTargets = true;
		}
		else if (ActionInfo.Action == TEXT("trace_game_framework_extension_events"))
		{
			bHasTraceEvents = true;
		}
	}

	TestTrue(TEXT("modular.get_status registered"), bHasStatus);
	TestTrue(TEXT("modular.describe_extension_receiver_lifecycle registered"), bHasLifecycle);
	TestTrue(TEXT("modular.validate_add_component_targets registered"), bHasValidateTargets);
	TestTrue(TEXT("modular.trace_game_framework_extension_events registered"), bHasTraceEvents);

	for (const TCHAR* Action : {
		TEXT("get_status"),
		TEXT("describe_extension_receiver_lifecycle"),
		TEXT("validate_add_component_targets"),
		TEXT("trace_game_framework_extension_events")
	})
	{
		TestEqual(
			FString::Printf(TEXT("modular.%s is read-only"), Action),
			Registry.GetActionExecutionPolicy(TEXT("modular"), Action).PolicyId,
			FString(TEXT("read_only")));
	}

	const FMonolithActionResult StatusResult = FMonolithModularActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), StatusResult.bSuccess);
	TestTrue(TEXT("get_status result object is valid"), StatusResult.Result.IsValid());
	if (StatusResult.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* KnownLifecycle = nullptr;
		TestTrue(TEXT("known receiver lifecycle array exists"), StatusResult.Result->TryGetArrayField(TEXT("known_receiver_lifecycle"), KnownLifecycle) && KnownLifecycle && KnownLifecycle->Num() >= 8);
	}

	TSharedPtr<FJsonObject> LifecycleParams = MakeShared<FJsonObject>();
	LifecycleParams->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	const FMonolithActionResult LifecycleResult = FMonolithModularActions::DescribeExtensionReceiverLifecycle(LifecycleParams);
	TestTrue(TEXT("describe_extension_receiver_lifecycle succeeds for Actor"), LifecycleResult.bSuccess);
	TestTrue(TEXT("describe_extension_receiver_lifecycle result object is valid"), LifecycleResult.Result.IsValid());
	if (LifecycleResult.Result.IsValid())
	{
		const TSharedPtr<FJsonObject>* Classification = nullptr;
		TestTrue(TEXT("classification object exists"), LifecycleResult.Result->TryGetObjectField(TEXT("classification"), Classification) && Classification && Classification->IsValid());
	}

	TSharedPtr<FJsonObject> ValidParams = MakeShared<FJsonObject>();
	ValidParams->SetStringField(TEXT("actor_class"), TEXT("/Script/ModularGameplayActors.ModularCharacter"));
	ValidParams->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.StaticMeshComponent"));
	const FMonolithActionResult ValidResult = FMonolithModularActions::ValidateAddComponentTargets(ValidParams);
	TestTrue(TEXT("validate_add_component_targets succeeds for known modular receiver"), ValidResult.bSuccess);
	TestTrue(TEXT("valid target result object is valid"), ValidResult.Result.IsValid());
	if (ValidResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("valid target ok field exists"), ValidResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("known modular receiver target is ok"), bOk);
	}

	TSharedPtr<FJsonObject> PlainActorParams = MakeShared<FJsonObject>();
	PlainActorParams->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	PlainActorParams->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.StaticMeshComponent"));
	const FMonolithActionResult PlainActorResult = FMonolithModularActions::ValidateAddComponentTargets(PlainActorParams);
	TestTrue(TEXT("validate_add_component_targets returns structured result for plain Actor"), PlainActorResult.bSuccess);
	TestTrue(TEXT("plain Actor result object is valid"), PlainActorResult.Result.IsValid());
	if (PlainActorResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("plain Actor ok field exists"), PlainActorResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("plain Actor receiver lifecycle is not proven by default"), bOk);
	}

	TSharedPtr<FJsonObject> BadComponentParams = MakeShared<FJsonObject>();
	BadComponentParams->SetStringField(TEXT("actor_class"), TEXT("/Script/ModularGameplayActors.ModularCharacter"));
	BadComponentParams->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.Actor"));
	const FMonolithActionResult BadComponentResult = FMonolithModularActions::ValidateAddComponentTargets(BadComponentParams);
	TestTrue(TEXT("validate_add_component_targets returns structured result for invalid component class"), BadComponentResult.bSuccess);
	TestTrue(TEXT("bad component result object is valid"), BadComponentResult.Result.IsValid());
	if (BadComponentResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("bad component ok field exists"), BadComponentResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("Actor is not a valid component class"), bOk);

		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		TestTrue(TEXT("bad component issues array exists"), BadComponentResult.Result->TryGetArrayField(TEXT("issues"), Issues) && Issues && Issues->Num() > 0);
	}

	TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
	TraceParams->SetStringField(TEXT("source_root"), TEXT("Source/LyraGame"));
	TraceParams->SetNumberField(TEXT("max_results"), 50);
	const FMonolithActionResult TraceResult = FMonolithModularActions::TraceGameFrameworkExtensionEvents(TraceParams);
	TestTrue(TEXT("trace_game_framework_extension_events succeeds for LyraGame source"), TraceResult.bSuccess);
	TestTrue(TEXT("trace result object is valid"), TraceResult.Result.IsValid());
	if (TraceResult.Result.IsValid())
	{
		double MatchCount = 0.0;
		TestTrue(TEXT("trace match_count field exists"), TraceResult.Result->TryGetNumberField(TEXT("match_count"), MatchCount));
		TestTrue(TEXT("trace finds Lyra modular call sites"), MatchCount > 0.0);

		const TArray<TSharedPtr<FJsonValue>>* EventTokens = nullptr;
		TestTrue(TEXT("trace event_tokens array exists"), TraceResult.Result->TryGetArrayField(TEXT("event_tokens"), EventTokens) && EventTokens);
		const TArray<TSharedPtr<FJsonValue>>* EventSenders = nullptr;
		TestTrue(TEXT("trace event_senders array exists"), TraceResult.Result->TryGetArrayField(TEXT("event_senders"), EventSenders) && EventSenders && EventSenders->Num() > 0);
		const TArray<TSharedPtr<FJsonValue>>* Limitations = nullptr;
		TestTrue(TEXT("trace limitations array exists"), TraceResult.Result->TryGetArrayField(TEXT("limitations"), Limitations) && Limitations && Limitations->Num() > 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
