#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/PackageName.h"
#include "MonolithGameSettingsActions.h"
#include "MonolithToolRegistry.h"
#include "PlayerMappableKeySettings.h"
#include "UObject/UnrealType.h"

namespace
{
	bool ConfigurePlayerMappableTestRow(
		FEnhancedActionKeyMapping& Mapping,
		UObject* Outer,
		const FName MappingName,
		const FText& DisplayName,
		const FText& DisplayCategory)
	{
		UPlayerMappableKeySettings* Settings =
			NewObject<UPlayerMappableKeySettings>(
				Outer,
				UPlayerMappableKeySettings::StaticClass(),
				NAME_None,
				RF_Transient);
		if (!Settings)
		{
			return false;
		}

		Settings->Name = MappingName;
		Settings->DisplayName = DisplayName;
		Settings->DisplayCategory = DisplayCategory;

		UScriptStruct* MappingStruct = FEnhancedActionKeyMapping::StaticStruct();
		FProperty* BehaviorProperty =
			MappingStruct
				? MappingStruct->FindPropertyByName(TEXT("SettingBehavior"))
				: nullptr;
		FObjectPropertyBase* SettingsProperty =
			MappingStruct
				? CastField<FObjectPropertyBase>(
					MappingStruct->FindPropertyByName(TEXT("PlayerMappableKeySettings")))
				: nullptr;
		if (!BehaviorProperty || !SettingsProperty)
		{
			return false;
		}

		const int64 OverrideValue =
			static_cast<int64>(EPlayerMappableKeySettingBehaviors::OverrideSettings);
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(BehaviorProperty))
		{
			void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(&Mapping);
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
				ValuePtr,
				OverrideValue);
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(BehaviorProperty))
		{
			ByteProperty->SetPropertyValue_InContainer(
				&Mapping,
				static_cast<uint8>(OverrideValue));
		}
		else
		{
			return false;
		}

		SettingsProperty->SetObjectPropertyValue_InContainer(&Mapping, Settings);
		return Mapping.IsPlayerMappable();
	}
}

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

	{
		UInputMappingContext* Context = NewObject<UInputMappingContext>(
			GetTransientPackage(),
			MakeUniqueObjectName(
				GetTransientPackage(),
				UInputMappingContext::StaticClass(),
				TEXT("MonolithAlternateSlotValidation")),
			RF_Transient);
		UInputAction* Action = NewObject<UInputAction>(
			Context,
			MakeUniqueObjectName(
				Context,
				UInputAction::StaticClass(),
				TEXT("IA_MonolithAlternateSlotValidation")),
			RF_Transient);
		TestNotNull(TEXT("alternate-slot transient context created"), Context);
		TestNotNull(TEXT("alternate-slot transient action created"), Action);

		if (Context && Action)
		{
			const FName MappingName(TEXT("MonolithAlternateSlot"));
			const FText DisplayName =
				FText::FromString(TEXT("Monolith Alternate Slot"));
			const FText DisplayCategory =
				FText::FromString(TEXT("Automation"));

			FEnhancedActionKeyMapping& Primary =
				Context->MapKey(Action, EKeys::W);
			TestTrue(
				TEXT("primary player-mappable row configured"),
				ConfigurePlayerMappableTestRow(
					Primary,
					Context,
					MappingName,
					DisplayName,
					DisplayCategory));

			FEnhancedActionKeyMapping& Secondary =
				Context->MapKey(Action, EKeys::Up);
			TestTrue(
				TEXT("secondary player-mappable row configured"),
				ConfigurePlayerMappableTestRow(
					Secondary,
					Context,
					MappingName,
					DisplayName,
					DisplayCategory));

			FEnhancedActionKeyMapping& Gamepad =
				Context->MapKey(Action, EKeys::Gamepad_FaceButton_Bottom);
			TestTrue(
				TEXT("gamepad player-mappable row configured"),
				ConfigurePlayerMappableTestRow(
					Gamepad,
					Context,
					MappingName,
					DisplayName,
					DisplayCategory));

			TSharedPtr<FJsonObject> AlternateSlotParams =
				MakeShared<FJsonObject>();
			AlternateSlotParams->SetStringField(
				TEXT("context_path"),
				Context->GetPathName());
			const FMonolithActionResult AlternateSlotResult =
				FMonolithGameSettingsActions::ValidatePlayerMappableInputSettings(
					AlternateSlotParams);
			TestTrue(
				TEXT("alternate-slot validation action succeeds"),
				AlternateSlotResult.bSuccess);
			TestTrue(
				TEXT("alternate-slot validation result is valid"),
				AlternateSlotResult.Result.IsValid());

			if (AlternateSlotResult.Result.IsValid())
			{
				TestTrue(
					TEXT("alternate-slot context satisfies validator"),
					AlternateSlotResult.Result->GetBoolField(TEXT("ok")));
				const TArray<TSharedPtr<FJsonValue>>& ContextResults =
					AlternateSlotResult.Result->GetArrayField(TEXT("contexts"));
				TestEqual(
					TEXT("one transient context result returned"),
					ContextResults.Num(),
					1);
				if (ContextResults.Num() == 1
					&& ContextResults[0].IsValid()
					&& ContextResults[0]->AsObject().IsValid())
				{
					const TSharedPtr<FJsonObject> ContextResult =
						ContextResults[0]->AsObject();
					TestEqual(
						TEXT("three mappable rows registered"),
						static_cast<int32>(
							ContextResult->GetNumberField(
								TEXT("mappable_mapping_count"))),
						3);
					TestEqual(
						TEXT("one keyboard alternate slot assigned"),
						static_cast<int32>(
							ContextResult->GetNumberField(
								TEXT("alternate_slot_mapping_count"))),
						1);
					TestEqual(
						TEXT("no slot identity conflict reported"),
						static_cast<int32>(
							ContextResult->GetNumberField(
								TEXT("mapping_slot_identity_conflict_count"))),
						0);
					TestEqual(
						TEXT("no slot capacity overflow reported"),
						static_cast<int32>(
							ContextResult->GetNumberField(
								TEXT("mapping_slot_overflow_count"))),
						0);
				}
			}
		}
	}

	{
		TSharedPtr<FJsonObject> InvalidBoolParams = MakeShared<FJsonObject>();
		InvalidBoolParams->SetStringField(
			TEXT("context_path"),
			TEXT("/Game/Input/Mappings/IMC_Default"));
		InvalidBoolParams->SetStringField(
			TEXT("require_valid_keys"),
			TEXT("true"));

		const FMonolithActionResult InvalidBoolResult =
			FMonolithGameSettingsActions::ValidatePlayerMappableInputSettings(
				InvalidBoolParams);
		TestFalse(
			TEXT("player-mappable validator rejects non-boolean option values"),
			InvalidBoolResult.bSuccess);
		TestTrue(
			TEXT("boolean type failure identifies require_valid_keys"),
			InvalidBoolResult.ErrorMessage.Contains(TEXT("require_valid_keys"))
				&& InvalidBoolResult.ErrorMessage.Contains(TEXT("boolean")));
	}

	{
		TSharedPtr<FJsonObject> InvalidClassBoolParams = MakeShared<FJsonObject>();
		InvalidClassBoolParams->SetStringField(
			TEXT("setting_class"),
			TEXT("/Script/GameSettings.GameSetting"));
		InvalidClassBoolParams->SetStringField(
			TEXT("require_concrete"),
			TEXT("false"));

		const FMonolithActionResult InvalidClassBoolResult =
			FMonolithGameSettingsActions::ValidateSettingClassContract(
				InvalidClassBoolParams);
		TestFalse(
			TEXT("setting-class validator rejects non-boolean option values"),
			InvalidClassBoolResult.bSuccess);
		TestTrue(
			TEXT("setting-class boolean failure identifies require_concrete"),
			InvalidClassBoolResult.ErrorMessage.Contains(TEXT("require_concrete"))
				&& InvalidClassBoolResult.ErrorMessage.Contains(TEXT("boolean")));
	}

	{
		TSharedPtr<FJsonObject> InvalidOptionalStringParams =
			MakeShared<FJsonObject>();
		InvalidOptionalStringParams->SetNumberField(
			TEXT("screen_class"),
			42.0);

		const FMonolithActionResult InvalidOptionalStringResult =
			FMonolithGameSettingsActions::DescribeRegistryTree(
				InvalidOptionalStringParams);
		TestFalse(
			TEXT("registry description rejects non-string class paths"),
			InvalidOptionalStringResult.bSuccess);
		TestTrue(
			TEXT("class-path type failure identifies screen_class"),
			InvalidOptionalStringResult.ErrorMessage.Contains(TEXT("screen_class"))
				&& InvalidOptionalStringResult.ErrorMessage.Contains(TEXT("string")));
	}

	{
		TSharedPtr<FJsonObject> InvalidArrayParams = MakeShared<FJsonObject>();
		InvalidArrayParams->SetStringField(
			TEXT("dynamic_paths"),
			TEXT("GetLocalSettings"));

		const FMonolithActionResult InvalidArrayResult =
			FMonolithGameSettingsActions::ValidateDataSourceBindings(
				InvalidArrayParams);
		TestFalse(
			TEXT("data-source validator rejects non-array dynamic_paths"),
			InvalidArrayResult.bSuccess);
		TestTrue(
			TEXT("dynamic-path type failure identifies dynamic_paths"),
			InvalidArrayResult.ErrorMessage.Contains(TEXT("dynamic_paths"))
				&& InvalidArrayResult.ErrorMessage.Contains(TEXT("array")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
