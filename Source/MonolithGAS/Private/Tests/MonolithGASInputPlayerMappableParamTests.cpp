#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithGASInputAssetActions.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"

namespace
{
struct FMonolithGASInputValidationFixture
{
	FMonolithGASInputValidationFixture()
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Package = CreatePackage(*FString::Printf(TEXT("/Engine/Transient/MonolithInputValidation_%s"), *Suffix));
	}

	~FMonolithGASInputValidationFixture()
	{
		for (UObject* Object : CreatedObjects)
		{
			if (Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	UInputAction* CreateAction(const FName Name)
	{
		UInputAction* Action =
			Package ? NewObject<UInputAction>(Package, Name, RF_Public | RF_Standalone | RF_Transient) : nullptr;
		CreatedObjects.Add(Action);
		return Action;
	}

	UInputMappingContext* CreateContext(const FName Name)
	{
		UInputMappingContext* Context =
			Package ? NewObject<UInputMappingContext>(Package, Name, RF_Public | RF_Standalone | RF_Transient)
					: nullptr;
		CreatedObjects.Add(Context);
		return Context;
	}

	UPackage* Package = nullptr;
	TArray<UObject*> CreatedObjects;
};

void EnsureMonolithGASInputActionsRegistered()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("input"), TEXT("validate_input_mappings")))
	{
		FMonolithGASInputAssetActions::RegisterActions(Registry);
	}
}

TSharedPtr<FJsonObject> MakeMonolithGASInputValidationParams(const UInputMappingContext* Context,
															 const bool bFailOnUnbound = false)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ContextPaths;
	ContextPaths.Add(MakeShared<FJsonValueString>(Context ? Context->GetPathName() : TEXT("")));
	Params->SetArrayField(TEXT("context_paths"), ContextPaths);
	Params->SetBoolField(TEXT("fail_on_unbound"), bFailOnUnbound);
	return Params;
}

