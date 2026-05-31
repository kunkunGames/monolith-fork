#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MonolithGASAbilityActions.h"
#include "MonolithGASDataAssetProfileActions.h"
#include "MonolithGASInspectActions.h"
#include "MonolithToolRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
static void EnsureDataAssetProfileActionsRegistered()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("gas"), TEXT("describe_data_asset_gas_profile"))
		|| !Registry.HasAction(TEXT("gas"), TEXT("validate_data_asset_gas_profile"))
		|| !Registry.HasAction(TEXT("gas"), TEXT("set_data_asset_gas_fields")))
	{
		FMonolithGASDataAssetProfileActions::RegisterActions(Registry);
	}
	if (!Registry.HasAction(TEXT("gas"), TEXT("export_gas_manifest")))
	{
		FMonolithGASInspectActions::RegisterActions(Registry);
	}
	if (!Registry.HasAction(TEXT("gas"), TEXT("validate_ability_blueprint")))
	{
		FMonolithGASAbilityActions::RegisterActions(Registry);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASDataAssetProfileMissingParamTest,
	"Monolith.GAS.DataAssetProfile.MissingParamErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASDataAssetProfileMissingParamTest::RunTest(const FString& Parameters)
{
	EnsureDataAssetProfileActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("gas"), TEXT("describe_data_asset_gas_profile"), Params);
	TestFalse(TEXT("Missing asset_path should fail cleanly"), Result.bSuccess);
	TestFalse(TEXT("Missing asset_path should report an error"), Result.ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASDataAssetProfileSetMissingFieldsTest,
	"Monolith.GAS.DataAssetProfile.SetMissingFieldsParamErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASDataAssetProfileSetMissingFieldsTest::RunTest(const FString& Parameters)
{
	EnsureDataAssetProfileActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MonolithMissing/DA_MissingSkill"));

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("gas"), TEXT("set_data_asset_gas_fields"), Params);
	TestFalse(TEXT("Missing fields should fail cleanly before asset load"), Result.bSuccess);
	TestFalse(TEXT("Missing fields should report an error"), Result.ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASDataAssetProfileValidateEmptyPathShapeTest,
	"Monolith.GAS.DataAssetProfile.ValidateEmptyPathShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASDataAssetProfileValidateEmptyPathShapeTest::RunTest(const FString& Parameters)
{
	EnsureDataAssetProfileActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path_filter"), TEXT("/Game/__MonolithMissing"));
	Params->SetBoolField(TEXT("include_source_scan"), false);
	Params->SetNumberField(TEXT("max_assets"), 1);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("gas"), TEXT("validate_data_asset_gas_profile"), Params);
	TestTrue(TEXT("Empty validation scan should succeed with a structured report"), Result.bSuccess);
	TestTrue(TEXT("Validation scan should return a JSON result"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	bool bReadOnly = false;
	TestTrue(TEXT("Validation report should expose read_only"), Result.Result->TryGetBoolField(TEXT("read_only"), bReadOnly));
	TestTrue(TEXT("Validation report should be read-only"), bReadOnly);

	double Count = -1.0;
	TestTrue(TEXT("Validation report should expose count"), Result.Result->TryGetNumberField(TEXT("count"), Count));
	TestEqual(TEXT("Missing test path should have zero profiles"), Count, 0.0);

	const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
	TestTrue(TEXT("Validation report should expose profiles array"), Result.Result->TryGetArrayField(TEXT("profiles"), Profiles));
	TestTrue(TEXT("Profiles array should be empty"), Profiles != nullptr && Profiles->Num() == 0);

	const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
	TestTrue(TEXT("Validation report should expose issues array"), Result.Result->TryGetArrayField(TEXT("issues"), Issues));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASDataAssetProfileManifestIncludesProfileReportTest,
	"Monolith.GAS.DataAssetProfile.ManifestIncludesProfileReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASDataAssetProfileManifestIncludesProfileReportTest::RunTest(const FString& Parameters)
{
	EnsureDataAssetProfileActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("path_filter"), TEXT("/Game/__MonolithMissing"));
	Params->SetStringField(TEXT("data_asset_path_filter"), TEXT("/Game/__MonolithMissing"));
	Params->SetBoolField(TEXT("include_relationships"), false);
	Params->SetBoolField(TEXT("include_data_asset_profiles"), true);
	Params->SetNumberField(TEXT("max_data_asset_profiles"), 1);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("gas"), TEXT("export_gas_manifest"), Params);
	TestTrue(TEXT("Manifest export should succeed for an empty filtered path"), Result.bSuccess);
	TestTrue(TEXT("Manifest export should return a JSON result"), Result.Result.IsValid());
	if (!Result.Result.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Manifest = nullptr;
	TestTrue(TEXT("Manifest export should include inline manifest"),
		Result.Result->TryGetObjectField(TEXT("manifest"), Manifest));
	if (!Manifest || !Manifest->IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* DataAssetProfiles = nullptr;
	TestTrue(TEXT("Manifest should include data_asset_profiles report"),
		(*Manifest)->TryGetObjectField(TEXT("data_asset_profiles"), DataAssetProfiles));
	if (!DataAssetProfiles || !DataAssetProfiles->IsValid())
	{
		return false;
	}

	double Count = -1.0;
	TestTrue(TEXT("DataAsset profile report should expose count"),
		(*DataAssetProfiles)->TryGetNumberField(TEXT("count"), Count));
	TestEqual(TEXT("Missing test path should have zero profile rows"), Count, 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGASAbilityBlueprintValidationMissingParamTest,
	"Monolith.GAS.AbilityBlueprintValidation.MissingParamErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGASAbilityBlueprintValidationMissingParamTest::RunTest(const FString& Parameters)
{
	EnsureDataAssetProfileActionsRegistered();

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
		TEXT("gas"), TEXT("validate_ability_blueprint"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Missing asset_path should fail cleanly"), Result.bSuccess);
	TestFalse(TEXT("Missing asset_path should report an error"), Result.ErrorMessage.IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
