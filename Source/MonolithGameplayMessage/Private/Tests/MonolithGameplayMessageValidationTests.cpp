#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithGameplayMessageActions.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameplayMessageValidationTest,
	"Monolith.GameplayMessage.ValidationContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameplayMessageValidationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = GameplayMessageRegistry();

	FMonolithActionResult Status = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("get_status"),
		MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), Status.bSuccess);
	TestTrue(TEXT("get_status returns JSON"), Status.Result.IsValid());
	if (Status.Result.IsValid())
	{
		TestTrue(TEXT("status reports plugins"), Status.Result->HasField(TEXT("plugins")));
		TestTrue(TEXT("status reports modules"), Status.Result->HasField(TEXT("modules")));
		TestTrue(TEXT("status reports exact enum status"), Status.Result->HasField(TEXT("match_enum")));
	}

	FMonolithActionResult ListenerContract = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("describe_listener_contract"),
		MakeShared<FJsonObject>());
	TestTrue(TEXT("describe_listener_contract succeeds"), ListenerContract.bSuccess);
	TestTrue(TEXT("describe_listener_contract returns JSON"), ListenerContract.Result.IsValid());
	if (ListenerContract.Result.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MatchTypes = nullptr;
		TestTrue(
			TEXT("listener contract reports match types"),
			ListenerContract.Result->TryGetArrayField(TEXT("match_types"), MatchTypes)
				&& MatchTypes);
		TestEqual(
			TEXT("listener contract has exact and partial match types"),
			MatchTypes ? MatchTypes->Num() : 0,
			2);

		const TArray<TSharedPtr<FJsonValue>>* ListenerRows = nullptr;
		TestTrue(
			TEXT("listener contract reports compatibility rules"),
			ListenerContract.Result->TryGetArrayField(
				TEXT("listener_contract"),
				ListenerRows)
				&& ListenerRows);
		bool bFoundDerivedPayloadCompatibility = false;
		if (ListenerRows)
		{
			for (const TSharedPtr<FJsonValue>& Row : *ListenerRows)
			{
				bFoundDerivedPayloadCompatibility |=
					Row.IsValid()
					&& Row->AsString().Contains(
						TEXT("equal or derive"),
						ESearchCase::CaseSensitive)
					&& Row->AsString().Contains(
						TEXT("accepted parent type"),
						ESearchCase::CaseSensitive);
			}
		}
		TestTrue(
			TEXT("listener contract allows derived broadcast payload structs"),
			bFoundDerivedPayloadCompatibility);
	}

	TSharedPtr<FJsonObject> ValidStructParams = MakeShared<FJsonObject>();
	ValidStructParams->SetStringField(
		TEXT("message_struct"),
		TEXT("/Script/GameplayTags.GameplayTagContainer"));
	FMonolithActionResult ValidStruct = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("validate_message_struct"),
		ValidStructParams);
	TestTrue(TEXT("native exact script struct validation executes"), ValidStruct.bSuccess);
	TestTrue(TEXT("native exact script struct validation returns JSON"), ValidStruct.Result.IsValid());
	if (ValidStruct.Result.IsValid())
	{
		TestTrue(TEXT("GameplayTagContainer is a valid script struct"), ValidStruct.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* StructReport = nullptr;
		TestTrue(
			TEXT("struct report exists"),
			ValidStruct.Result->TryGetObjectField(TEXT("message_struct"), StructReport)
				&& StructReport);
		if (StructReport && StructReport->IsValid())
		{
			TestTrue(
				TEXT("struct path resolved exactly"),
				(*StructReport)->GetBoolField(TEXT("found_exact")));
			TestEqual(
				TEXT("resolved struct path preserves exact identity"),
				(*StructReport)->GetStringField(TEXT("resolved_path")),
				FString(TEXT("/Script/GameplayTags.GameplayTagContainer")));
			TestTrue(
				TEXT("struct report includes a positive structure size"),
				(*StructReport)->GetNumberField(TEXT("structure_size")) > 0);
			TestTrue(
				TEXT("struct report declares recursive object-reference scanning"),
				(*StructReport)->GetBoolField(
					TEXT("object_reference_scan_recursive")));
		}
	}

	TSharedPtr<FJsonObject> NestedObjectReferenceParams =
		MakeShared<FJsonObject>();
	NestedObjectReferenceParams->SetStringField(
		TEXT("message_struct"),
		TEXT("/Script/Engine.PooledCameraShakes"));
	NestedObjectReferenceParams->SetBoolField(
		TEXT("require_no_object_references"),
		true);
	FMonolithActionResult NestedObjectReference = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("validate_message_struct"),
		NestedObjectReferenceParams);
	TestTrue(
		TEXT("nested object-reference struct validation executes"),
		NestedObjectReference.bSuccess);
	TestTrue(
		TEXT("nested object-reference struct validation returns JSON"),
		NestedObjectReference.Result.IsValid());
	if (NestedObjectReference.Result.IsValid())
	{
		TestFalse(
			TEXT("nested array object references violate the requested policy"),
			NestedObjectReference.Result->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* StructReport = nullptr;
		TestTrue(
			TEXT("nested object-reference struct report exists"),
			NestedObjectReference.Result->TryGetObjectField(
				TEXT("message_struct"),
				StructReport)
				&& StructReport);
		if (StructReport && StructReport->IsValid())
		{
			TestTrue(
				TEXT("nested object-reference property is counted"),
				(*StructReport)->GetNumberField(
					TEXT("object_property_count")) >= 1);
			TestTrue(
				TEXT("nested object-reference report is recursive"),
				(*StructReport)->GetBoolField(
					TEXT("object_reference_scan_recursive")));
		}
	}

	TSharedPtr<FJsonObject> WrongTypeParams = MakeShared<FJsonObject>();
	WrongTypeParams->SetStringField(TEXT("message_struct"), TEXT("/Script/Engine.Actor"));
	FMonolithActionResult WrongType = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("validate_message_struct"),
		WrongTypeParams);
	TestTrue(TEXT("wrong object type returns structured validation"), WrongType.bSuccess);
	TestTrue(TEXT("wrong object type returns JSON"), WrongType.Result.IsValid());
	if (WrongType.Result.IsValid())
	{
		TestFalse(TEXT("UClass is not accepted as a message struct"), WrongType.Result->GetBoolField(TEXT("ok")));
	}

	TSharedPtr<FJsonObject> MissingStructParams = MakeShared<FJsonObject>();
	MissingStructParams->SetStringField(
		TEXT("message_struct"),
		TEXT("/Script/GameplayTags.MonolithDefinitelyMissingStruct"));
	FMonolithActionResult MissingStruct = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("validate_message_struct"),
		MissingStructParams);
	TestTrue(TEXT("missing exact object returns structured validation"), MissingStruct.bSuccess);
	TestTrue(TEXT("missing exact object returns JSON"), MissingStruct.Result.IsValid());
	if (MissingStruct.Result.IsValid())
	{
		TestFalse(TEXT("missing exact object is invalid"), MissingStruct.Result->GetBoolField(TEXT("ok")));
	}

	TSharedPtr<FJsonObject> PreflightChannelParams = MakeShared<FJsonObject>();
	PreflightChannelParams->SetStringField(
		TEXT("channel_tag"),
		TEXT("Monolith.GameplayMessage.UnregisteredPreflight"));
	PreflightChannelParams->SetBoolField(TEXT("require_registered_tag"), false);
	PreflightChannelParams->SetStringField(
		TEXT("message_struct"),
		TEXT("/Script/GameplayTags.GameplayTagContainer"));
	FMonolithActionResult PreflightChannel = Registry.ExecuteAction(
		TEXT("gameplay_message"),
		TEXT("validate_channel_contract"),
		PreflightChannelParams);
	TestTrue(TEXT("unregistered preflight channel executes"), PreflightChannel.bSuccess);
	TestTrue(TEXT("unregistered preflight channel returns JSON"), PreflightChannel.Result.IsValid());
	if (PreflightChannel.Result.IsValid())
	{
		TestTrue(
			TEXT("unregistered preflight channel remains valid when explicitly allowed"),
			PreflightChannel.Result->GetBoolField(TEXT("ok")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
