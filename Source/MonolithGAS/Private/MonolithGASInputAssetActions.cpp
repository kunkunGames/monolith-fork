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
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FInputMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = false;
	};

	FMonolithActionResult InvalidParams(const FString& Message)
	{
		return FMonolithActionResult::Error(Message, -32602);
	}

	bool HasParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
	{
		return Params.IsValid() && Params->TryGetField(FieldName).IsValid();
	}

	bool ReadOptionalStringParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		FString& OutValue,
		FString& OutError,
		bool bAllowEmpty = true)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			return true;
		}
		if (Field->Type != EJson::String || !Field->TryGetString(OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a string"), FieldName);
			return false;
		}
		if (!bAllowEmpty && OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must not be empty"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadOptionalBoolParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool& OutValue,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid())
		{
			return true;
		}
		if (Field->Type != EJson::Boolean || !Field->TryGetBool(OutValue))
		{
			OutError = FString::Printf(TEXT("Malformed parameter: %s must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	bool ReadMutationOptions(
		const TSharedPtr<FJsonObject>& Params,
		FInputMutationOptions& OutOptions,
		FString& OutError)
	{
		if (!ReadOptionalBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError)
			|| !ReadOptionalBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError)
			|| !ReadOptionalBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError))
		{
			return false;
		}
		if (!OutOptions.bDryRun && !OutOptions.bConfirm)
		{
			OutError = TEXT("Mutation requires dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

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

	FString NormalizeInputAssetPackagePath(const FString& Path)
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

	bool NormalizeAndValidateInputAssetPath(
		const FString& InputPath,
		FString& OutPackagePath,
		FString& OutObjectPath,
		FString& OutError)
	{
		OutPackagePath = NormalizeInputAssetPackagePath(InputPath);
		if (!OutPackagePath.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("Input asset path '%s' must resolve under /Game"), *InputPath);
			return false;
		}

		FText InvalidReason;
		if (!FPackageName::IsValidLongPackageName(OutPackagePath, false, &InvalidReason))
		{
			OutError = FString::Printf(
				TEXT("Invalid input asset package path '%s': %s"),
				*OutPackagePath,
				*InvalidReason.ToString());
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(OutPackagePath);
		if (AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Input asset path '%s' must include an asset name"), *InputPath);
			return false;
		}

		OutObjectPath = NormalizeObjectPath(OutPackagePath);
		return true;
	}

	bool NormalizeAndValidateContentPath(
		const FString& InputPath,
		FString& OutPath,
		FString& OutError)
	{
		OutPath = InputPath;
		OutPath.TrimStartAndEndInline();
		if (OutPath.IsEmpty())
		{
			return true;
		}
		if (!OutPath.StartsWith(TEXT("/")))
		{
			OutPath = TEXT("/Game/") + OutPath;
		}
		OutPath.RemoveFromEnd(TEXT("/"));
		if (OutPath != TEXT("/Game") && !OutPath.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("Input asset search path '%s' must resolve under /Game"), *InputPath);
			return false;
		}
		return true;
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
		FString PackagePath;
		FString ObjectPath;
		if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Obj = MonolithGAS::LoadAssetFromPath(ObjectPath, OutError);
		UInputAction* Action = Cast<UInputAction>(Obj);
		if (!Action && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputAction asset not found: %s"), *Path);
		}
		return Action;
	}

	UInputMappingContext* LoadInputMappingContext(const FString& Path, FString& OutError)
	{
		FString PackagePath;
		FString ObjectPath;
		if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
		{
			return nullptr;
		}

		UObject* Obj = MonolithGAS::LoadAssetFromPath(ObjectPath, OutError);
		UInputMappingContext* Context = Cast<UInputMappingContext>(Obj);
		if (!Context && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("InputMappingContext asset not found: %s"), *Path);
		}
		return Context;
	}

	bool AreInstancedObjectsEquivalent(const UObject* A, const UObject* B)
	{
		if (A == B)
		{
			return true;
		}
		if (!A || !B || A->GetClass() != B->GetClass())
		{
			return false;
		}
		for (TFieldIterator<FProperty> It(A->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property || Property->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			const void* AValue = Property->ContainerPtrToValuePtr<const void>(A);
			const void* BValue = Property->ContainerPtrToValuePtr<const void>(B);
			if (!Property->Identical(AValue, BValue, PPF_None))
			{
				return false;
			}
		}
		return true;
	}

	template <typename TObjectType>
	bool AreInstancedObjectArraysEquivalent(const TArray<TObjectPtr<TObjectType>>& A, const TArray<TObjectPtr<TObjectType>>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!AreInstancedObjectsEquivalent(A[Index].Get(), B[Index].Get()))
			{
				return false;
			}
		}
		return true;
	}

	template <typename TObjectType>
	bool CloneInstancedObjectArray(
		const TArray<TObjectPtr<TObjectType>>& Source,
		UObject* Outer,
		TArray<TObjectPtr<TObjectType>>& OutClones,
		FString& OutError)
	{
		OutClones.Reset();
		for (TObjectType* SourceObject : Source)
		{
			if (!SourceObject)
			{
				OutClones.Add(nullptr);
				continue;
			}
			TObjectType* Clone = DuplicateObject<TObjectType>(SourceObject, Outer);
			if (!Clone)
			{
				OutError = FString::Printf(TEXT("Failed to duplicate instanced input object '%s'"), *SourceObject->GetPathName());
				return false;
			}
			OutClones.Add(Clone);
		}
		return true;
	}

	template <typename TObjectType>
	bool NewInstancedObjectArrayFromClasses(
		const TArray<UClass*>& Classes,
		UObject* Outer,
		TArray<TObjectPtr<TObjectType>>& OutObjects,
		FString& OutError)
	{
		OutObjects.Reset();
		for (UClass* Class : Classes)
		{
			if (!Class || !Class->IsChildOf(TObjectType::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(TEXT("Invalid input object class '%s'"), *GetPathNameSafe(Class));
				return false;
			}
			TObjectType* Object = NewObject<TObjectType>(Outer, Class, NAME_None, RF_Transactional);
			if (!Object)
			{
				OutError = FString::Printf(TEXT("Failed to create input object of class '%s'"), *Class->GetPathName());
				return false;
			}
			OutObjects.Add(Object);
		}
		return true;
	}

	bool ReadInputObjectClassArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		UClass* RequiredBaseClass,
		TArray<UClass*>& OutClasses,
		FString& OutError)
	{
		OutClasses.Reset();
		if (!HasParam(Params, FieldName))
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Field->Type != EJson::Array || !Field->TryGetArray(Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of class paths"), FieldName);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ClassPath;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(ClassPath) || ClassPath.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of class paths"), FieldName);
				return false;
			}
			ClassPath.TrimStartAndEndInline();
			UClass* Class = StaticLoadClass(RequiredBaseClass, nullptr, *ClassPath);
			if (!Class)
			{
				OutError = FString::Printf(TEXT("Could not load class '%s' for param '%s'"), *ClassPath, FieldName);
				return false;
			}
			if (!Class->IsChildOf(RequiredBaseClass) || Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutError = FString::Printf(TEXT("Class '%s' must be a non-abstract child of '%s'"), *Class->GetPathName(), *RequiredBaseClass->GetPathName());
				return false;
			}
			OutClasses.Add(Class);
		}
		return true;
	}

	int32 FindMappingIndexByActionAndKey(const UInputMappingContext* Context, const UInputAction* Action, const FKey& Key)
	{
		if (!Context || !Action)
		{
			return INDEX_NONE;
		}
		const TArray<FEnhancedActionKeyMapping>& Mappings = Context->GetMappings();
		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (Mappings[Index].Action == Action && Mappings[Index].Key == Key)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool AreMappingsEquivalentForAuthoring(const FEnhancedActionKeyMapping& A, const FEnhancedActionKeyMapping& B)
	{
		return A.Action == B.Action
			&& A.Key == B.Key
			&& AreInstancedObjectArraysEquivalent(A.Modifiers, B.Modifiers)
			&& AreInstancedObjectArraysEquivalent(A.Triggers, B.Triggers);
	}

	template <typename AssetType>
	AssetType* FindExistingInputAssetForCreate(const FString& ObjectPath, const TCHAR* ExpectedTypeName, FString& OutError)
	{
		if (AssetType* Existing = FindObject<AssetType>(nullptr, *ObjectPath))
		{
			return Existing;
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const FAssetData ExistingAsset = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (!ExistingAsset.IsValid())
		{
			return nullptr;
		}

		UObject* LoadedObject = ExistingAsset.GetAsset();
		if (AssetType* Existing = Cast<AssetType>(LoadedObject))
		{
			return Existing;
		}

		OutError = FString::Printf(
			TEXT("Asset already exists at '%s' but is not a %s."),
			*ObjectPath,
			ExpectedTypeName);
		return nullptr;
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

	bool GetAssetsByClass(UClass* Class, const TSharedPtr<FJsonObject>& Params, TArray<FAssetData>& OutAssets, FString& OutError)
	{
		FARFilter Filter;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = true;

		FString Path;
		if (!ReadOptionalStringParam(Params, TEXT("path"), Path, OutError))
		{
			return false;
		}
		if (!Path.IsEmpty())
		{
			FString NormalizedPath;
			if (!NormalizeAndValidateContentPath(Path, NormalizedPath, OutError))
			{
				return false;
			}
			Path = MoveTemp(NormalizedPath);
			Filter.PackagePaths.Add(FName(*Path));
			Filter.bRecursivePaths = true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
		OutAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.GetObjectPathString() < B.GetObjectPathString();
		});
		return true;
	}

	bool ReadContextPaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutPaths,
		FString& OutError)
	{
		OutPaths.Reset();
		if (!HasParam(Params, TEXT("context_paths")))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(TEXT("context_paths"));
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Field->Type != EJson::Array || !Field->TryGetArray(Values) || !Values)
		{
			OutError = TEXT("Malformed parameter: context_paths must be an array of non-empty strings");
			return false;
		}

		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			FString Path;
			if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Path) || Path.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Malformed parameter: context_paths[%d] must be a non-empty string"),
					Index);
				return false;
			}
			FString PackagePath;
			FString ObjectPath;
			if (!NormalizeAndValidateInputAssetPath(Path, PackagePath, ObjectPath, OutError))
			{
				return false;
			}
			OutPaths.AddUnique(ObjectPath);
		}
		return true;
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
		TEXT("Create or update a UInputAction asset. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputAction),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path, e.g. /Game/Input/IA_Jump"))
			.Optional(TEXT("value_type"), TEXT("string"), TEXT("Boolean, Axis1D, Axis2D, or Axis3D"), TEXT("Boolean"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("consume_input"), TEXT("boolean"), TEXT("Consume lower priority enhanced input mappings"), TEXT("true"))
			.Optional(TEXT("trigger_when_paused"), TEXT("boolean"), TEXT("Allow action while paused"), TEXT("false"))
			.Optional(TEXT("accumulation"), TEXT("string"), TEXT("TakeHighestAbsoluteValue or Cumulative"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing InputAction"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying or creating an asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("set_input_action_properties"),
		TEXT("Update common UInputAction properties. Requires dry_run=true or confirm=true."),
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
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying the asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
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
		TEXT("Create or update a UInputMappingContext asset. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputMappingContext),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path, e.g. /Game/Input/IMC_Default"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing context"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying or creating an asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("add_input_mapping"),
		TEXT("Add or update a key mapping on an Input Mapping Context. Requires dry_run=true or confirm=true. Idempotently reuses an existing action+key mapping unless allow_duplicate=true, and can clone modifiers/triggers from another mapping or instantiate explicit modifier/trigger classes."),
		FMonolithActionHandler::CreateStatic(&HandleAddInputMapping),
		FParamSchemaBuilder()
			.Required(TEXT("context_path"), TEXT("string"), TEXT("InputMappingContext asset path"))
			.Required(TEXT("action_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name, e.g. SpaceBar, LeftMouseButton, Gamepad_FaceButton_Bottom"))
			.Optional(TEXT("source_context_path"), TEXT("string"), TEXT("Optional source InputMappingContext to clone modifiers/triggers from"))
			.Optional(TEXT("source_action_path"), TEXT("string"), TEXT("Source InputAction for the mapping to clone"))
			.Optional(TEXT("source_key"), TEXT("string"), TEXT("Source FKey for the mapping to clone"))
			.Optional(TEXT("modifier_classes"), TEXT("array"), TEXT("Optional UInputModifier class paths. If present, replaces cloned/existing modifiers; empty array clears modifiers."))
			.Optional(TEXT("trigger_classes"), TEXT("array"), TEXT("Optional UInputTrigger class paths. If present, replaces cloned/existing triggers; empty array clears triggers."))
			.Optional(TEXT("allow_duplicate"), TEXT("boolean"), TEXT("Always add a new mapping instead of updating an existing action+key mapping"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("remove_input_mapping"),
		TEXT("Remove a key mapping from an Input Mapping Context. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&HandleRemoveInputMapping),
		FParamSchemaBuilder()
			.Required(TEXT("context_path"), TEXT("string"), TEXT("InputMappingContext asset path"))
			.Required(TEXT("action_path"), TEXT("string"), TEXT("InputAction asset path"))
			.Required(TEXT("key"), TEXT("string"), TEXT("FKey name to remove"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview without modifying the context"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for non-dry-run writes"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately after a confirmed change"), TEXT("false"))
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
	FString Error;
	if (!GetAssetsByClass(UInputAction::StaticClass(), Params, Assets, Error))
	{
		return InvalidParams(Error);
	}
	bool bIncludeDetails = false;
	if (!ReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return InvalidParams(Error);
	}

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
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}
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
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bOverwrite = false;
	if (!ReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return InvalidParams(Error);
	}

	FString PackagePath;
	FString ObjectPath;
	if (!NormalizeAndValidateInputAssetPath(AssetPath, PackagePath, ObjectPath, Error))
	{
		return InvalidParams(Error);
	}

	UInputAction* Action = FindExistingInputAssetForCreate<UInputAction>(ObjectPath, TEXT("InputAction"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Action && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputAction already exists: %s"), *AssetPath));
	}

	const bool bWillCreate = Action == nullptr;

	const bool bHasValueType = HasParam(Params, TEXT("value_type"));
	const bool bApplyValueType = bHasValueType || bWillCreate;
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (bApplyValueType)
	{
		FString ValueTypeString = TEXT("Boolean");
		if (!ReadOptionalStringParam(Params, TEXT("value_type"), ValueTypeString, Error))
		{
			return InvalidParams(Error);
		}
		if (!ParseValueType(ValueTypeString, ValueType))
		{
			return InvalidParams(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
		}
	}

	FString Description;
	const bool bHasDescription = HasParam(Params, TEXT("description"));
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	bool bConsumeInput = false;
	const bool bHasConsumeInput = HasParam(Params, TEXT("consume_input"));
	if (!ReadOptionalBoolParam(Params, TEXT("consume_input"), bConsumeInput, Error))
	{
		return InvalidParams(Error);
	}

	bool bTriggerWhenPaused = false;
	const bool bHasTriggerWhenPaused = HasParam(Params, TEXT("trigger_when_paused"));
	if (!ReadOptionalBoolParam(Params, TEXT("trigger_when_paused"), bTriggerWhenPaused, Error))
	{
		return InvalidParams(Error);
	}

	EInputActionAccumulationBehavior AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	const bool bHasAccumulation = HasParam(Params, TEXT("accumulation"));
	if (bHasAccumulation)
	{
		FString Accumulation;
		if (!ReadOptionalStringParam(Params, TEXT("accumulation"), Accumulation, Error))
		{
			return InvalidParams(Error);
		}
		if (!ParseAccumulation(Accumulation, AccumulationBehavior))
		{
			return InvalidParams(FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation));
		}
	}

	const bool bWouldChange = bWillCreate
		|| (bApplyValueType && Action->ValueType != ValueType)
		|| (bHasDescription && Action->ActionDescription.ToString() != Description)
		|| (bHasConsumeInput && Action->bConsumeInput != bConsumeInput)
		|| (bHasTriggerWhenPaused && Action->bTriggerWhenPaused != bTriggerWhenPaused)
		|| (bHasAccumulation && Action->AccumulationBehavior != AccumulationBehavior);

	if (Options.bDryRun)
	{
		TSharedPtr<FJsonObject> Result = Action ? InputActionToJson(Action) : MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), ObjectPath);
		Result->SetStringField(TEXT("package_path"), PackagePath);
		Result->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
		Result->SetBoolField(TEXT("would_create"), bWillCreate);
		Result->SetBoolField(TEXT("would_update"), !bWillCreate && bWouldChange);
		Result->SetBoolField(TEXT("would_change"), bWouldChange);
		Result->SetBoolField(TEXT("created"), false);
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
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

	if (bWouldChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputAction", "Create Input Action"));
		Action->Modify();

		if (bApplyValueType)
		{
			Action->ValueType = ValueType;
		}

		if (bHasDescription)
		{
			Action->ActionDescription = FText::FromString(Description);
		}
		if (bHasConsumeInput)
		{
			Action->bConsumeInput = bConsumeInput;
		}
		if (bHasTriggerWhenPaused)
		{
			Action->bTriggerWhenPaused = bTriggerWhenPaused;
		}
		if (bHasAccumulation)
		{
			Action->AccumulationBehavior = AccumulationBehavior;
		}
	}

	bool bSaved = false;
	if (bWouldChange && !SaveAssetIfRequested(Action, Options.bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = InputActionToJson(Action);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("changed"), bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleSetInputActionProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasValueType = HasParam(Params, TEXT("value_type"));
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (bHasValueType)
	{
		FString ValueTypeString;
		if (!ReadOptionalStringParam(Params, TEXT("value_type"), ValueTypeString, Error)
			|| !ParseValueType(ValueTypeString, ValueType))
		{
			return InvalidParams(Error.IsEmpty()
				? FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString)
				: Error);
		}
	}

	const bool bHasDescription = HasParam(Params, TEXT("description"));
	FString Description;
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasConsumeInput = HasParam(Params, TEXT("consume_input"));
	bool bConsumeInput = false;
	if (!ReadOptionalBoolParam(Params, TEXT("consume_input"), bConsumeInput, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasConsumeLegacy = HasParam(Params, TEXT("consume_legacy_mappings"));
	bool bConsumeLegacy = false;
	if (!ReadOptionalBoolParam(Params, TEXT("consume_legacy_mappings"), bConsumeLegacy, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasTriggerWhenPaused = HasParam(Params, TEXT("trigger_when_paused"));
	bool bTriggerWhenPaused = false;
	if (!ReadOptionalBoolParam(Params, TEXT("trigger_when_paused"), bTriggerWhenPaused, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasReserveMappings = HasParam(Params, TEXT("reserve_all_mappings"));
	bool bReserveMappings = false;
	if (!ReadOptionalBoolParam(Params, TEXT("reserve_all_mappings"), bReserveMappings, Error))
	{
		return InvalidParams(Error);
	}

	const bool bHasAccumulation = HasParam(Params, TEXT("accumulation"));
	EInputActionAccumulationBehavior AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	if (bHasAccumulation)
	{
		FString Accumulation;
		if (!ReadOptionalStringParam(Params, TEXT("accumulation"), Accumulation, Error)
			|| !ParseAccumulation(Accumulation, AccumulationBehavior))
		{
			return InvalidParams(Error.IsEmpty()
				? FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation)
				: Error);
		}
	}

	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	const bool bWouldChange =
		(bHasValueType && Action->ValueType != ValueType)
		|| (bHasDescription && Action->ActionDescription.ToString() != Description)
		|| (bHasConsumeInput && Action->bConsumeInput != bConsumeInput)
		|| (bHasConsumeLegacy && Action->bConsumesActionAndAxisMappings != bConsumeLegacy)
		|| (bHasTriggerWhenPaused && Action->bTriggerWhenPaused != bTriggerWhenPaused)
		|| (bHasReserveMappings && Action->bReserveAllMappings != bReserveMappings)
		|| (bHasAccumulation && Action->AccumulationBehavior != AccumulationBehavior);

	if (!Options.bDryRun && bWouldChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "SetInputActionProperties", "Set Input Action Properties"));
		Action->Modify();
		if (bHasValueType) Action->ValueType = ValueType;
		if (bHasDescription) Action->ActionDescription = FText::FromString(Description);
		if (bHasConsumeInput) Action->bConsumeInput = bConsumeInput;
		if (bHasConsumeLegacy) Action->bConsumesActionAndAxisMappings = bConsumeLegacy;
		if (bHasTriggerWhenPaused) Action->bTriggerWhenPaused = bTriggerWhenPaused;
		if (bHasReserveMappings) Action->bReserveAllMappings = bReserveMappings;
		if (bHasAccumulation) Action->AccumulationBehavior = AccumulationBehavior;
	}

	bool bSaved = false;
	if (!Options.bDryRun && bWouldChange && !SaveAssetIfRequested(Action, Options.bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = InputActionToJson(Action);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputMappingContexts(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	FString Error;
	if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
	{
		return InvalidParams(Error);
	}
	bool bIncludeDetails = false;
	if (!ReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return InvalidParams(Error);
	}

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
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}
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
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bOverwrite = false;
	if (!ReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return InvalidParams(Error);
	}

	FString PackagePath;
	FString ObjectPath;
	if (!NormalizeAndValidateInputAssetPath(AssetPath, PackagePath, ObjectPath, Error))
	{
		return InvalidParams(Error);
	}

	UInputMappingContext* Context = FindExistingInputAssetForCreate<UInputMappingContext>(ObjectPath, TEXT("InputMappingContext"), Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error);
	}
	if (Context && !bOverwrite)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("InputMappingContext already exists: %s"), *AssetPath));
	}

	FString Description;
	const bool bHasDescription = HasParam(Params, TEXT("description"));
	if (!ReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return InvalidParams(Error);
	}

	const bool bWillCreate = Context == nullptr;
	const bool bWouldChange = bWillCreate
		|| (bHasDescription && Context->ContextDescription.ToString() != Description);

	if (Options.bDryRun)
	{
		TSharedPtr<FJsonObject> Result = Context ? MappingContextToJson(Context) : MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset_path"), ObjectPath);
		Result->SetStringField(TEXT("package_path"), PackagePath);
		Result->SetStringField(TEXT("name"), FPackageName::GetLongPackageAssetName(PackagePath));
		Result->SetBoolField(TEXT("would_create"), bWillCreate);
		Result->SetBoolField(TEXT("would_update"), !bWillCreate && bWouldChange);
		Result->SetBoolField(TEXT("would_change"), bWouldChange);
		Result->SetBoolField(TEXT("created"), false);
		Result->SetBoolField(TEXT("changed"), false);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Success(Result);
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

	if (bWouldChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "CreateInputMappingContext", "Create Input Mapping Context"));
		Context->Modify();
		if (bHasDescription)
		{
			Context->ContextDescription = FText::FromString(Description);
		}
	}

	bool bSaved = false;
	if (bWouldChange && !SaveAssetIfRequested(Context, Options.bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MappingContextToJson(Context);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("changed"), bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), false);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleAddInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString ContextPath;
	FString ActionPath;
	FString KeyName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("context_path"), ContextPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("action_path"), ActionPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("key"), KeyName, Err)) return Err;

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

	bool bAllowDuplicate = false;
	if (!ReadOptionalBoolParam(Params, TEXT("allow_duplicate"), bAllowDuplicate, Error))
	{
		return InvalidParams(Error);
	}

	TArray<UClass*> ModifierClasses;
	TArray<UClass*> TriggerClasses;
	if (!ReadInputObjectClassArray(Params, TEXT("modifier_classes"), UInputModifier::StaticClass(), ModifierClasses, Error)
		|| !ReadInputObjectClassArray(Params, TEXT("trigger_classes"), UInputTrigger::StaticClass(), TriggerClasses, Error))
	{
		return InvalidParams(Error);
	}
	const bool bHasModifierClasses = HasParam(Params, TEXT("modifier_classes"));
	const bool bHasTriggerClasses = HasParam(Params, TEXT("trigger_classes"));

	const bool bHasSourceContextPath = HasParam(Params, TEXT("source_context_path"));
	const bool bHasSourceActionPath = HasParam(Params, TEXT("source_action_path"));
	const bool bHasSourceKey = HasParam(Params, TEXT("source_key"));
	const bool bHasAnySourceField = bHasSourceContextPath || bHasSourceActionPath || bHasSourceKey;
	if (bHasAnySourceField && !(bHasSourceContextPath && bHasSourceActionPath && bHasSourceKey))
	{
		return InvalidParams(
			TEXT("source_context_path, source_action_path, and source_key must be provided together"));
	}

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
		return InvalidParams(Error);
	}

	const FEnhancedActionKeyMapping* SourceMapping = nullptr;
	if (bHasAnySourceField)
	{
		FString SourceContextPath;
		FString SourceActionPath;
		FString SourceKeyName;
		if (!ReadOptionalStringParam(Params, TEXT("source_context_path"), SourceContextPath, Error, false)
			|| !ReadOptionalStringParam(Params, TEXT("source_action_path"), SourceActionPath, Error, false)
			|| !ReadOptionalStringParam(Params, TEXT("source_key"), SourceKeyName, Error, false))
		{
			return InvalidParams(Error);
		}

		UInputMappingContext* SourceContext = LoadInputMappingContext(SourceContextPath, Error);
		if (!SourceContext)
		{
			return FMonolithActionResult::Error(Error);
		}
		UInputAction* SourceAction = LoadInputAction(SourceActionPath, Error);
		if (!SourceAction)
		{
			return FMonolithActionResult::Error(Error);
		}
		FKey SourceKey;
		if (!ParseKey(SourceKeyName, SourceKey, Error))
		{
			return InvalidParams(Error);
		}
		const int32 SourceIndex = FindMappingIndexByActionAndKey(SourceContext, SourceAction, SourceKey);
		if (SourceIndex == INDEX_NONE)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Source mapping not found for action '%s' and key '%s' in '%s'."),
				*SourceAction->GetPathName(),
				*SourceKey.ToString(),
				*SourceContext->GetPathName()));
		}
		SourceMapping = &SourceContext->GetMappings()[SourceIndex];
	}

	const int32 Before = Context->GetMappings().Num();
	int32 ExistingIndex = bAllowDuplicate ? INDEX_NONE : FindMappingIndexByActionAndKey(Context, Action, Key);

	FEnhancedActionKeyMapping DesiredMapping(Action, Key);
	const UObject* DesiredOuter = GetTransientPackage();
	if (SourceMapping)
	{
		if (!CloneInstancedObjectArray(SourceMapping->Modifiers, const_cast<UObject*>(DesiredOuter), DesiredMapping.Modifiers, Error)
			|| !CloneInstancedObjectArray(SourceMapping->Triggers, const_cast<UObject*>(DesiredOuter), DesiredMapping.Triggers, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}
	else if (ExistingIndex != INDEX_NONE)
	{
		const FEnhancedActionKeyMapping& ExistingMapping = Context->GetMappings()[ExistingIndex];
		if (!CloneInstancedObjectArray(ExistingMapping.Modifiers, const_cast<UObject*>(DesiredOuter), DesiredMapping.Modifiers, Error)
			|| !CloneInstancedObjectArray(ExistingMapping.Triggers, const_cast<UObject*>(DesiredOuter), DesiredMapping.Triggers, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	if (bHasModifierClasses)
	{
		if (!NewInstancedObjectArrayFromClasses(ModifierClasses, const_cast<UObject*>(DesiredOuter), DesiredMapping.Modifiers, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}
	if (bHasTriggerClasses)
	{
		if (!NewInstancedObjectArrayFromClasses(TriggerClasses, const_cast<UObject*>(DesiredOuter), DesiredMapping.Triggers, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	const bool bWouldCreate = ExistingIndex == INDEX_NONE;
	bool bWouldUpdate = false;
	int32 MappingIndex = ExistingIndex;
	if (ExistingIndex != INDEX_NONE)
	{
		const FEnhancedActionKeyMapping& ExistingMapping = Context->GetMappings()[ExistingIndex];
		bWouldUpdate = !AreMappingsEquivalentForAuthoring(ExistingMapping, DesiredMapping);
	}

	const bool bWouldChange = bWouldCreate || bWouldUpdate;
	if (bWouldCreate && Options.bDryRun)
	{
		MappingIndex = Before;
	}

	if (bWouldChange && !Options.bDryRun)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "AddInputMapping", "Add Input Mapping"));
		Context->Modify();
		if (bWouldCreate)
		{
			FEnhancedActionKeyMapping& NewMapping = Context->MapKey(Action, Key);
			MappingIndex = Context->GetMappings().IndexOfByPredicate(
				[&NewMapping](const FEnhancedActionKeyMapping& Candidate)
				{
					return &Candidate == &NewMapping;
				});
		}

		FEnhancedActionKeyMapping& TargetMapping = Context->GetMapping(MappingIndex);
		TargetMapping.Action = Action;
		TargetMapping.Key = Key;
		if (!CloneInstancedObjectArray(DesiredMapping.Modifiers, Context, TargetMapping.Modifiers, Error)
			|| !CloneInstancedObjectArray(DesiredMapping.Triggers, Context, TargetMapping.Triggers, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	bool bSaved = false;
	if (bWouldChange && !Options.bDryRun && !SaveAssetIfRequested(Context, Options.bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(
		TEXT("after_count"),
		Options.bDryRun ? Before + (bWouldCreate ? 1 : 0) : Context->GetMappings().Num());
	Result->SetNumberField(TEXT("mapping_index"), MappingIndex);
	Result->SetBoolField(TEXT("would_create"), bWouldCreate);
	Result->SetBoolField(TEXT("would_update"), bWouldUpdate);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("created"), !Options.bDryRun && bWouldCreate);
	Result->SetBoolField(TEXT("updated"), !Options.bDryRun && bWouldUpdate);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && bWouldChange);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);
	Result->SetBoolField(TEXT("cloned_from_source"), SourceMapping != nullptr);
	Result->SetNumberField(TEXT("modifier_count"), DesiredMapping.Modifiers.Num());
	Result->SetNumberField(TEXT("trigger_count"), DesiredMapping.Triggers.Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleRemoveInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString ContextPath;
	FString ActionPath;
	FString KeyName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("context_path"), ContextPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("action_path"), ActionPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("key"), KeyName, Err)) return Err;

	FString Error;
	FInputMutationOptions Options;
	if (!ReadMutationOptions(Params, Options, Error))
	{
		return InvalidParams(Error);
	}

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
		return InvalidParams(Error);
	}

	const int32 Before = Context->GetMappings().Num();
	int32 WouldRemoveCount = 0;
	for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
	{
		if (Mapping.Action == Action && Mapping.Key == Key)
		{
			++WouldRemoveCount;
		}
	}
	const bool bWouldChange = WouldRemoveCount > 0;

	int32 RemovedCount = 0;
	if (bWouldChange && !Options.bDryRun)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "RemoveInputMapping", "Remove Input Mapping"));
		Context->Modify();
		Context->UnmapKey(Action, Key);
		RemovedCount = Before - Context->GetMappings().Num();
	}
	const int32 After = Options.bDryRun ? Before - WouldRemoveCount : Context->GetMappings().Num();

	bool bSaved = false;
	if (bWouldChange && !Options.bDryRun && !SaveAssetIfRequested(Context, Options.bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(TEXT("after_count"), After);
	Result->SetNumberField(TEXT("would_remove_count"), WouldRemoveCount);
	Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && RemovedCount > 0);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("saved"), bSaved);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleValidateInputMappings(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> ContextPaths;
	FString ParamError;
	if (!ReadContextPaths(Params, ContextPaths, ParamError))
	{
		return InvalidParams(ParamError);
	}

	if (ContextPaths.Num() == 0)
	{
		TArray<FAssetData> Assets;
		FString Error;
		if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
		{
			return InvalidParams(Error);
		}
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
