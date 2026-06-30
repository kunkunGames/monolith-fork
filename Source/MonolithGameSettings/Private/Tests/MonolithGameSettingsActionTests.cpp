#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "MonolithGameSettingsActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithGameSettingsRegistryAndValidationTest,
	"Monolith.GameSettings.RegistryAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGameSettingsRegistryAndValidationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	bool bHasStatus = false;
	bool bHasDescribeTree = false;
	bool bHasValidateSetting = false;
	bool bHasValidateDataSource = false;
	bool bHasValidateVisualData = false;
	bool bHasValidatePlayerMappable = false;
	for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("settings")))
	{
		if (ActionInfo.Action == TEXT("get_status"))
		{
			bHasStatus = true;
		}
		else if (ActionInfo.Action == TEXT("describe_registry_tree"))
		{
			bHasDescribeTree = true;
		}
		else if (ActionInfo.Action == TEXT("validate_setting_class_contract"))
		{
			bHasValidateSetting = true;
		}
		else if (ActionInfo.Action == TEXT("validate_data_source_bindings"))
		{
			bHasValidateDataSource = true;
		}
		else if (ActionInfo.Action == TEXT("validate_visual_data"))
		{
			bHasValidateVisualData = true;
		}
		else if (ActionInfo.Action == TEXT("validate_player_mappable_input_settings"))
		{
			bHasValidatePlayerMappable = true;
		}
	}

	TestTrue(TEXT("settings.get_status registered"), bHasStatus);
	TestTrue(TEXT("settings.describe_registry_tree registered"), bHasDescribeTree);
	TestTrue(TEXT("settings.validate_setting_class_contract registered"), bHasValidateSetting);
	TestTrue(TEXT("settings.validate_data_source_bindings registered"), bHasValidateDataSource);
	TestTrue(TEXT("settings.validate_visual_data registered"), bHasValidateVisualData);
	TestTrue(TEXT("settings.validate_player_mappable_input_settings registered"), bHasValidatePlayerMappable);
	for (const TCHAR* Action : {
		TEXT("get_status"),
		TEXT("describe_registry_tree"),
		TEXT("validate_setting_class_contract"),
		TEXT("validate_data_source_bindings"),
		TEXT("validate_visual_data"),
		TEXT("validate_player_mappable_input_settings")
	})
	{
		TestEqual(
			FString::Printf(TEXT("settings.%s is read-only"), Action),
			Registry.GetActionExecutionPolicy(TEXT("settings"), Action).PolicyId,
			FString(TEXT("read_only")));
	}

	const FMonolithActionResult StatusResult = FMonolithGameSettingsActions::GetStatus(MakeShared<FJsonObject>());
	TestTrue(TEXT("get_status succeeds"), StatusResult.bSuccess);
	TestTrue(TEXT("get_status result object is valid"), StatusResult.Result.IsValid());

	TSharedPtr<FJsonObject> TreeParams = MakeShared<FJsonObject>();
	TreeParams->SetStringField(TEXT("screen_class"), TEXT("/Script/GameSettings.GameSettingScreen"));
	TreeParams->SetStringField(TEXT("registry_class"), TEXT("/Script/GameSettings.GameSettingRegistry"));
	const FMonolithActionResult TreeResult = FMonolithGameSettingsActions::DescribeRegistryTree(TreeParams);
	TestTrue(TEXT("describe_registry_tree succeeds"), TreeResult.bSuccess);
	TestTrue(TEXT("describe_registry_tree result object is valid"), TreeResult.Result.IsValid());

	TSharedPtr<FJsonObject> SettingParams = MakeShared<FJsonObject>();
	SettingParams->SetStringField(TEXT("setting_class"), TEXT("/Script/GameSettings.GameSettingCollection"));
	SettingParams->SetBoolField(TEXT("require_collection"), true);
	SettingParams->SetBoolField(TEXT("require_concrete"), true);
	const FMonolithActionResult SettingResult = FMonolithGameSettingsActions::ValidateSettingClassContract(SettingParams);
	TestTrue(TEXT("validate_setting_class_contract succeeds for collection"), SettingResult.bSuccess);
	TestTrue(TEXT("collection setting result object is valid"), SettingResult.Result.IsValid());
	if (SettingResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("collection setting ok field exists"), SettingResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("GameSettingCollection satisfies collection contract"), bOk);
	}

	TSharedPtr<FJsonObject> DataSourceParams = MakeShared<FJsonObject>();
	DataSourceParams->SetStringField(TEXT("getter_path"), TEXT("LocalSettings.Audio.Volume"));
	DataSourceParams->SetStringField(TEXT("setter_path"), TEXT("LocalSettings.Audio.Volume"));
	const FMonolithActionResult DataSourceResult = FMonolithGameSettingsActions::ValidateDataSourceBindings(DataSourceParams);
	TestTrue(TEXT("validate_data_source_bindings succeeds for dotted paths"), DataSourceResult.bSuccess);
	TestTrue(TEXT("data source result object is valid"), DataSourceResult.Result.IsValid());
	if (DataSourceResult.Result.IsValid())
	{
		bool bOk = false;
		TestTrue(TEXT("data source ok field exists"), DataSourceResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestTrue(TEXT("valid dotted data source paths are accepted"), bOk);
	}

	TSharedPtr<FJsonObject> BadSettingParams = MakeShared<FJsonObject>();
	BadSettingParams->SetStringField(TEXT("setting_class"), TEXT("/Script/Engine.Actor"));
	const FMonolithActionResult BadSettingResult = FMonolithGameSettingsActions::ValidateSettingClassContract(BadSettingParams);
	TestTrue(TEXT("validate_setting_class_contract returns structured result for Actor"), BadSettingResult.bSuccess);
	TestTrue(TEXT("bad setting result object is valid"), BadSettingResult.Result.IsValid());
	if (BadSettingResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("bad setting ok field exists"), BadSettingResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("Actor is not a GameSetting"), bOk);
	}

	TSharedPtr<FJsonObject> BadVisualParams = MakeShared<FJsonObject>();
	BadVisualParams->SetStringField(TEXT("asset_path"), TEXT("/Script/Engine.Actor"));
	const FMonolithActionResult BadVisualResult = FMonolithGameSettingsActions::ValidateVisualData(BadVisualParams);
	TestTrue(TEXT("validate_visual_data returns structured result for wrong type"), BadVisualResult.bSuccess);
	TestTrue(TEXT("bad visual data result object is valid"), BadVisualResult.Result.IsValid());
	if (BadVisualResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("bad visual data ok field exists"), BadVisualResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("Actor class is not GameSettingVisualData"), bOk);
	}

	TSharedPtr<FJsonObject> BadMappableParams = MakeShared<FJsonObject>();
	BadMappableParams->SetStringField(TEXT("config_path"), TEXT("/Script/Engine.Actor"));
	const FMonolithActionResult BadMappableResult = FMonolithGameSettingsActions::ValidatePlayerMappableInputSettings(BadMappableParams);
	TestTrue(TEXT("validate_player_mappable_input_settings returns structured result for wrong type"), BadMappableResult.bSuccess);
	TestTrue(TEXT("bad mappable result object is valid"), BadMappableResult.Result.IsValid());
	if (BadMappableResult.Result.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("bad mappable ok field exists"), BadMappableResult.Result->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("Actor class is not PlayerMappableInputConfig"), bOk);
	}

	if (FPackageName::DoesPackageExist(TEXT("/Game/Input/Configs/PMI_Default_KBM")))
	{
		TSharedPtr<FJsonObject> MappableParams = MakeShared<FJsonObject>();
		MappableParams->SetStringField(TEXT("config_path"), TEXT("/Game/Input/Configs/PMI_Default_KBM"));
		MappableParams->SetBoolField(TEXT("require_config_display_name"), false);
		const FMonolithActionResult MappableResult = FMonolithGameSettingsActions::ValidatePlayerMappableInputSettings(MappableParams);
		TestTrue(TEXT("validate_player_mappable_input_settings succeeds for Speed default KBM config"), MappableResult.bSuccess);
		TestTrue(TEXT("mappable result object is valid"), MappableResult.Result.IsValid());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
