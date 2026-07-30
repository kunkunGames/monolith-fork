#include "MonolithGASInputAssetActions.h"

#include "MonolithGASInternal.h"
#include "MonolithParamSchema.h"
#include "MonolithParamUtils.h"

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
#include "PlayerMappableKeySettings.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

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

	bool ParseTrackingMode(const FString& Input, EMappingContextRegistrationTrackingMode& OutMode)
	{
		if (Input.Equals(TEXT("Untracked"), ESearchCase::IgnoreCase))
		{
			OutMode = EMappingContextRegistrationTrackingMode::Untracked;
			return true;
		}
		if (Input.Equals(TEXT("CountRegistrations"), ESearchCase::IgnoreCase))
		{
			OutMode = EMappingContextRegistrationTrackingMode::CountRegistrations;
			return true;
		}
		return false;
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
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of class paths"), FieldName);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString ClassPath;
			if (!Value.IsValid() || !Value->TryGetString(ClassPath))
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

	struct FMonolithGASInputExactDuplicateGroup
	{
		FString ActionPath;
		FString KeyName;
		TArray<int32> Indices;
	};

	TArray<FMonolithGASInputExactDuplicateGroup> FindMonolithGASInputExactDuplicateGroups(
		const TArray<FEnhancedActionKeyMapping>& Mappings)
	{
		TArray<FMonolithGASInputExactDuplicateGroup> Groups;
		TArray<bool> bConsumed;
		bConsumed.Init(false, Mappings.Num());

		for (int32 Index = 0; Index < Mappings.Num(); ++Index)
		{
			if (bConsumed[Index])
			{
				continue;
			}

			const FEnhancedActionKeyMapping& Mapping = Mappings[Index];
			if (!Mapping.Action || !Mapping.Key.IsValid())
			{
				continue;
			}

			FMonolithGASInputExactDuplicateGroup Group;
			Group.ActionPath = Mapping.Action->GetPathName();
			Group.KeyName = Mapping.Key.ToString();
			Group.Indices.Add(Index);
			bConsumed[Index] = true;

			for (int32 CandidateIndex = Index + 1; CandidateIndex < Mappings.Num(); ++CandidateIndex)
			{
				if (bConsumed[CandidateIndex])
				{
					continue;
				}

				const FEnhancedActionKeyMapping& Candidate = Mappings[CandidateIndex];
				if (Candidate.Action
					&& Candidate.Key.IsValid()
					&& AreMappingsEquivalentForAuthoring(Mapping, Candidate))
				{
					Group.Indices.Add(CandidateIndex);
					bConsumed[CandidateIndex] = true;
				}
			}

			if (Group.Indices.Num() > 1)
			{
				Groups.Add(MoveTemp(Group));
			}
		}

		return Groups;
	}

	struct FMonolithGASInputSharedKeyGroup
	{
		TArray<FString> ActionPaths;
		TArray<int32> Indices;
	};

	bool ResolvePlayerMappableProperties(
		FProperty*& OutBehaviorProperty,
		FObjectPropertyBase*& OutSettingsProperty,
		FString& OutError)
	{
		UScriptStruct* MappingStruct = FEnhancedActionKeyMapping::StaticStruct();
		OutBehaviorProperty = MappingStruct
			? MappingStruct->FindPropertyByName(TEXT("SettingBehavior"))
			: nullptr;
		OutSettingsProperty = MappingStruct
			? CastField<FObjectPropertyBase>(MappingStruct->FindPropertyByName(TEXT("PlayerMappableKeySettings")))
			: nullptr;

		if ((!CastField<FEnumProperty>(OutBehaviorProperty) && !CastField<FByteProperty>(OutBehaviorProperty))
			|| !OutSettingsProperty)
		{
			OutError = TEXT(
				"UE Enhanced Input FEnhancedActionKeyMapping player-mappable property layout is unsupported; "
				"expected SettingBehavior enum and PlayerMappableKeySettings object properties.");
			return false;
		}
		return true;
	}

	bool ReadPlayerMappableBehavior(
		const FEnhancedActionKeyMapping& Mapping,
		EPlayerMappableKeySettingBehaviors& OutBehavior,
		FString& OutError)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, OutError))
		{
			return false;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(BehaviorProperty))
		{
			const void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<const void>(&Mapping);
			OutBehavior = static_cast<EPlayerMappableKeySettingBehaviors>(
				EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr));
			return true;
		}

		const FByteProperty* ByteProperty = CastFieldChecked<FByteProperty>(BehaviorProperty);
		OutBehavior = static_cast<EPlayerMappableKeySettingBehaviors>(
			ByteProperty->GetPropertyValue_InContainer(&Mapping));
		return true;
	}

	bool WritePlayerMappableBehavior(
		FEnhancedActionKeyMapping& Mapping,
		EPlayerMappableKeySettingBehaviors Behavior,
		UPlayerMappableKeySettings* Settings,
		FString& OutError)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, OutError))
		{
			return false;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(BehaviorProperty))
		{
			void* ValuePtr = EnumProperty->ContainerPtrToValuePtr<void>(&Mapping);
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(
				ValuePtr,
				static_cast<int64>(Behavior));
		}
		else
		{
			CastFieldChecked<FByteProperty>(BehaviorProperty)->SetPropertyValue_InContainer(
				&Mapping,
				static_cast<uint8>(Behavior));
		}
		SettingsProperty->SetObjectPropertyValue_InContainer(&Mapping, Settings);
		return true;
	}

	bool IsPlayerMappableStateEquivalent(
		const FEnhancedActionKeyMapping& Mapping,
		bool bPlayerMappable,
		const FString& MappingName,
		const FString& DisplayName,
		const FString& DisplayCategory,
		const TArray<FString>& SupportedKeyProfileIds,
		bool& bOutEquivalent,
		FString& OutError)
	{
		EPlayerMappableKeySettingBehaviors Behavior;
		if (!ReadPlayerMappableBehavior(Mapping, Behavior, OutError))
		{
			return false;
		}

		if (!bPlayerMappable)
		{
			bOutEquivalent = Behavior == EPlayerMappableKeySettingBehaviors::IgnoreSettings;
			return true;
		}

		const UPlayerMappableKeySettings* Settings = Mapping.GetPlayerMappableKeySettings();
		bOutEquivalent =
			Behavior == EPlayerMappableKeySettingBehaviors::OverrideSettings
			&& Settings
			&& Settings->Name == FName(*MappingName)
			&& Settings->DisplayName.EqualTo(FText::FromString(DisplayName))
			&& Settings->DisplayCategory.EqualTo(FText::FromString(DisplayCategory))
			&& Settings->SupportedKeyProfileIds == SupportedKeyProfileIds;
		return true;
	}

	bool ConfigurePlayerMappableState(
		FEnhancedActionKeyMapping& Mapping,
		UObject* Outer,
		bool bPlayerMappable,
		const FString& MappingName,
		const FString& DisplayName,
		const FString& DisplayCategory,
		const TArray<FString>& SupportedKeyProfileIds,
		FString& OutError)
	{
		UPlayerMappableKeySettings* Settings = nullptr;
		EPlayerMappableKeySettingBehaviors Behavior = EPlayerMappableKeySettingBehaviors::IgnoreSettings;
		if (bPlayerMappable)
		{
			Settings = NewObject<UPlayerMappableKeySettings>(
				Outer,
				UPlayerMappableKeySettings::StaticClass(),
				NAME_None,
				RF_Transactional);
			if (!Settings)
			{
				OutError = TEXT("Failed to create UPlayerMappableKeySettings for the input mapping.");
				return false;
			}
			Settings->Name = FName(*MappingName);
			Settings->DisplayName = FText::FromString(DisplayName);
			Settings->DisplayCategory = FText::FromString(DisplayCategory);
			Settings->SupportedKeyProfileIds = SupportedKeyProfileIds;
			Behavior = EPlayerMappableKeySettingBehaviors::OverrideSettings;
		}

		return WritePlayerMappableBehavior(Mapping, Behavior, Settings, OutError);
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
		if (!MonolithGAS::TryReadOptionalStringParam(Params, TEXT("path"), Path, OutError))
		{
			return false;
		}
		if (!Path.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*Path));
			Filter.bRecursivePaths = true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		AssetRegistryModule.Get().GetAssets(Filter, OutAssets);
		return true;
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
		TEXT("Create or update a UInputMappingContext asset and its registration ownership policy"),
		FMonolithActionHandler::CreateStatic(&HandleCreateInputMappingContext),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Package path, e.g. /Game/Input/IMC_Default"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Localized description text"))
			.Optional(TEXT("registration_tracking_mode"), TEXT("string"), TEXT("Untracked or CountRegistrations"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Allow updating an existing context"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save package immediately"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("input"), TEXT("add_input_mapping"),
		TEXT("Add or update a key mapping on an Input Mapping Context. Idempotently reuses an existing action+key mapping unless allow_duplicate=true, can clone modifiers/triggers, and can author per-row Enhanced Input player-mappable metadata."),
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
			.Optional(TEXT("player_mappable"), TEXT("boolean"), TEXT("When true, author per-mapping override metadata; when false, explicitly opt this row out. Omit to preserve existing behavior."))
			.Optional(TEXT("mapping_name"), TEXT("string"), TEXT("Stable save/remap row name; required when player_mappable=true"))
			.Optional(TEXT("display_name"), TEXT("string"), TEXT("Localized settings-screen row label; required when player_mappable=true"))
			.Optional(TEXT("display_category"), TEXT("string"), TEXT("Localized settings-screen category; required when player_mappable=true"))
			.Optional(TEXT("supported_key_profile_ids"), TEXT("array"), TEXT("Optional array of Enhanced Input key profile IDs for this row"))
			.Optional(TEXT("allow_duplicate"), TEXT("boolean"), TEXT("Always add a new mapping instead of updating an existing action+key mapping"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
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
		TEXT("Validate Enhanced Input Mapping Contexts for missing actions and exact duplicate mappings; report legal shared keys and unbound rows separately"),
		FMonolithActionHandler::CreateStatic(&HandleValidateInputMappings),
		FParamSchemaBuilder()
			.Optional(TEXT("context_paths"), TEXT("array"), TEXT("Specific InputMappingContext paths; omitted means all contexts"))
			.Optional(TEXT("path"), TEXT("string"), TEXT("Optional package path root when context_paths is omitted"))
			.Optional(TEXT("fail_on_unbound"), TEXT("boolean"), TEXT("Treat EKeys::Invalid/None rows as validation errors instead of informational unbound mappings"), TEXT("false"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("create_input_action"),
		{ TEXT("enhanced input"), TEXT("IA asset"), TEXT("value type"), TEXT("axis2d"), TEXT("boolean action"), TEXT("jump fire move") },
		{ TEXT("new_input_action"), TEXT("make_ia"), TEXT("add_input_action") },
		{ TEXT("create an Input Action asset IA_Jump"), TEXT("make a 2D axis input action for movement") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("create_input_mapping_context"),
		{ TEXT("enhanced input"), TEXT("IMC asset"), TEXT("mapping context"), TEXT("input context"), TEXT("default mapping"), TEXT("registration tracking"), TEXT("CountRegistrations"), TEXT("shared ownership") },
		{ TEXT("new_input_mapping_context"), TEXT("make_imc"), TEXT("create_imc") },
		{ TEXT("create an Input Mapping Context IMC_Default"), TEXT("make a new IMC for the player"), TEXT("set an IMC to CountRegistrations for shared ownership") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("add_input_mapping"),
		{ TEXT("bind key"), TEXT("key mapping"), TEXT("map key to action"), TEXT("FKey"), TEXT("spacebar gamepad"), TEXT("keybind"), TEXT("modifiers"), TEXT("triggers"), TEXT("clone input mapping") },
		{ TEXT("map_key"), TEXT("bind_key"), TEXT("add_key_mapping"), TEXT("add_keybinding"), TEXT("clone_key_mapping") },
		{ TEXT("bind SpaceBar to IA_Jump in IMC_Default"), TEXT("clone modifiers and triggers from an existing input mapping") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("validate_input_mappings"),
		{ TEXT("duplicate mapping"), TEXT("exact duplicate"), TEXT("shared key"), TEXT("missing action"), TEXT("unbound"), TEXT("lint input"), TEXT("check bindings") },
		{ TEXT("check_input_mappings"), TEXT("lint_input"), TEXT("find_mapping_conflicts") },
		{ TEXT("find exact duplicate input mappings"), TEXT("report shared keys without treating them as conflicts"), TEXT("check input mappings for missing actions and unbound rows") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("get_input_action"),
		{ TEXT("inspect IA"), TEXT("value type"), TEXT("triggers modifiers"), TEXT("action properties"), TEXT("read input action") },
		{ TEXT("describe_input_action"), TEXT("show_ia"), TEXT("read_input_action") },
		{ TEXT("what value type does IA_Move use"), TEXT("inspect an Input Action's triggers and modifiers") });
	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("input"), TEXT("get_input_mapping_context"),
		{ TEXT("inspect IMC"), TEXT("list mappings"), TEXT("which keys"), TEXT("context bindings"), TEXT("read mapping context") },
		{ TEXT("describe_input_mapping_context"), TEXT("show_imc"), TEXT("list_mappings") },
		{ TEXT("what keys are bound in IMC_Default"), TEXT("inspect the mappings of an Input Mapping Context") });
}

FMonolithActionResult FMonolithGASInputAssetActions::HandleListInputActions(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FAssetData> Assets;
	FString Error;
	if (!GetAssetsByClass(UInputAction::StaticClass(), Params, Assets, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	bool bIncludeDetails = false;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return FMonolithActionResult::Error(Error);
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
	bool bOverwrite = false;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	bool bSave = true;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString PackagePath = NormalizeInputAssetPackagePath(AssetPath);
	const FString ObjectPath = NormalizeObjectPath(PackagePath);
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

	TSharedPtr<FJsonValue> ValueTypeField = Params->TryGetField(TEXT("value_type"));
	const bool bApplyValueType = ValueTypeField.IsValid() || bWillCreate;
	EInputActionValueType ValueType = EInputActionValueType::Boolean;
	if (bApplyValueType)
	{
		FString ValueTypeString = TEXT("Boolean");
		if (!MonolithGAS::TryReadOptionalStringParam(Params, TEXT("value_type"), ValueTypeString, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (!ParseValueType(ValueTypeString, ValueType))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
		}
	}

	FString Description;
	bool bHasDescription = Params->TryGetField(TEXT("description")).IsValid();
	if (!MonolithGAS::TryReadOptionalStringParam(Params, TEXT("description"), Description, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bConsumeInput = false;
	bool bHasConsumeInput = Params->TryGetField(TEXT("consume_input")).IsValid();
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("consume_input"), bConsumeInput, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bTriggerWhenPaused = false;
	bool bHasTriggerWhenPaused = Params->TryGetField(TEXT("trigger_when_paused")).IsValid();
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("trigger_when_paused"), bTriggerWhenPaused, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	EInputActionAccumulationBehavior AccumulationBehavior = EInputActionAccumulationBehavior::TakeHighestAbsoluteValue;
	const bool bHasAccumulation = Params->TryGetField(TEXT("accumulation")).IsValid();
	if (bHasAccumulation)
	{
		FString Accumulation;
		if (!MonolithGAS::TryReadOptionalStringParam(Params, TEXT("accumulation"), Accumulation, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		if (!ParseAccumulation(Accumulation, AccumulationBehavior))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid accumulation: %s"), *Accumulation));
		}
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
	FString AssetPath;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("asset_path"), AssetPath, Err))
	{
		return Err;
	}
	bool bSave = true;
	FString Error;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	UInputAction* Action = LoadInputAction(AssetPath, Error);
	if (!Action)
	{
		return FMonolithActionResult::Error(Error);
	}

	const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "SetInputActionProperties", "Set Input Action Properties"));
	Action->Modify();

	TSharedPtr<FJsonValue> ValueTypeValPtr = Params->TryGetField(TEXT("value_type"));
	if (ValueTypeValPtr.IsValid())
	{
		EInputActionValueType ValueType;
		FString ValueTypeString;
		if (ValueTypeValPtr->Type != EJson::String || !ValueTypeValPtr->TryGetString(ValueTypeString))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: value_type must be a string"));
		}
		if (!ParseValueType(ValueTypeString, ValueType))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid value_type: %s"), *ValueTypeString));
		}
		Action->ValueType = ValueType;
	}
	TSharedPtr<FJsonValue> DescriptionValPtr = Params->TryGetField(TEXT("description"));
	if (DescriptionValPtr.IsValid())
	{
		FString Description;
		if (DescriptionValPtr->Type != EJson::String || !DescriptionValPtr->TryGetString(Description))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: description must be a string"));
		}
		Action->ActionDescription = FText::FromString(Description);
	}
	TSharedPtr<FJsonValue> ConsumeInputValPtr = Params->TryGetField(TEXT("consume_input"));
	if (ConsumeInputValPtr.IsValid())
	{
		bool bConsumeInput = false;
		if (ConsumeInputValPtr->Type != EJson::Boolean || !ConsumeInputValPtr->TryGetBool(bConsumeInput))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: consume_input must be a boolean"));
		}
		Action->bConsumeInput = bConsumeInput;
	}
	TSharedPtr<FJsonValue> ConsumeLegacyValPtr = Params->TryGetField(TEXT("consume_legacy_mappings"));
	if (ConsumeLegacyValPtr.IsValid())
	{
		bool bConsumeLegacy = false;
		if (ConsumeLegacyValPtr->Type != EJson::Boolean || !ConsumeLegacyValPtr->TryGetBool(bConsumeLegacy))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: consume_legacy_mappings must be a boolean"));
		}
		Action->bConsumesActionAndAxisMappings = bConsumeLegacy;
	}
	TSharedPtr<FJsonValue> TriggerPausedValPtr = Params->TryGetField(TEXT("trigger_when_paused"));
	if (TriggerPausedValPtr.IsValid())
	{
		bool bTriggerWhenPaused = false;
		if (TriggerPausedValPtr->Type != EJson::Boolean || !TriggerPausedValPtr->TryGetBool(bTriggerWhenPaused))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: trigger_when_paused must be a boolean"));
		}
		Action->bTriggerWhenPaused = bTriggerWhenPaused;
	}
	TSharedPtr<FJsonValue> ReserveMappingsValPtr = Params->TryGetField(TEXT("reserve_all_mappings"));
	if (ReserveMappingsValPtr.IsValid())
	{
		bool bReserveMappings = false;
		if (ReserveMappingsValPtr->Type != EJson::Boolean || !ReserveMappingsValPtr->TryGetBool(bReserveMappings))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: reserve_all_mappings must be a boolean"));
		}
		Action->bReserveAllMappings = bReserveMappings;
	}
	TSharedPtr<FJsonValue> AccumulationValPtr = Params->TryGetField(TEXT("accumulation"));
	if (AccumulationValPtr.IsValid())
	{
		EInputActionAccumulationBehavior Behavior;
		FString Accumulation;
		if (AccumulationValPtr->Type != EJson::String || !AccumulationValPtr->TryGetString(Accumulation))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: accumulation must be a string"));
		}
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
	FString Error;
	if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	bool bIncludeDetails = false;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("include_details"), bIncludeDetails, Error))
	{
		return FMonolithActionResult::Error(Error);
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
	bool bOverwrite = false;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("overwrite"), bOverwrite, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	bool bSave = true;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString PackagePath = NormalizeInputAssetPackagePath(AssetPath);
	const FString ObjectPath = NormalizeObjectPath(PackagePath);
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
	TSharedPtr<FJsonValue> DescValPtr = Params->TryGetField(TEXT("description"));
	bool bHasDescription = DescValPtr.IsValid();
	if (bHasDescription && (DescValPtr->Type != EJson::String || !DescValPtr->TryGetString(Description)))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter: description must be a string"));
	}

	FString TrackingModeText;
	const bool bHasTrackingMode = Params.IsValid() && Params->HasField(TEXT("registration_tracking_mode"));
	if (bHasTrackingMode &&
		!MonolithGAS::TryReadOptionalStringParam(
			Params,
			TEXT("registration_tracking_mode"),
			TrackingModeText,
			Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	EMappingContextRegistrationTrackingMode TrackingMode =
		EMappingContextRegistrationTrackingMode::Untracked;
	FEnumProperty* TrackingModeProperty = nullptr;
	if (bHasTrackingMode)
	{
		if (!ParseTrackingMode(TrackingModeText, TrackingMode))
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Invalid registration_tracking_mode '%s'; expected Untracked or CountRegistrations"),
					*TrackingModeText));
		}
		TrackingModeProperty = FindFProperty<FEnumProperty>(
			UInputMappingContext::StaticClass(),
			TEXT("RegistrationTrackingMode"));
		if (!TrackingModeProperty)
		{
			return FMonolithActionResult::Error(
				TEXT("UInputMappingContext.RegistrationTrackingMode reflection property was not found"));
		}
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
	if (bHasDescription)
	{
		Context->ContextDescription = FText::FromString(Description);
	}
	if (TrackingModeProperty)
	{
		void* TrackingModeValue = TrackingModeProperty->ContainerPtrToValuePtr<void>(Context);
		TrackingModeProperty->GetUnderlyingProperty()->SetIntPropertyValue(
			TrackingModeValue,
			static_cast<int64>(TrackingMode));
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
	FString ContextPath;
	FString ActionPath;
	FString KeyName;
	FMonolithActionResult Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("context_path"), ContextPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("action_path"), ActionPath, Err)) return Err;
	if (!MonolithGAS::RequireStringParam(Params, TEXT("key"), KeyName, Err)) return Err;
	bool bSave = true;
	bool bAllowDuplicate = false;
	bool bDryRun = false;
	bool bPlayerMappable = false;

	FString Error;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("allow_duplicate"), bAllowDuplicate, Error)
		|| !MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("player_mappable"), bPlayerMappable, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const bool bHasPlayerMappable = Params.IsValid() && Params->HasField(TEXT("player_mappable"));
	const bool bHasPlayerMappableMetadata = Params.IsValid()
		&& (Params->HasField(TEXT("mapping_name"))
			|| Params->HasField(TEXT("display_name"))
			|| Params->HasField(TEXT("display_category"))
			|| Params->HasField(TEXT("supported_key_profile_ids")));

	FString MappingName;
	FString DisplayName;
	FString DisplayCategory;
	TArray<FString> SupportedKeyProfileIds;
	if (bHasPlayerMappable && bPlayerMappable)
	{
		if (!MonolithGAS::RequireStringParam(Params, TEXT("mapping_name"), MappingName, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("display_name"), DisplayName, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("display_category"), DisplayCategory, Err)) return Err;
		if (FName(*MappingName).IsNone())
		{
			return FMonolithActionResult::Error(
				TEXT("Invalid parameter: mapping_name must resolve to a non-None FName"));
		}
		if (!MonolithParamUtils::GetOptionalStringArrayParam(
			Params,
			TEXT("supported_key_profile_ids"),
			SupportedKeyProfileIds,
			Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		TArray<FString> NormalizedProfileIds;
		for (FString ProfileId : SupportedKeyProfileIds)
		{
			ProfileId.TrimStartAndEndInline();
			if (ProfileId.IsEmpty())
			{
				return FMonolithActionResult::Error(
					TEXT("Invalid parameter: supported_key_profile_ids entries must be non-empty strings"));
			}
			NormalizedProfileIds.AddUnique(ProfileId);
		}
		SupportedKeyProfileIds = MoveTemp(NormalizedProfileIds);
	}
	else if (bHasPlayerMappableMetadata)
	{
		return FMonolithActionResult::Error(
			TEXT("Player-mappable metadata is valid only when player_mappable=true."));
	}

	if (bHasPlayerMappable)
	{
		FProperty* BehaviorProperty = nullptr;
		FObjectPropertyBase* SettingsProperty = nullptr;
		if (!ResolvePlayerMappableProperties(BehaviorProperty, SettingsProperty, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	TArray<UClass*> ModifierClasses;
	TArray<UClass*> TriggerClasses;
	if (!ReadInputObjectClassArray(Params, TEXT("modifier_classes"), UInputModifier::StaticClass(), ModifierClasses, Error)
		|| !ReadInputObjectClassArray(Params, TEXT("trigger_classes"), UInputTrigger::StaticClass(), TriggerClasses, Error))
	{
		return FMonolithActionResult::Error(Error);
	}
	const bool bHasModifierClasses = Params.IsValid() && Params->HasField(TEXT("modifier_classes"));
	const bool bHasTriggerClasses = Params.IsValid() && Params->HasField(TEXT("trigger_classes"));

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

	const bool bHasAnySourceField = Params.IsValid()
		&& (Params->HasField(TEXT("source_context_path"))
			|| Params->HasField(TEXT("source_action_path"))
			|| Params->HasField(TEXT("source_key")));
	const FEnhancedActionKeyMapping* SourceMapping = nullptr;
	if (bHasAnySourceField)
	{
		FString SourceContextPath;
		FString SourceActionPath;
		FString SourceKeyName;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("source_context_path"), SourceContextPath, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("source_action_path"), SourceActionPath, Err)) return Err;
		if (!MonolithGAS::RequireStringParam(Params, TEXT("source_key"), SourceKeyName, Err)) return Err;

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
			return FMonolithActionResult::Error(Error);
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

	bool bCreated = ExistingIndex == INDEX_NONE;
	bool bUpdated = false;
	bool bPlayerMappableUpdated = false;
	int32 MappingIndex = ExistingIndex;
	if (ExistingIndex != INDEX_NONE)
	{
		const FEnhancedActionKeyMapping& ExistingMapping = Context->GetMappings()[ExistingIndex];
		bUpdated = !AreMappingsEquivalentForAuthoring(ExistingMapping, DesiredMapping);
		if (bHasPlayerMappable)
		{
			bool bEquivalent = false;
			if (!IsPlayerMappableStateEquivalent(
				ExistingMapping,
				bPlayerMappable,
				MappingName,
				DisplayName,
				DisplayCategory,
				SupportedKeyProfileIds,
				bEquivalent,
				Error))
			{
				return FMonolithActionResult::Error(Error);
			}
			bPlayerMappableUpdated = !bEquivalent;
			bUpdated |= bPlayerMappableUpdated;
		}
	}

	const bool bChanged = bCreated || bUpdated;
	if (bChanged && !bDryRun)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("Monolith", "AddInputMapping", "Add Input Mapping"));
		Context->Modify();
		if (bCreated)
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
		if (bHasPlayerMappable
			&& !ConfigurePlayerMappableState(
				TargetMapping,
				Context,
				bPlayerMappable,
				MappingName,
				DisplayName,
				DisplayCategory,
				SupportedKeyProfileIds,
				Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}
	else if (bCreated && bDryRun)
	{
		MappingIndex = Before;
	}

	bool bSaved = false;
	if (bChanged && !bDryRun && !SaveAssetIfRequested(Context, bSave, bSaved, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("context_path"), Context->GetPathName());
	Result->SetStringField(TEXT("action_path"), Action->GetPathName());
	Result->SetStringField(TEXT("key"), Key.ToString());
	Result->SetNumberField(TEXT("before_count"), Before);
	Result->SetNumberField(TEXT("after_count"), bDryRun ? Before + (bCreated ? 1 : 0) : Context->GetMappings().Num());
	Result->SetNumberField(TEXT("mapping_index"), MappingIndex);
	Result->SetBoolField(TEXT("created"), bCreated);
	Result->SetBoolField(TEXT("updated"), bUpdated);
	Result->SetBoolField(TEXT("player_mappable_updated"), bPlayerMappableUpdated);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("allow_duplicate"), bAllowDuplicate);
	Result->SetBoolField(TEXT("cloned_from_source"), SourceMapping != nullptr);
	Result->SetBoolField(TEXT("player_mappable_requested"), bHasPlayerMappable);
	if (bHasPlayerMappable)
	{
		Result->SetBoolField(TEXT("requested_player_mappable"), bPlayerMappable);
		if (bPlayerMappable)
		{
			Result->SetStringField(TEXT("requested_mapping_name"), MappingName);
			Result->SetStringField(TEXT("requested_display_name"), DisplayName);
			Result->SetStringField(TEXT("requested_display_category"), DisplayCategory);
			TArray<TSharedPtr<FJsonValue>> ProfileIdsJson;
			for (const FString& ProfileId : SupportedKeyProfileIds)
			{
				ProfileIdsJson.Add(MakeShared<FJsonValueString>(ProfileId));
			}
			Result->SetArrayField(TEXT("requested_supported_key_profile_ids"), ProfileIdsJson);
		}
	}
	Result->SetNumberField(TEXT("modifier_count"), DesiredMapping.Modifiers.Num());
	Result->SetNumberField(TEXT("trigger_count"), DesiredMapping.Triggers.Num());
	Result->SetBoolField(TEXT("saved"), bSaved);
	if (!bDryRun && Context->GetMappings().IsValidIndex(MappingIndex))
	{
		Result->SetObjectField(TEXT("mapping"), MappingToJson(Context->GetMappings()[MappingIndex], MappingIndex));
	}
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
	bool bSave = true;

	FString Error;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error))
	{
		return FMonolithActionResult::Error(Error);
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
	bool bFailOnUnbound = false;
	FString ParamError;
	if (!MonolithGAS::TryReadOptionalBoolParam(Params, TEXT("fail_on_unbound"), bFailOnUnbound, ParamError))
	{
		return FMonolithActionResult::Error(ParamError);
	}

	TArray<FString> ContextPaths = ReadContextPaths(Params);
	if (ContextPaths.Num() == 0)
	{
		TArray<FAssetData> Assets;
		FString Error;
		if (!GetAssetsByClass(UInputMappingContext::StaticClass(), Params, Assets, Error))
		{
			return FMonolithActionResult::Error(Error);
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
	int32 SharedKeyGroupCount = 0;
	int32 UnboundMappingCount = 0;

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

		TMap<FString, FMonolithGASInputSharedKeyGroup> SharedKeyCandidates;
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<TSharedPtr<FJsonValue>> UnboundMappings;
		TArray<TSharedPtr<FJsonValue>> SharedKeys;
		int32 ContextConflictCount = 0;
		int32 ContextMissingActionCount = 0;
		int32 ContextUnboundMappingCount = 0;
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
				ContextMissingActionCount++;
			}

			if (!Mapping.Key.IsValid())
			{
				TSharedPtr<FJsonObject> Unbound = MakeShared<FJsonObject>();
				Unbound->SetNumberField(TEXT("index"), Index);
				Unbound->SetStringField(TEXT("action"), ActionPath);
				Unbound->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
				UnboundMappings.Add(MakeShared<FJsonValueObject>(Unbound));
				UnboundMappingCount++;
				ContextUnboundMappingCount++;

				if (bFailOnUnbound)
				{
					TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
					Issue->SetStringField(TEXT("type"), TEXT("unbound_mapping"));
					Issue->SetNumberField(TEXT("index"), Index);
					Issue->SetStringField(TEXT("action"), ActionPath);
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				continue;
			}

			if (!Mapping.Action)
			{
				continue;
			}

			FMonolithGASInputSharedKeyGroup& Candidate = SharedKeyCandidates.FindOrAdd(KeyName);
			Candidate.ActionPaths.AddUnique(ActionPath);
			Candidate.Indices.Add(Index);
		}

		for (const FMonolithGASInputExactDuplicateGroup& Group : FindMonolithGASInputExactDuplicateGroups(Mappings))
		{
			TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
			Issue->SetStringField(TEXT("type"), TEXT("duplicate_mapping_conflict"));
			Issue->SetStringField(TEXT("action"), Group.ActionPath);
			Issue->SetStringField(TEXT("key"), Group.KeyName);
			Issue->SetNumberField(TEXT("mapping_count"), Group.Indices.Num());
			TArray<TSharedPtr<FJsonValue>> IndicesJson;
			for (const int32 Index : Group.Indices)
			{
				IndicesJson.Add(MakeShared<FJsonValueNumber>(Index));
			}
			Issue->SetArrayField(TEXT("indices"), IndicesJson);
			Issues.Add(MakeShared<FJsonValueObject>(Issue));
			ConflictCount++;
			ContextConflictCount++;
		}

		TArray<FString> SharedKeyNames;
		SharedKeyCandidates.GetKeys(SharedKeyNames);
		SharedKeyNames.Sort();
		for (const FString& SharedKeyName : SharedKeyNames)
		{
			FMonolithGASInputSharedKeyGroup& Group = SharedKeyCandidates.FindChecked(SharedKeyName);
			if (Group.ActionPaths.Num() > 1)
			{
				Group.ActionPaths.Sort();
				Group.Indices.Sort();
				TSharedPtr<FJsonObject> SharedKey = MakeShared<FJsonObject>();
				SharedKey->SetStringField(TEXT("key"), SharedKeyName);
				SharedKey->SetNumberField(TEXT("mapping_count"), Group.Indices.Num());
				TArray<TSharedPtr<FJsonValue>> ActionsJson;
				for (const FString& Action : Group.ActionPaths)
				{
					ActionsJson.Add(MakeShared<FJsonValueString>(Action));
				}
				SharedKey->SetArrayField(TEXT("actions"), ActionsJson);
				TArray<TSharedPtr<FJsonValue>> IndicesJson;
				for (const int32 Index : Group.Indices)
				{
					IndicesJson.Add(MakeShared<FJsonValueNumber>(Index));
				}
				SharedKey->SetArrayField(TEXT("indices"), IndicesJson);
				SharedKeys.Add(MakeShared<FJsonValueObject>(SharedKey));
				SharedKeyGroupCount++;
			}
		}

		ContextResult->SetBoolField(TEXT("valid"), Issues.Num() == 0);
		ContextResult->SetStringField(TEXT("asset_path"), Context->GetPathName());
		ContextResult->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		ContextResult->SetNumberField(TEXT("conflicts"), ContextConflictCount);
		ContextResult->SetNumberField(TEXT("missing_actions"), ContextMissingActionCount);
		ContextResult->SetNumberField(TEXT("shared_key_groups"), SharedKeys.Num());
		ContextResult->SetNumberField(TEXT("unbound_mappings"), ContextUnboundMappingCount);
		ContextResult->SetArrayField(TEXT("issues"), Issues);
		ContextResult->SetArrayField(TEXT("shared_keys"), SharedKeys);
		ContextResult->SetArrayField(TEXT("unbound_rows"), UnboundMappings);
		ContextResults.Add(MakeShared<FJsonValueObject>(ContextResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(
		TEXT("valid"),
		ConflictCount == 0
			&& MissingActionCount == 0
			&& ContextLoadFailureCount == 0
			&& (!bFailOnUnbound || UnboundMappingCount == 0));
	Result->SetNumberField(TEXT("contexts_checked"), ContextPaths.Num());
	Result->SetNumberField(TEXT("context_load_failures"), ContextLoadFailureCount);
	Result->SetNumberField(TEXT("conflicts"), ConflictCount);
	Result->SetNumberField(TEXT("duplicate_mapping_conflicts"), ConflictCount);
	Result->SetNumberField(TEXT("missing_actions"), MissingActionCount);
	Result->SetNumberField(TEXT("shared_key_groups"), SharedKeyGroupCount);
	Result->SetNumberField(TEXT("unbound_mappings"), UnboundMappingCount);
	Result->SetBoolField(TEXT("fail_on_unbound"), bFailOnUnbound);
	Result->SetArrayField(TEXT("contexts"), ContextResults);
	return FMonolithActionResult::Success(Result);
}