TSharedPtr<FJsonObject> GetOnlyMonolithGASInputValidationContext(const FMonolithActionResult& Result)
{
	if (!Result.Result.IsValid())
	{
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* Contexts = nullptr;
	if (!Result.Result->TryGetArrayField(TEXT("contexts"), Contexts) || !Contexts || Contexts->Num() != 1)
	{
		return nullptr;
	}
	return (*Contexts)[0]->AsObject();
}

bool MonolithGASInputValidationHasIssueType(const TSharedPtr<FJsonObject>& ContextResult, const FString& ExpectedType)
{
	if (!ContextResult.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	if (!ContextResult->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
	{
		const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
		if (Issue.IsValid() && Issue->GetStringField(TEXT("type")) == ExpectedType)
		{
			return true;
		}
	}
	return false;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputPlayerMappableParamsRejectInvalidMetadataTest,
								 "Monolith.ParamGuard.GAS.InputPlayerMappableParamsRejectInvalidMetadata",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInputPlayerMappableParamsRejectInvalidMetadataTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("input"), TEXT("add_input_mapping")))
	{
		FMonolithGASInputAssetActions::RegisterActions(Registry);
	}

	auto MakeBaseParams = []()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("context_path"), TEXT("/Game/Input/IMC_ParamGuard"));
		Params->SetStringField(TEXT("action_path"), TEXT("/Game/Input/IA_ParamGuard"));
		Params->SetStringField(TEXT("key"), TEXT("SpaceBar"));
		return Params;
	};

	{
		TSharedPtr<FJsonObject> Params = MakeBaseParams();
		Params->SetStringField(TEXT("player_mappable"), TEXT("true"));

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("input"), TEXT("add_input_mapping"), Params);
		TestFalse(TEXT("player_mappable must reject non-boolean JSON values"), Result.bSuccess);
		TestTrue(TEXT("player_mappable type failure identifies the parameter"),
				 Result.ErrorMessage.Contains(TEXT("player_mappable")) &&
					 Result.ErrorMessage.Contains(TEXT("boolean")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeBaseParams();
		Params->SetBoolField(TEXT("player_mappable"), true);
		Params->SetStringField(TEXT("mapping_name"), TEXT("ParamGuard.Jump"));
		Params->SetStringField(TEXT("display_name"), TEXT("Jump"));
		Params->SetStringField(TEXT("display_category"), TEXT("Movement"));
		TArray<TSharedPtr<FJsonValue>> ProfileIds;
		ProfileIds.Add(MakeShared<FJsonValueNumber>(123.0));
		Params->SetArrayField(TEXT("supported_key_profile_ids"), ProfileIds);

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("input"), TEXT("add_input_mapping"), Params);
		TestFalse(TEXT("profile IDs must reject non-string array entries"), Result.bSuccess);
		TestTrue(TEXT("profile ID type failure identifies the parameter"),
				 Result.ErrorMessage.Contains(TEXT("supported_key_profile_ids")) &&
					 Result.ErrorMessage.Contains(TEXT("string")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeBaseParams();
		Params->SetStringField(TEXT("mapping_name"), TEXT("ParamGuard.Jump"));

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("input"), TEXT("add_input_mapping"), Params);
		TestFalse(TEXT("metadata without player_mappable=true must fail"), Result.bSuccess);
		TestTrue(TEXT("metadata coupling failure explains the required boundary"),
				 Result.ErrorMessage.Contains(TEXT("player_mappable=true")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeBaseParams();
		Params->SetBoolField(TEXT("player_mappable"), true);
		Params->SetStringField(TEXT("mapping_name"), TEXT("None"));
		Params->SetStringField(TEXT("display_name"), TEXT("Jump"));
		Params->SetStringField(TEXT("display_category"), TEXT("Movement"));

		const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("input"), TEXT("add_input_mapping"), Params);
		TestFalse(TEXT("mapping_name=None must fail before asset loading"), Result.bSuccess);
		TestTrue(TEXT("None-name failure identifies mapping_name"),
				 Result.ErrorMessage.Contains(TEXT("mapping_name")) && Result.ErrorMessage.Contains(TEXT("non-None")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputMappingValidationSemanticsTest,
								 "Monolith.ParamGuard.GAS.InputMappingValidationSemantics",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInputMappingValidationSemanticsTest::RunTest(const FString& Parameters)
{
	EnsureMonolithGASInputActionsRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	FMonolithGASInputValidationFixture Fixture;
	if (!TestNotNull(TEXT("transient validation package exists"), Fixture.Package))
	{
		return false;
	}

	UInputAction* PrimaryAction = Fixture.CreateAction(TEXT("IA_Primary"));
	UInputAction* SecondaryAction = Fixture.CreateAction(TEXT("IA_Secondary"));
	if (!TestNotNull(TEXT("primary action exists"), PrimaryAction) ||
		!TestNotNull(TEXT("secondary action exists"), SecondaryAction))
	{
		return false;
	}

	UInputMappingContext* AdvisoryContext = Fixture.CreateContext(TEXT("IMC_Advisory"));
	if (!TestNotNull(TEXT("advisory context exists"), AdvisoryContext))
	{
		return false;
	}
	AdvisoryContext->MapKey(PrimaryAction, EKeys::LeftMouseButton);
	AdvisoryContext->MapKey(SecondaryAction, EKeys::LeftMouseButton);
	AdvisoryContext->MapKey(PrimaryAction, EKeys::Invalid);

	const FMonolithActionResult AdvisoryResult = Registry.ExecuteAction(
		TEXT("input"), TEXT("validate_input_mappings"), MakeMonolithGASInputValidationParams(AdvisoryContext));
	TestTrue(TEXT("legal shared/unbound validation executes"), AdvisoryResult.bSuccess);
	if (!TestTrue(TEXT("legal shared/unbound validation returns a payload"), AdvisoryResult.Result.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("shared keys and default-policy unbound rows remain valid"),
			 AdvisoryResult.Result->GetBoolField(TEXT("valid")));
	TestEqual(TEXT("shared key is not a conflict"),
			  static_cast<int32>(AdvisoryResult.Result->GetIntegerField(TEXT("conflicts"))), 0);
	TestEqual(TEXT("one legal shared-key group is reported"),
			  static_cast<int32>(AdvisoryResult.Result->GetIntegerField(TEXT("shared_key_groups"))), 1);
	TestEqual(TEXT("one unbound row is reported"),
			  static_cast<int32>(AdvisoryResult.Result->GetIntegerField(TEXT("unbound_mappings"))), 1);

	const TSharedPtr<FJsonObject> AdvisoryContextResult = GetOnlyMonolithGASInputValidationContext(AdvisoryResult);
	if (!TestTrue(TEXT("advisory context payload exists"), AdvisoryContextResult.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("advisory context reports one shared-key group"),
			  AdvisoryContextResult->GetArrayField(TEXT("shared_keys")).Num(), 1);
	TestEqual(TEXT("advisory context reports one unbound row"),
			  AdvisoryContextResult->GetArrayField(TEXT("unbound_rows")).Num(), 1);
	TestEqual(TEXT("advisory context has no hard issues"), AdvisoryContextResult->GetArrayField(TEXT("issues")).Num(),
			  0);

	const FMonolithActionResult StrictUnboundResult = Registry.ExecuteAction(
		TEXT("input"), TEXT("validate_input_mappings"), MakeMonolithGASInputValidationParams(AdvisoryContext, true));
	TestTrue(TEXT("strict unbound validation executes"), StrictUnboundResult.bSuccess);
	if (!TestTrue(TEXT("strict unbound validation returns a payload"), StrictUnboundResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("strict policy rejects an unbound row"), StrictUnboundResult.Result->GetBoolField(TEXT("valid")));
	TestTrue(TEXT("strict policy emits an unbound_mapping issue"),
			 MonolithGASInputValidationHasIssueType(GetOnlyMonolithGASInputValidationContext(StrictUnboundResult),
													TEXT("unbound_mapping")));

	UInputMappingContext* ConflictContext = Fixture.CreateContext(TEXT("IMC_Conflict"));
	if (!TestNotNull(TEXT("conflict context exists"), ConflictContext))
	{
		return false;
	}
	ConflictContext->MapKey(PrimaryAction, EKeys::SpaceBar);
	ConflictContext->MapKey(PrimaryAction, EKeys::SpaceBar);
	ConflictContext->MapKey(PrimaryAction, EKeys::Enter);
	FEnhancedActionKeyMapping& ModifiedEnterMapping = ConflictContext->MapKey(PrimaryAction, EKeys::Enter);
	ModifiedEnterMapping.Modifiers.Add(NewObject<UInputModifierNegate>(ConflictContext, NAME_None, RF_Transient));
	ConflictContext->MapKey(nullptr, EKeys::Tab);

	const FMonolithActionResult ConflictResult = Registry.ExecuteAction(
		TEXT("input"), TEXT("validate_input_mappings"), MakeMonolithGASInputValidationParams(ConflictContext));
	TestTrue(TEXT("conflict validation executes"), ConflictResult.bSuccess);
	if (!TestTrue(TEXT("conflict validation returns a payload"), ConflictResult.Result.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("exact duplicate and missing action invalidate the context"),
			  ConflictResult.Result->GetBoolField(TEXT("valid")));
	TestEqual(TEXT("one exact duplicate group is reported"),
			  static_cast<int32>(ConflictResult.Result->GetIntegerField(TEXT("conflicts"))), 1);
	TestEqual(TEXT("one missing action is reported"),
			  static_cast<int32>(ConflictResult.Result->GetIntegerField(TEXT("missing_actions"))), 1);
	TestEqual(TEXT("same action/key with different modifiers is not a duplicate"),
			  static_cast<int32>(ConflictResult.Result->GetIntegerField(TEXT("duplicate_mapping_conflicts"))), 1);

	const TSharedPtr<FJsonObject> ConflictContextResult = GetOnlyMonolithGASInputValidationContext(ConflictResult);
	TestTrue(TEXT("exact duplicate issue uses the precise contract name"),
			 MonolithGASInputValidationHasIssueType(ConflictContextResult, TEXT("duplicate_mapping_conflict")));
	TestTrue(TEXT("missing action remains a hard validation issue"),
			 MonolithGASInputValidationHasIssueType(ConflictContextResult, TEXT("missing_action")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInputMappingContextRegistrationModeTest,
								 "Monolith.ParamGuard.GAS.InputMappingContextRegistrationMode",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInputMappingContextRegistrationModeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	EnsureMonolithGASInputActionsRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	const FString AssetName = FString::Printf(
		TEXT("MonolithInputRegistration_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = FString::Printf(TEXT("/Engine/Transient/%s"), *AssetName);
	UPackage* Package = CreatePackage(*PackagePath);
	if (!TestNotNull(TEXT("transient registration-mode package exists"), Package))
	{
		return false;
	}

	UInputMappingContext* Context = NewObject<UInputMappingContext>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone | RF_Transient);
	if (!TestNotNull(TEXT("transient registration-mode context exists"), Context))
	{
		Package->MarkAsGarbage();
		return false;
	}

	auto MakeParams = [&PackagePath](const FString& TrackingMode)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), PackagePath);
		Params->SetStringField(TEXT("registration_tracking_mode"), TrackingMode);
		Params->SetBoolField(TEXT("overwrite"), true);
		Params->SetBoolField(TEXT("save"), false);
		return Params;
	};

	const FMonolithActionResult InvalidResult = Registry.ExecuteAction(
		TEXT("input"),
		TEXT("create_input_mapping_context"),
		MakeParams(TEXT("ReferenceCounted")));
	TestFalse(TEXT("unknown registration tracking mode is rejected"), InvalidResult.bSuccess);
	TestTrue(
		TEXT("invalid tracking-mode error names both supported values"),
		InvalidResult.ErrorMessage.Contains(TEXT("Untracked")) &&
			InvalidResult.ErrorMessage.Contains(TEXT("CountRegistrations")));
	TestEqual(
		TEXT("rejected value does not mutate the context"),
		Context->GetRegistrationTrackingMode(),
		EMappingContextRegistrationTrackingMode::Untracked);

	const FMonolithActionResult CountedResult = Registry.ExecuteAction(
		TEXT("input"),
		TEXT("create_input_mapping_context"),
		MakeParams(TEXT("CountRegistrations")));
	TestTrue(TEXT("CountRegistrations update succeeds"), CountedResult.bSuccess);
	TestEqual(
		TEXT("CountRegistrations is applied to the context"),
		Context->GetRegistrationTrackingMode(),
		EMappingContextRegistrationTrackingMode::CountRegistrations);
	if (CountedResult.Result.IsValid())
	{
		TestEqual(
			TEXT("result reports the applied registration tracking mode"),
			CountedResult.Result->GetStringField(TEXT("registration_tracking_mode")),
			FString(TEXT("CountRegistrations")));
	}

	Context->ClearFlags(RF_Public | RF_Standalone);
	Context->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
