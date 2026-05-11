#include "MonolithGASInputAssetActions.h"

#include "MonolithGASInternal.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	FString ValueTypeToString(EInputActionValueType ValueType)
	{
		switch (ValueType)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D: return TEXT("Axis1D");
		case EInputActionValueType::Axis2D: return TEXT("Axis2D");
		case EInputActionValueType::Axis3D: return TEXT("Axis3D");
		default: return TEXT("Unknown");
		}
	}

	bool ParseValueType(const FString& Input, EInputActionValueType& OutValueType)
	{
		if (Input.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Boolean;
			return true;
		}
		if (Input.Equals(TEXT("Axis1D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis1d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis1D;
			return true;
		}
		if (Input.Equals(TEXT("Axis2D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis2d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis2D;
			return true;
		}
		if (Input.Equals(TEXT("Axis3D"), ESearchCase::IgnoreCase) || Input.Equals(TEXT("axis3d"), ESearchCase::IgnoreCase))
		{
			OutValueType = EInputActionValueType::Axis3D;
			return true;
		}
		return false;
	}

	FString AccumulationToString(EInputActionAccumulationBehavior Behavior)
	{
		switch (Behavior)
		{
		case EInputActionAccumulationBehavior::Cumulative: return TEXT("Cumulative");
		case EInputActionAccumulationBehavior::TakeHighestAbsoluteValue:
		default:
			return TEXT("TakeHighestAbsoluteValue");
		}
	}

	bool ParseAccumulation(const FString& Input, EInputActionAccumulationBehavior& OutBehavior)
	{
		if (Input.Equals(TEXT("cumulative"), ESearchCase::IgnoreCase))
		{
			OutBehavior = EInputActionAccumulationBehavior::Cumulative;
			return true;
		}
		if (Input.Equals(TEXT("take_highest_absolute_value"), ESearchCase::IgnoreCase) ||
			Input.Equals(TEXT("TakeHighestAbsoluteValue"), ESearchCase::IgnoreCase) ||
			Input.Equals(TEXT("highest"), ESearchCase::IgnoreCase))
		{
			OutBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
			return true;
		}
		return false;
	}

	FString TrackingModeToString(EMappingContextRegistrationTrackingMode Mode)
	{
		switch (Mode)
		{
		case EMappingContextRegistrationTrackingMode::CountRegistrations: return TEXT("CountRegistrations");
		case EMappingContextRegistrationTrackingMode::Untracked:
		default:
			return TEXT("Untracked");
		}
	}

	FString NormalizeObjectPath(const FString& Path)
	{
		if (Path.Contains(TEXT(".")))
		{
			return Path;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		return Path + TEXT(".") + AssetName;
	}

	FString NormalizePackagePath(const FString& Path)
	{
		FString PackagePath = Path;
		FString ObjectName;
		if (PackagePath.Split(TEXT("."), &PackagePath, &ObjectName))
		{
			// PackagePath now contains the long package part of an object path.
		}
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			PackagePath = TEXT("/Game/") + PackagePath;
		}
		return PackagePath;
	}

	bool SaveAssetIfRequested(UObject* Asset, bool bSave, bool& bSaved, FString& OutError)
	{
		bSaved = false;
		if (!Asset)
		{
			OutError = TEXT("Asset is null");
			return false;
		}

		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			OutError = TEXT("Asset has no outer package");
			return false;
		}

		Package->MarkPackageDirty();
		if (!bSave)
		{
			return true;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bSaved = UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
		if (!bSaved)
		{
			OutError = FString::Printf(TEXT("SavePackage failed for '%s'"), *PackageFilename);
			return false;
		}
		return true;
	}

	UInputAction* LoadInputAction(const FString& Path, FString& OutError)
	{
		UObject* Obj = MonolithGAS::LoadAssetFromPath(Path, OutError);
		UInputAction* Action = Cast<UInputAction>(Obj);
		if (!Action && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputAction asset not found: %s"), *Path);
		}
		return Action;
	}

	UInputMappingContext* LoadInputMappingContext(const FString& Path, FString& OutError)
	{
		UObject* Obj = MonolithGAS::LoadAssetFromPath(Path, OutError);
		UInputMappingContext* Context = Cast<UInputMappingContext>(Obj);
		if (!Context && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputMappingContext asset not found: %s"), *Path);
		}
		return Context;
	}

	bool ParseKey(const FString& KeyName, FKey& OutKey, FString& OutError)
	{
		OutKey = FKey(*KeyName);
		if (!OutKey.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid input key: %s"), *KeyName);
			return false;
		}

		TArray<FKey> KnownKeys;
		EKeys::GetAllKeys(KnownKeys);
		if (!KnownKeys.Contains(OutKey))
		{
			OutError = FString::Printf(TEXT("Unknown input key: %s"), *KeyName);
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> TriggerOrModifierClassJson(const UObject* Obj)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("class"), Obj ? Obj->GetClass()->GetName() : TEXT("None"));
		Json->SetStringField(TEXT("path"), Obj ? Obj->GetClass()->GetPathName() : TEXT(""));
		return Json;
	}

	TArray<TSharedPtr<FJsonValue>> ObjectClassArray(const TArray<TObjectPtr<UInputTrigger>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TObjectPtr<UInputTrigger>& Obj : Objects)
		{
			Result.Add(MakeShared<FJsonValueObject>(TriggerOrModifierClassJson(Obj.Get())));
		}
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> ObjectClassArray(const TArray<TObjectPtr<UInputModifier>>& Objects)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TObjectPtr<UInputModifier>& Obj : Objects)
		{
			Result.Add(MakeShared<FJsonValueObject>(TriggerOrModifierClassJson(Obj.Get())));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> InputActionToJson(const UInputAction* Action)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Action)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Action->GetPathName());
		Json->SetStringField(TEXT("package_path"), Action->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Action->GetName());
		Json->SetStringField(TEXT("value_type"), ValueTypeToString(Action->ValueType));
		Json->SetStringField(TEXT("description"), Action->ActionDescription.ToString());
		Json->SetBoolField(TEXT("consume_input"), Action->bConsumeInput);
		Json->SetBoolField(TEXT("consume_legacy_mappings"), Action->bConsumesActionAndAxisMappings);
		Json->SetBoolField(TEXT("trigger_when_paused"), Action->bTriggerWhenPaused);
		Json->SetBoolField(TEXT("reserve_all_mappings"), Action->bReserveAllMappings);
		Json->SetStringField(TEXT("accumulation"), AccumulationToString(Action->AccumulationBehavior));
		Json->SetArrayField(TEXT("triggers"), ObjectClassArray(Action->Triggers));
		Json->SetArrayField(TEXT("modifiers"), ObjectClassArray(Action->Modifiers));
		Json->SetBoolField(TEXT("has_player_mappable_settings"), Action->GetPlayerMappableKeySettings() != nullptr);
		return Json;
	}

	TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("index"), Index);
		Json->SetStringField(TEXT("action"), Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
		Json->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
		Json->SetStringField(TEXT("key"), Mapping.Key.ToString());
		Json->SetStringField(TEXT("key_name"), Mapping.Key.GetFName().ToString());
		Json->SetBoolField(TEXT("is_player_mappable"), Mapping.IsPlayerMappable());
		Json->SetStringField(TEXT("mapping_name"), Mapping.GetMappingName().ToString());
		Json->SetStringField(TEXT("display_name"), Mapping.GetDisplayName().ToString());
		Json->SetStringField(TEXT("display_category"), Mapping.GetDisplayCategory().ToString());
		Json->SetArrayField(TEXT("triggers"), ObjectClassArray(Mapping.Triggers));
		Json->SetArrayField(TEXT("modifiers"), ObjectClassArray(Mapping.Modifiers));
		return Json;
	}

	TSharedPtr<FJsonObject> MappingContextToJson(const UInputMappingContext* Context)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		if (!Context)
		{
			return Json;
		}

		Json->SetStringField(TEXT("asset_path"), Context->GetPathName());
		Json->SetStringField(TEXT("package_path"), Context->GetOutermost()->GetName());
		Json->SetStringField(TEXT("name"), Context->GetName());
		Json->SetStringField(TEXT("description"), Context->ContextDescription.ToString());
		Json->SetBoolField(TEXT("filters_by_input_mode"), Context->ShouldFilterMappingByInputMode());
		Json->SetStringField(TEXT("registration_tracking_mode"), TrackingModeToString(Context->GetRegistrationTrackingMode()));

		TArray<TSharedPtr<FJsonValue>> MappingsJson;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			MappingsJson.Add(MakeShared<FJsonValueObject>(MappingToJson(Mappings[Index], Index)));
		}
		Json->SetArrayField(TEXT("mappings"), MappingsJson);
		Json->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		return Json;
	}

	void GetAssetsByClass(UClass* Class, const TSharedPtr<FJsonObject>& Params, TArray<FAssetData>& OutAssets)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = true;

		FString Path;
		if (Params->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*Path));
			Filter.bRecursivePaths = true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
	}

	TArray<FString> ReadContextPaths(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Paths;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params->TryGetArrayField(TEXT("context_paths"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Path;
				if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
				{
					Paths.Add(Path);
				}
			}
		}
		return Paths;
	}
}

void FMonolithGASInputAssetActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("input"), TEXT("list_input_actions"),
		TEXT("List Enhanced Input UInputAction assets"),
		FMonolithActionHandler::CreateStatic(&HandleListInputActions),
		FParamSchemaBuilder()
			.Optional(TEXT("path"), TEXT("string"), TEXT("Optional package path root, e.g. /Game/Input"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load assets and include value type/triggers/modifiers"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_action"),
		TEXT("Inspect an Enhanced Input UInputAction asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputAction),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("create_input_action"),
		TEXT("Create or update a UInputAction asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputAction),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path, e.g. /Game/Input/IA_Jump"))
			.Optional(TEXT("value_type"), TEXT("string"), TEXT("Boolean, Axis1D, Axis2D, or Axis3D"), TEXT("Boolean"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("consume_input"), TEXT("boolean"), TEXT("Consume lower priority enhanced input mappings"), TEXT("true"))
			.Optional(TEXT("trigger_when_paused"), TEXT("boolean"), TEXT("Allow action while paused"), TEXT("false"))
			.Optional(TEXT("accumulation"), TEXT("string"), TEXT("TakeHighestAbsoluteValue or Cumulative"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing InputAction"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_input_action_properties"),
		TEXT("Update common UInputAction properties"),
		FMonolithActionHandler::CreateStatic(&HandleSetInputActionProperties),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Optional(TEXT("value_type"), TEXT("string"), TEXT("Boolean, Axis1D, Axis2D, or Axis3D"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("consume_input"), TEXT("boolean"), TEXT("Consume lower priority enhanced input mappings"))
			.Optional(TEXT("consume_legacy_mappings"), TEXT("boolean"), TEXT("Consume legacy action/axis mappings"))
			.Optional(TEXT("trigger_when_paused"), TEXT("boolean"), TEXT("Allow action while paused"))
			.Optional(TEXT("reserve_all_mappings"), TEXT("boolean"), TEXT("Reserve all mappings"))
			.Optional(TEXT("accumulation"), TEXT("string"), TEXT("TakeHighestAbsoluteValue or Cumulative"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("list_input_mapping_contexts"),
		TEXT("List Enhanced Input UInputMappingContext assets"),
		FMonolithActionHandler::CreateStatic(&HandleListInputMappingContexts),
		FParamSchemaBuilder()
			.Optional(TEXT("path"), TEXT("string"), TEXT("Optional package path root, e.g. /Game/Input"))
			.Optional(TEXT("include_details"), TEXT("boolean"), TEXT("Load assets and include mappings"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("get_input_mapping_context"),
		TEXT("Inspect an Enhanced Input UInputMappingContext asset"),
		FMonolithActionHandler::CreateStatic(&HandleGetInputMappingContext),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("InputMappingContext asset path"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("create_input_mapping_context"),
		TEXT("Create or update a UInputMappingContext asset"),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputMappingContext),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path, e.g. /Game/Input/IMC_Default"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing context"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("add_input_mapping"),
		TEXT("Add a key mapping to an Input Mapping Context"),
		FMonolithActionHandler::CreateStatic(&HandleAddInputMapping),
		FParamSchemaBuilder()
			.Required(TEXT("context_path"), TEXT("string"), TEXT("InputMappingContext asset path"))
			.Required(TEXT("action_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name, e.g. SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("remove_input_mapping"),
		TEXT("Remove a key mapping from an Input Mapping Context"),
		FMonolithActionHandler::CreateStatic(&HandleRemoveInputMapping),
		FParamSchemaBuilder()
			.Required(TEXT("context_path"), TEXT("string"), TEXT("InputMappingContext asset path"))
			.Required(TEXT("action_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name to remove"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("validate_input_mappings"),
		TEXT("Validate Enhanced Input Mapping Contexts for missing actions and duplicate key conflicts"),
		FMonolithActionHandler::CreateStatic(&HandleValidateInputMappings),
		FParamSchemaBuilder()
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Specific InputMappingContext paths; omitted means all contexts"))
			.Optional(TEXT("path"), TEXT("string"), TEXT("Optional package path root when context_paths is omitted"))
			.Build());
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputActions(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	GetAssetsByClass(UInputAction::StaticClass(), Params, Assets);
	const bool bIncludeDetails = Params->HasField(TEXT("include_details")) && Params->GetBoolField(TEXT("include_details"));

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputAction* Action = Cast<UInputAction>(AssetData.GetAsset()))
			{
				Row = InputActionToJson(Action);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("actions"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputAction(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString Error;
	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(InputActionToJson(Action));
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleCreateInputAction(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	bool bOverwrite = false;
	if (Params->HasField(TEXT("overwrite")) && !Params->TryGetBoolField(TEXT("overwrite"), bOverwrite))
	{
		return FMonolithActionResult::Error(TEXT("overwrite must be a boolean"));
	}
	bool bSave = true;
	if (Params->HasField(TEXT("save")) && !Params->TryGetBoolField(TEXT("save"), bSave))
	{
		return FMonolithActionResult::Error(TEXT("save must be a boolean"));
	}

	const FString PackagePath = NormalizePackagePath(AssetPath);
	const FString ObjectPath = NormalizeObjectPath(PackagePath);
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ObjectPath);
	if (Action && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputAction already exists: %s"), *AssetPath));
	}

	bool bCreated = false;
	if (!Action)
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(PackagePath, AssetName, ExistError))
		{
			return FMonolithActionResult::Error(ExistError);
		}

		UPackage* Package = MonolithGAS::GetOrCreatePackage(PackagePath, Error);
		if (!Package)
		{
			return FMonolithActionResult::Error(Error);
		}

		Action = NewObject<UInputAction>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Action)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create InputAction"));
		}
		FAssetRegistryModule::AssetCreated(Action);
		bCreated = true;
	}

	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputAction", "Create Input Action"));
		Action->Modify();

		if (Params->HasField(TEXT("value_type")) || bCreated)
		{
			FString ValueTypeString = TEXT("Boolean");
			if (Params->HasField(TEXT("value_type")) && !Params->TryGetStringField(TEXT("value_type"), ValueTypeString))
			{
				return FMonolithActionResult::Error(TEXT("value_type must be a string"));
			}
			EInputActionValueType ValueType;
			if (!ParseValueType(ValueTypeString, ValueType))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
			}
			Action->ValueType = ValueType;
		}

		if (Params->HasField(TEXT("description")))
		{
			FString Description;
			if (!Params->TryGetStringField(TEXT("description"), Description))
			{
				return FMonolithActionResult::Error(TEXT("description must be a string"));
			}
			Action->ActionDescription = FText::FromString(Description);
		}
		if (Params->HasField(TEXT("consume_input")))
		{
			bool bConsumeInput = false;
			if (!Params->TryGetBoolField(TEXT("consume_input"), bConsumeInput))
			{
				return FMonolithActionResult::Error(TEXT("consume_input must be a boolean"));
			}
			Action->bConsumeInput = bConsumeInput;
		}
		if (Params->HasField(TEXT("trigger_when_paused")))
		{
			bool bTriggerWhenPaused = false;
			if (!Params->TryGetBoolField(TEXT("trigger_when_paused"), bTriggerWhenPaused))
			{
				return FMonolithActionResult::Error(TEXT("trigger_when_paused must be a boolean"));
			}
			Action->bTriggerWhenPaused = bTriggerWhenPaused;
		}
		if (Params->HasField(TEXT("accumulation")))
		{
			EInputActionAccumulationBehavior Behavior;
			FString Accumulation;
			if (!Params->TryGetStringField(TEXT("accumulation"), Accumulation))
			{
				return FMonolithActionResult::Error(TEXT("accumulation must be a string"));
			}
			if (!ParseAccumulation(Accumulation, Behavior))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation));
			}
			Action->AccumulationBehavior = Behavior;
		}
	}

	bool bSaved = false;
	if (!SaveAssetIfRequested(Action, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = InputActionToJson(Action);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleSetInputActionProperties(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));
	FString Error;
	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "SetInputActionProperties", "Set Input Action Properties"));
	Action->Modify();

	if (Params->HasField(TEXT("value_type")))
	{
		EInputActionValueType ValueType;
		const FString ValueTypeString = Params->GetStringField(TEXT("value_type"));
		if (!ParseValueType(ValueTypeString, ValueType))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
		}
		Action->ValueType = ValueType;
	}
	if (Params->HasField(TEXT("description")))
	{
		Action->ActionDescription = FText::FromString(Params->GetStringField(TEXT("description")));
	}
	if (Params->HasField(TEXT("consume_input")))
	{
		Action->bConsumeInput = Params->GetBoolField(TEXT("consume_input"));
	}
	if (Params->HasField(TEXT("consume_legacy_mappings")))
	{
		Action->bConsumesActionAndAxisMappings = Params->GetBoolField(TEXT("consume_legacy_mappings"));
	}
	if (Params->HasField(TEXT("trigger_when_paused")))
	{
		Action->bTriggerWhenPaused = Params->GetBoolField(TEXT("trigger_when_paused"));
	}
	if (Params->HasField(TEXT("reserve_all_mappings")))
	{
		Action->bReserveAllMappings = Params->GetBoolField(TEXT("reserve_all_mappings"));
	}
	if (Params->HasField(TEXT("accumulation")))
	{
		EInputActionAccumulationBehavior Behavior;
		const FString Accumulation = Params->GetStringField(TEXT("accumulation"));
		if (!ParseAccumulation(Accumulation, Behavior))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation));
		}
		Action->AccumulationBehavior = Behavior;
	}

	bool bSaved = false;
	if (!SaveAssetIfRequested(Action, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = InputActionToJson(Action);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets);
	const bool bIncludeDetails = Params->HasField(TEXT("include_details")) && Params->GetBoolField(TEXT("include_details"));

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FAssetData& AssetData : Assets)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("name"), AssetData.AssetName.ToString());
		if (bIncludeDetails)
		{
			if (UInputMappingContext* Context = Cast<UInputMappingContext>(AssetData.GetAsset()))
			{
				Row = MappingContextToJson(Context);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("contexts"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleGetInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
	const FString AssetPath = Params->GetStringField(TEXT("asset_path"));
	FString Error;
	UInputMappingContext* Context = LoadInputMappingContext(AssetPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}
	return FMonolithActionResult::Success(MappingContextToJson(Context));
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleCreateInputMappingContext(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required"));
	}

	FString Error;
	bool bOverwrite = false;
	if (Params->HasField(TEXT("overwrite")) && !Params->TryGetBoolField(TEXT("overwrite"), bOverwrite))
	{
		return FMonolithActionResult::Error(TEXT("overwrite must be a boolean"));
	}
	bool bSave = true;
	if (Params->HasField(TEXT("save")) && !Params->TryGetBoolField(TEXT("save"), bSave))
	{
		return FMonolithActionResult::Error(TEXT("save must be a boolean"));
	}

	const FString PackagePath = NormalizePackagePath(AssetPath);
	const FString ObjectPath = NormalizeObjectPath(PackagePath);
	UInputMappingContext* Context = LoadObject<UInputMappingContext>(nullptr, *ObjectPath);
	if (Context && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputMappingContext already exists: %s"), *AssetPath));
	}

	bool bCreated = false;
	if (!Context)
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		FString ExistError;
		if (!MonolithGAS::EnsureAssetPathFree(PackagePath, AssetName, ExistError))
		{
			return FMonolithActionResult::Error(ExistError);
		}

		UPackage* Package = MonolithGAS::GetOrCreatePackage(PackagePath, Error);
		if (!Package)
		{
			return FMonolithActionResult::Error(Error);
		}

		Context = NewObject<UInputMappingContext>(Package, *AssetName, RF_Public | RF_Standalone);
		if (!Context)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create InputMappingContext"));
		}
		FAssetRegistryModule::AssetCreated(Context);
		bCreated = true;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputMappingContext", "Create Input Mapping Context"));
	Context->Modify();
	if (Params->HasField(TEXT("description")))
	{
		FString Description;
		if (!Params->TryGetStringField(TEXT("description"), Description))
		{
			return FMonolithActionResult::Error(TEXT("description must be a string"));
		}
		Context->ContextDescription = FText::FromString(Description);
	}

	bool bSaved = false;
	if (!SaveAssetIfRequested(Context, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MappingContextToJson(Context);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleAddInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	const FString ContextPath = Params->GetStringField(TEXT("context_path"));
	const FString ActionPath = Params->GetStringField(TEXT("action_path"));
	const FString KeyName = Params->GetStringField(TEXT("key"));
	const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));

	FString Error;
	UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}

	UInputAction* Action = LoadInputAction(ActionPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	FKey Key;
	if (!ParseKey(KeyName, Key, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "AddInputMapping", "Add Input Mapping"));
	Context->Modify();
	const int32 Before = Context->GetMappings().Num();
	FEnhancedActionKeyMapping& Mapping = Context->MapKey(Action, Key);
	const int32 Index = Context->GetMappings().IndexOfByPredicate(
		[&Mapping](const FEnhancedActionKeyMapping& Candidate)
		{
			return &Candidate == &Mapping;
		});

	bool bSaved = false;
	if (!SaveAssetIfRequested(Context, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(TEXT("after_count"), Context->GetMappings().Num());
	Result->SetNumberField(TEXT("mapping_index"), Index);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleRemoveInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	const FString ContextPath = Params->GetStringField(TEXT("context_path"));
	const FString ActionPath = Params->GetStringField(TEXT("action_path"));
	const FString KeyName = Params->GetStringField(TEXT("key"));
	const bool bSave = !Params->HasField(TEXT("save")) || Params->GetBoolField(TEXT("save"));

	FString Error;
	UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
	if (!Context)
	{
		return FMonolithActionResult::Error(Error);
	}

	UInputAction* Action = LoadInputAction(ActionPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	FKey Key;
	if (!ParseKey(KeyName, Key, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "RemoveInputMapping", "Remove Input Mapping"));
	Context->Modify();
	const int32 Before = Context->GetMappings().Num();
	Context->UnmapKey(Action, Key);
	const int32 After = Context->GetMappings().Num();

	bool bSaved = false;
	if (!SaveAssetIfRequested(Context, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(TEXT("after_count"), After);
	Result->SetNumberField(TEXT("removed_count"), FMath::Max(0, Before - After));
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> ContextPaths = ReadContextPaths(Params);
	if (ContextPaths.Num() == 0)
	{
		TArray<FAssetData> Assets;
		GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets);
		for (const FAssetData& AssetData : Assets)
		{
			ContextPaths.Add(AssetData.GetObjectPathString());
		}
	}

	TArray<TSharedPtr<FJsonValue>> ContextResults;
	int32 ConflictCount = 0;
	int32 MissingActionCount = 0;
	int32 ContextLoadFailureCount = 0;

	for (const FString& ContextPath : ContextPaths)
	{
		FString Error;
		UInputMappingContext* Context = LoadInputMappingContext(ContextPath, Error);
		TSharedPtr<FJsonObject> ContextResult = MakeShared<FJsonObject>();
		ContextResult->SetStringField(TEXT("context_path"), ContextPath);
		if (!Context)
		{
			ContextLoadFailureCount++;
			ContextResult->SetBoolField(TEXT("valid"), false);
			ContextResult->SetStringField(TEXT("error"), Error);
			ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
			continue;
		}

		TMap<FString, TArray<FString>> KeyToActions;
		TArray<TSharedPtr<FJsonValue>> Issues;
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			const FEnhancedActionKeyMapping& Mapping = Mappings[Index];
			const FString KeyName = Mapping.Key.ToString();
			const FString ActionPath = Mapping.Action ? Mapping.Action->GetPathName() : TEXT("");

			if (!Mapping.Action)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("type"), TEXT("missing_action"));
				Issue->SetNumberField(TEXT("index"), Index);
				Issue->SetStringField(TEXT("key"), KeyName);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				MissingActionCount++;
				continue;
			}

			TArray<FString>& Actions = KeyToActions.FindOrAdd(KeyName);
			if (!Actions.Contains(ActionPath))
			{
				Actions.Add(ActionPath);
			}
		}

		for (const TPair<FString, TArray<FString>>& Pair : KeyToActions)
		{
			if (Pair.Value.Num() > 1)
			{
				TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
				Issue->SetStringField(TEXT("type"), TEXT("duplicate_key_conflict"));
				Issue->SetStringField(TEXT("key"), Pair.Key);
				TArray<TSharedPtr<FJsonValue>> ActionsJson;
				for (const FString& Action : Pair.Value)
				{
					ActionsJson.Add(MakeShared<FJsonValueString>(Action));
				}
				Issue->SetArrayField(TEXT("actions"), ActionsJson);
				Issues.Add(MakeShared<FJsonValueObject>(Issue));
				ConflictCount++;
			}
		}

		ContextResult->SetBoolField(TEXT("valid"), Issues.Num() == 0);
		ContextResult->SetStringField(TEXT("asset_path"), Context->GetPathName());
		ContextResult->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		ContextResult->SetArrayField(TEXT("issues"), Issues);
		ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), ConflictCount == 0 && MissingActionCount == 0 && ContextLoadFailureCount == 0);
	Result->SetNumberField(TEXT("contexts_checked"), ContextPaths.Num());
	Result->SetNumberField(TEXT("context_load_failures"), ContextLoadFailureCount);
	Result->SetNumberField(TEXT("conflicts"), ConflictCount);
	Result->SetNumberField(TEXT("missing_actions"), MissingActionCount);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	return FMonolithActionResult::Success(Result);
}
