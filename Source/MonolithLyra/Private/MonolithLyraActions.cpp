#include "MonolithLyraActions.h"

#include "MonolithParamSchema.h"
#include "MonolithBulkFillRegistry.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionReader.h"
#include "Reflection/MonolithDryRunGuard.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/AssetManager.h"
#include "Engine/Blueprint.h"
#include "Engine/DataAsset.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace MonolithLyra
{
	static constexpr int32 ErrInvalidParams = -32602;

	static const TCHAR* LyraExperienceDefinitionClassPath = TEXT("/Script/LyraGame.LyraExperienceDefinition");
	static const TCHAR* LyraExperienceActionSetClassPath = TEXT("/Script/LyraGame.LyraExperienceActionSet");
	static const TCHAR* LyraUserFacingExperienceClassPath = TEXT("/Script/LyraGame.LyraUserFacingExperienceDefinition");
	static const TCHAR* LyraWorldSettingsClassPath = TEXT("/Script/LyraGame.LyraWorldSettings");
	static const TCHAR* LyraGamePhaseAbilityClassPath = TEXT("/Script/LyraGame.LyraGamePhaseAbility");
	static const TCHAR* LyraGamePhaseSubsystemClassPath = TEXT("/Script/LyraGame.LyraGamePhaseSubsystem");
	static const TCHAR* LyraTeamCreationComponentClassPath = TEXT("/Script/LyraGame.LyraTeamCreationComponent");
	static const TCHAR* LyraInventoryItemDefinitionClassPath = TEXT("/Script/LyraGame.LyraInventoryItemDefinition");
	static const TCHAR* LyraInventoryItemFragmentClassPath = TEXT("/Script/LyraGame.LyraInventoryItemFragment");
	static const TCHAR* LyraEquipmentDefinitionClassPath = TEXT("/Script/LyraGame.LyraEquipmentDefinition");
	static const TCHAR* LyraWeaponInstanceClassPath = TEXT("/Script/LyraGame.LyraWeaponInstance");
	static const TCHAR* LyraPawnDataClassPath = TEXT("/Script/LyraGame.LyraPawnData");
	static const TCHAR* LyraControllerCharacterPartsClassPath = TEXT("/Script/LyraGame.LyraControllerComponent_CharacterParts");
	static const TCHAR* LyraPawnCharacterPartsClassPath = TEXT("/Script/LyraGame.LyraPawnComponent_CharacterParts");
	static const TCHAR* LyraCosmeticDeveloperSettingsClassPath = TEXT("/Script/LyraGame.LyraCosmeticDeveloperSettings");
	static const TCHAR* EngineActorClassPath = TEXT("/Script/Engine.Actor");
	static const TCHAR* EngineActorComponentClassPath = TEXT("/Script/Engine.ActorComponent");
	static const TCHAR* EnginePawnClassPath = TEXT("/Script/Engine.Pawn");
	static const TCHAR* GameFeatureActionClassPath = TEXT("/Script/GameFeatures.GameFeatureAction");
	static const TCHAR* GameFeatureActionAddComponentsClassPath = TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents");

	struct FResolvedLyraObject
	{
		UObject* Object = nullptr;
		UObject* AssetForSave = nullptr;
		UClass* ExpectedClass = nullptr;
		FString InputPath;
		FString ResolvedPath;
		FString SaveTargetPath;
		FString SourceKind;
	};

	struct FLyraMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = false;
		bool bStrict = true;
	};

	struct FComponentRemovalCandidate
	{
		UObject* Owner = nullptr;
		UObject* AssetForSave = nullptr;
		UObject* Action = nullptr;
		FArrayProperty* ComponentListProperty = nullptr;
		int32 ActionIndex = INDEX_NONE;
		int32 ComponentIndex = INDEX_NONE;
		FString OwnerPath;
		FString ActionPath;
		FString ActorClass;
		FString ComponentClass;
	};

	struct FComponentEntryMutationPlan
	{
		FArrayProperty* ComponentListProperty = nullptr;
		FStructProperty* EntryStructProperty = nullptr;
		FSoftClassProperty* ActorClassProperty = nullptr;
		FSoftClassProperty* ComponentClassProperty = nullptr;
		FBoolProperty* ClientProperty = nullptr;
		FBoolProperty* ServerProperty = nullptr;
		FNumericProperty* AdditionFlagsProperty = nullptr;
		int32 ComponentIndex = INDEX_NONE;
		int32 ComponentsBefore = 0;
		int32 ComponentsAfter = 0;
		bool bAdd = false;
		bool bUpdate = false;
	};

	struct FPhaseAbilitySummary
	{
		FString InputPath;
		FString AssetPath;
		FString ClassPath;
		FString CdoPath;
		FString SourceKind;
		FString PhaseTagString;
		bool bPhaseTagValid = false;
		bool bIsAbstract = false;
		bool bIsBlueprint = false;
	};

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> MakeCheck(const FString& Name, bool bOk, const FString& Severity, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("ok"), bOk);
		Check->SetStringField(TEXT("severity"), Severity);
		Check->SetStringField(TEXT("detail"), Detail);
		return Check;
	}

	static void AddCheck(
		TArray<TSharedPtr<FJsonValue>>& Checks,
		bool& bOverallOk,
		const FString& Name,
		bool bOk,
		const FString& Severity,
		const FString& Detail)
	{
		Checks.Add(MakeShared<FJsonValueObject>(MakeCheck(Name, bOk, Severity, Detail)));
		if (!bOk && Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase))
		{
			bOverallOk = false;
		}
	}

	static bool TryGetRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Missing required param '%s'"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		if (OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Param '%s' must not be empty"), FieldName);
			return false;
		}
		return true;
	}

	static bool TryReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	static bool TryReadIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double NumberValue = 0.0;
		if (!Params->TryGetNumberField(FieldName, NumberValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a number"), FieldName);
			return false;
		}
		InOutValue = static_cast<int32>(NumberValue);
		return true;
	}

	static bool TryReadMutationOptions(const TSharedPtr<FJsonObject>& Params, FLyraMutationOptions& OutOptions, FString& OutError)
	{
		if (!TryReadBoolParam(Params, TEXT("dry_run"), OutOptions.bDryRun, OutError)
			|| !TryReadBoolParam(Params, TEXT("confirm"), OutOptions.bConfirm, OutError)
			|| !TryReadBoolParam(Params, TEXT("save"), OutOptions.bSave, OutError)
			|| !TryReadBoolParam(Params, TEXT("strict"), OutOptions.bStrict, OutError))
		{
			return false;
		}

		if (!OutOptions.bDryRun && !OutOptions.bConfirm)
		{
			OutError = TEXT("Mutating Lyra actions require dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

	static bool TryGetOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetStringField(FieldName, OutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}

	static bool TryReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues, FString& OutError)
	{
		OutValues.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
				return false;
			}
			StringValue.TrimStartAndEndInline();
			if (!StringValue.IsEmpty())
			{
				OutValues.AddUnique(StringValue);
			}
		}
		return true;
	}

	static FString NormalizeObjectPath(FString AssetPath)
	{
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty() || AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? AssetPath : AssetPath + TEXT(".") + AssetName;
	}

	static FString NormalizeObjectPathForCompare(const FString& AssetPath)
	{
		const FString ObjectPath = NormalizeObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		return FSoftObjectPath(ObjectPath).GetAssetPathString().ToLower();
	}

	static UClass* LoadExpectedClass(const TCHAR* ExpectedClassPath)
	{
		return StaticLoadClass(UObject::StaticClass(), nullptr, ExpectedClassPath);
	}

	static UClass* LoadClassPathWithGeneratedFallback(const FString& InputPath)
	{
		const FString ObjectPath = NormalizeObjectPath(InputPath);
		UClass* Class = StaticLoadClass(UObject::StaticClass(), nullptr, *ObjectPath);
		if (!Class && !ObjectPath.EndsWith(TEXT("_C")))
		{
			Class = StaticLoadClass(UObject::StaticClass(), nullptr, *(ObjectPath + TEXT("_C")));
		}
		return Class;
	}

	static bool IsObjectCompatibleWithExpected(const UObject* Object, const UClass* ExpectedClass)
	{
		if (!Object || !ExpectedClass)
		{
			return false;
		}
		if (const UClass* ClassObject = Cast<UClass>(Object))
		{
			return ClassObject->IsChildOf(ExpectedClass);
		}
		return Object->IsA(ExpectedClass);
	}

	static UObject* ObjectForLoadedValue(UObject* Loaded, UObject*& OutAssetForSave, FString& InOutSourceKind)
	{
		OutAssetForSave = Loaded;
		if (UClass* LoadedClass = Cast<UClass>(Loaded))
		{
			InOutSourceKind = TEXT("class_default_object");
			if (UObject* ClassGeneratedBy = LoadedClass->ClassGeneratedBy)
			{
				OutAssetForSave = ClassGeneratedBy;
			}
			return LoadedClass->GetDefaultObject();
		}

		if (UBlueprint* Blueprint = Cast<UBlueprint>(Loaded))
		{
			OutAssetForSave = Blueprint;
			if (Blueprint->GeneratedClass)
			{
				InOutSourceKind = TEXT("blueprint_generated_class_default_object");
				return Blueprint->GeneratedClass->GetDefaultObject();
			}
		}

		InOutSourceKind = TEXT("asset_object");
		return Loaded;
	}

	static bool TryResolveLyraObject(
		const FString& InputPath,
		const TCHAR* ExpectedClassPath,
		FResolvedLyraObject& OutResolved,
		FString& OutError)
	{
		OutResolved = FResolvedLyraObject();
		OutResolved.InputPath = InputPath;
		OutResolved.ExpectedClass = LoadExpectedClass(ExpectedClassPath);
		if (!OutResolved.ExpectedClass)
		{
			OutError = FString::Printf(TEXT("Lyra class is unavailable: %s"), ExpectedClassPath);
			return false;
		}

		const FString ObjectPath = NormalizeObjectPath(InputPath);
		UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (Loaded)
		{
			UObject* Candidate = ObjectForLoadedValue(Loaded, OutResolved.AssetForSave, OutResolved.SourceKind);
			if (!IsObjectCompatibleWithExpected(Candidate, OutResolved.ExpectedClass))
			{
				OutError = FString::Printf(
					TEXT("Resolved '%s' as '%s', but it is not a child/object of '%s'"),
					*ObjectPath,
					Candidate ? *Candidate->GetClass()->GetPathName() : TEXT("<null>"),
					*OutResolved.ExpectedClass->GetPathName());
				return false;
			}
			OutResolved.Object = Candidate;
			OutResolved.ResolvedPath = Candidate ? Candidate->GetPathName() : ObjectPath;
			OutResolved.SaveTargetPath = OutResolved.AssetForSave ? OutResolved.AssetForSave->GetPathName() : FString();
			return true;
		}

		if (UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			if (!LoadedClass->IsChildOf(OutResolved.ExpectedClass))
			{
				OutError = FString::Printf(
					TEXT("Resolved '%s' as class '%s', but it is not a child of '%s'"),
					*ObjectPath,
					*LoadedClass->GetPathName(),
					*OutResolved.ExpectedClass->GetPathName());
				return false;
			}
			OutResolved.Object = LoadedClass->GetDefaultObject();
			OutResolved.AssetForSave = LoadedClass->ClassGeneratedBy.Get() ? LoadedClass->ClassGeneratedBy.Get() : OutResolved.Object;
			OutResolved.ResolvedPath = OutResolved.Object ? OutResolved.Object->GetPathName() : LoadedClass->GetPathName();
			OutResolved.SaveTargetPath = OutResolved.AssetForSave ? OutResolved.AssetForSave->GetPathName() : FString();
			OutResolved.SourceKind = LoadedClass->ClassGeneratedBy.Get() ? TEXT("class_default_object") : TEXT("native_class_default_object");
			return OutResolved.Object != nullptr;
		}

		const FString ClassPath = ObjectPath.EndsWith(TEXT("_C")) ? ObjectPath : ObjectPath + TEXT("_C");
		if (!ClassPath.Equals(ObjectPath, ESearchCase::IgnoreCase))
		{
			UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
			if (LoadedClass)
			{
				if (!LoadedClass->IsChildOf(OutResolved.ExpectedClass))
				{
					OutError = FString::Printf(
						TEXT("Resolved '%s' as class '%s', but it is not a child of '%s'"),
						*ClassPath,
						*LoadedClass->GetPathName(),
						*OutResolved.ExpectedClass->GetPathName());
					return false;
				}
				OutResolved.Object = LoadedClass->GetDefaultObject();
				OutResolved.AssetForSave = LoadedClass->ClassGeneratedBy.Get() ? LoadedClass->ClassGeneratedBy.Get() : OutResolved.Object;
				OutResolved.ResolvedPath = OutResolved.Object ? OutResolved.Object->GetPathName() : LoadedClass->GetPathName();
				OutResolved.SaveTargetPath = OutResolved.AssetForSave ? OutResolved.AssetForSave->GetPathName() : FString();
				OutResolved.SourceKind = TEXT("class_default_object");
				return OutResolved.Object != nullptr;
			}
		}

		OutError = FString::Printf(TEXT("Could not load Lyra asset or generated class from '%s'"), *InputPath);
		return false;
	}

	static TSharedPtr<FJsonValue> ReadPropertyValue(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return MakeShared<FJsonValueNull>();
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return MakeShared<FJsonValueNull>();
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		return FMonolithReflectionReader::PropertyToJsonValue(Property, ValuePtr, Object);
	}

	static FString ExportPropertyText(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return FString();
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return FString();
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		FString Value;
		Property->ExportTextItem_Direct(Value, ValuePtr, nullptr, Object, PPF_None);
		return Value;
	}

	static bool TryGetGameplayTagProperty(UObject* Object, const TCHAR* PropertyName, FGameplayTag& OutValue)
	{
		if (!Object)
		{
			return false;
		}
		if (FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), PropertyName))
		{
			if (Property->Struct == FGameplayTag::StaticStruct())
			{
				OutValue = *Property->ContainerPtrToValuePtr<FGameplayTag>(Object);
				return true;
			}
		}
		return false;
	}

	static FGameplayTag RequestTagNoError(const FString& TagName)
	{
		return TagName.IsEmpty() ? FGameplayTag() : FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
	}

	static TSharedPtr<FJsonObject> GameplayTagToJson(const FGameplayTag& Tag)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("is_valid"), Tag.IsValid());
		Result->SetStringField(TEXT("text"), Tag.ToString());
		if (!Tag.IsValid())
		{
			Result->SetStringField(TEXT("direct_parent"), FString());
			Result->SetArrayField(TEXT("sources"), TArray<TSharedPtr<FJsonValue>>());
			return Result;
		}

		UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
		const FGameplayTag Parent = TagsManager.RequestGameplayTagDirectParent(Tag);
		Result->SetStringField(TEXT("direct_parent"), Parent.ToString());

		FString Comment;
		TArray<FName> Sources;
		bool bIsExplicit = false;
		bool bIsRestricted = false;
		bool bAllowNonRestrictedChildren = false;
		if (TagsManager.GetTagEditorData(Tag.GetTagName(), Comment, Sources, bIsExplicit, bIsRestricted, bAllowNonRestrictedChildren))
		{
			Result->SetStringField(TEXT("comment"), Comment);
			Result->SetBoolField(TEXT("explicit"), bIsExplicit);
			Result->SetBoolField(TEXT("restricted"), bIsRestricted);
			Result->SetBoolField(TEXT("allow_non_restricted_children"), bAllowNonRestrictedChildren);

			TArray<FString> SourceStrings;
			SourceStrings.Reserve(Sources.Num());
			for (const FName Source : Sources)
			{
				SourceStrings.Add(Source.ToString());
			}
			Result->SetArrayField(TEXT("sources"), StringArrayToJson(SourceStrings));
		}
		else
		{
			Result->SetArrayField(TEXT("sources"), TArray<TSharedPtr<FJsonValue>>());
		}
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> GameplayTagsToJsonArray(const FGameplayTagContainer& Tags, int32 MaxTags, bool& bOutTruncated)
	{
		bOutTruncated = false;
		TArray<FGameplayTag> SortedTags;
		Tags.GetGameplayTagArray(SortedTags);
		SortedTags.Sort([](const FGameplayTag& Left, const FGameplayTag& Right)
		{
			return Left.ToString() < Right.ToString();
		});

		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < SortedTags.Num(); ++Index)
		{
			if (Index >= MaxTags)
			{
				bOutTruncated = true;
				break;
			}
			Rows.Add(MakeShared<FJsonValueObject>(GameplayTagToJson(SortedTags[Index])));
		}
		return Rows;
	}

	static bool TryGetBoolProperty(UObject* Object, const TCHAR* PropertyName, bool& OutValue)
	{
		if (Object)
		{
			if (FBoolProperty* Property = FindFProperty<FBoolProperty>(Object->GetClass(), PropertyName))
			{
				OutValue = Property->GetPropertyValue_InContainer(Object);
				return true;
			}
		}
		return false;
	}

	static bool TryGetIntProperty(UObject* Object, const TCHAR* PropertyName, int32& OutValue)
	{
		if (Object)
		{
			if (FNumericProperty* Property = FindFProperty<FNumericProperty>(Object->GetClass(), PropertyName))
			{
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
				OutValue = static_cast<int32>(Property->GetSignedIntPropertyValue(ValuePtr));
				return true;
			}
		}
		return false;
	}

	static bool TryGetPrimaryAssetIdProperty(UObject* Object, const TCHAR* PropertyName, FPrimaryAssetId& OutValue)
	{
		if (!Object)
		{
			return false;
		}
		if (FStructProperty* Property = FindFProperty<FStructProperty>(Object->GetClass(), PropertyName))
		{
			if (Property->Struct && Property->Struct->GetFName() == FName(TEXT("PrimaryAssetId")))
			{
				OutValue = *Property->ContainerPtrToValuePtr<FPrimaryAssetId>(Object);
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> PrimaryAssetIdToJson(const FPrimaryAssetId& AssetId)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("is_valid"), AssetId.IsValid());
		Result->SetStringField(TEXT("text"), AssetId.ToString());
		Result->SetStringField(TEXT("type"), AssetId.PrimaryAssetType.ToString());
		Result->SetStringField(TEXT("name"), AssetId.PrimaryAssetName.ToString());

		FString ResolvedPath;
		if (AssetId.IsValid() && UAssetManager::IsInitialized())
		{
			ResolvedPath = UAssetManager::Get().GetPrimaryAssetPath(AssetId).ToString();
		}
		Result->SetStringField(TEXT("resolved_object_path"), ResolvedPath);
		return Result;
	}

	static FString MapPackageToObjectPath(const FString& PackagePath)
	{
		if (PackagePath.Contains(TEXT(".")))
		{
			return PackagePath;
		}
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			return PackagePath;
		}
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	static bool ResolveMapObjectPath(const FString& InputPath, FString& OutObjectPath, FString& OutSourceKind, FString& OutError)
	{
		OutObjectPath.Reset();
		OutSourceKind.Reset();

		if (InputPath.IsEmpty())
		{
			OutError = TEXT("map_path must not be empty");
			return false;
		}

		const FPrimaryAssetId AssetId = FPrimaryAssetId::FromString(InputPath);
		if (AssetId.IsValid())
		{
			if (!AssetId.PrimaryAssetType.ToString().Equals(TEXT("Map"), ESearchCase::IgnoreCase))
			{
				OutError = FString::Printf(TEXT("map_path PrimaryAssetId must have type Map, got '%s'"), *AssetId.PrimaryAssetType.ToString());
				return false;
			}
			if (UAssetManager::IsInitialized())
			{
				const FSoftObjectPath AssetPath = UAssetManager::Get().GetPrimaryAssetPath(AssetId);
				if (AssetPath.IsValid())
				{
					OutObjectPath = AssetPath.ToString();
					OutSourceKind = TEXT("primary_asset_id");
					return true;
				}
			}

			const FString PackageName = AssetId.PrimaryAssetName.ToString();
			OutObjectPath = MapPackageToObjectPath(PackageName);
			OutSourceKind = TEXT("primary_asset_id_name_fallback");
			return true;
		}

		OutObjectPath = MapPackageToObjectPath(InputPath);
		OutSourceKind = InputPath.Contains(TEXT(".")) ? TEXT("object_path") : TEXT("package_path");
		return true;
	}

	static bool TryGetSoftClassPropertyPath(UObject* Object, const TCHAR* PropertyName, FSoftObjectPath& OutPath)
	{
		OutPath = FSoftObjectPath();
		if (!Object)
		{
			return false;
		}
		if (FSoftClassProperty* Property = FindFProperty<FSoftClassProperty>(Object->GetClass(), PropertyName))
		{
			const void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
			const FSoftObjectPtr* SoftPtr = static_cast<const FSoftObjectPtr*>(PropertyValue);
			OutPath = SoftPtr ? SoftPtr->ToSoftObjectPath() : FSoftObjectPath();
			return true;
		}
		return false;
	}

	static FPrimaryAssetId ResolveExperienceIdFromSoftClassPath(const FSoftObjectPath& ExperienceClassPath, FString& OutMethod, FString& OutLoadedClassPath)
	{
		OutMethod.Reset();
		OutLoadedClassPath.Reset();
		FPrimaryAssetId Result;
		if (!ExperienceClassPath.IsValid())
		{
			return Result;
		}

		if (UAssetManager::IsInitialized())
		{
			Result = UAssetManager::Get().GetPrimaryAssetIdForPath(ExperienceClassPath);
			if (Result.IsValid())
			{
				OutMethod = TEXT("asset_manager_soft_class_path");
				return Result;
			}

			if (ExperienceClassPath.GetAssetName().EndsWith(TEXT("_C")))
			{
				const FString ExperiencePackageName = ExperienceClassPath.GetLongPackageName();
				const FString ExperienceAssetName = FPackageName::GetLongPackageAssetName(ExperiencePackageName);
				Result = UAssetManager::Get().GetPrimaryAssetIdForPath(FSoftObjectPath(ExperiencePackageName + TEXT(".") + ExperienceAssetName));
				if (Result.IsValid())
				{
					OutMethod = TEXT("asset_manager_blueprint_asset_path");
					return Result;
				}
			}
		}

		if (UClass* ExperienceClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ExperienceClassPath.ToString()))
		{
			OutLoadedClassPath = ExperienceClass->GetPathName();
			if (const UPrimaryDataAsset* PrimaryDataAsset = Cast<UPrimaryDataAsset>(ExperienceClass->GetDefaultObject()))
			{
				Result = PrimaryDataAsset->GetPrimaryAssetId();
				if (Result.IsValid())
				{
					OutMethod = TEXT("loaded_class_default_object");
					return Result;
				}
			}
		}

		return Result;
	}

	static bool TryParseExpectedExperienceId(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& bOutHasExpected, FPrimaryAssetId& OutExpectedId, FString& OutError)
	{
		bOutHasExpected = false;
		OutExpectedId = FPrimaryAssetId();

		FString ExpectedText;
		if (!TryGetOptionalStringParam(Params, FieldName, ExpectedText, OutError))
		{
			return false;
		}
		if (ExpectedText.IsEmpty())
		{
			return true;
		}

		OutExpectedId = FPrimaryAssetId::FromString(ExpectedText);
		if (!OutExpectedId.IsValid())
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a valid PrimaryAssetId in Type:Name form"), FieldName);
			return false;
		}
		if (!OutExpectedId.PrimaryAssetType.ToString().Equals(TEXT("LyraExperienceDefinition"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Param '%s' must have type LyraExperienceDefinition"), FieldName);
			return false;
		}
		bOutHasExpected = true;
		return true;
	}

	static TSharedPtr<FJsonObject> BuildMapDefaultExperienceContract(
		const FString& MapInputPath,
		const FPrimaryAssetId& ExpectedExperienceId,
		bool bHasExpectedExperience,
		bool bRequireDefaultExperience,
		bool bRequireLyraWorldSettings,
		bool bRequireMatchingExperience,
		TArray<TSharedPtr<FJsonValue>>& Checks,
		TArray<TSharedPtr<FJsonValue>>& Warnings,
		bool& bOk)
	{
		TSharedPtr<FJsonObject> Contract = MakeShared<FJsonObject>();
		Contract->SetStringField(TEXT("map_input"), MapInputPath);

		FString MapObjectPath;
		FString MapSourceKind;
		FString ResolveError;
		const bool bResolvedMapPath = ResolveMapObjectPath(MapInputPath, MapObjectPath, MapSourceKind, ResolveError);
		Contract->SetStringField(TEXT("map_object_path"), MapObjectPath);
		Contract->SetStringField(TEXT("map_source_kind"), MapSourceKind);
		AddCheck(Checks, bOk, TEXT("map_path_resolved"), bResolvedMapPath, TEXT("error"), bResolvedMapPath ? MapObjectPath : ResolveError);
		if (!bResolvedMapPath)
		{
			return Contract;
		}

		UWorld* World = LoadObject<UWorld>(nullptr, *MapObjectPath);
		Contract->SetBoolField(TEXT("map_loaded"), World != nullptr);
		AddCheck(Checks, bOk, TEXT("map_loadable"), World != nullptr, TEXT("error"), World ? World->GetPathName() : FString::Printf(TEXT("Could not load UWorld '%s'"), *MapObjectPath));
		if (!World)
		{
			return Contract;
		}

		AWorldSettings* WorldSettings = World->GetWorldSettings();
		const FString WorldSettingsClassPath = WorldSettings && WorldSettings->GetClass() ? WorldSettings->GetClass()->GetPathName() : FString();
		Contract->SetStringField(TEXT("world_settings_path"), WorldSettings ? WorldSettings->GetPathName() : FString());
		Contract->SetStringField(TEXT("world_settings_class_path"), WorldSettingsClassPath);
		AddCheck(Checks, bOk, TEXT("world_settings_present"), WorldSettings != nullptr, TEXT("error"), WorldSettings ? WorldSettings->GetPathName() : TEXT("World has no WorldSettings"));

		UClass* LyraWorldSettingsClass = StaticLoadClass(UObject::StaticClass(), nullptr, LyraWorldSettingsClassPath);
		const bool bIsLyraWorldSettings = WorldSettings && LyraWorldSettingsClass && WorldSettings->IsA(LyraWorldSettingsClass);
		Contract->SetBoolField(TEXT("world_settings_is_lyra"), bIsLyraWorldSettings);
		AddCheck(
			Checks,
			bOk,
			TEXT("world_settings_is_lyra"),
			!bRequireLyraWorldSettings || bIsLyraWorldSettings,
			bRequireLyraWorldSettings ? TEXT("error") : TEXT("warning"),
			WorldSettingsClassPath.IsEmpty() ? TEXT("WorldSettings class unavailable") : WorldSettingsClassPath);

		FSoftObjectPath DefaultExperiencePath;
		const bool bPropertyFound = TryGetSoftClassPropertyPath(WorldSettings, TEXT("DefaultGameplayExperience"), DefaultExperiencePath);
		TSharedPtr<FJsonObject> DefaultExperience = MakeShared<FJsonObject>();
		DefaultExperience->SetBoolField(TEXT("property_found"), bPropertyFound);
		DefaultExperience->SetStringField(TEXT("soft_class_path"), DefaultExperiencePath.ToString());
		DefaultExperience->SetBoolField(TEXT("is_set"), DefaultExperiencePath.IsValid());

		FString ResolveMethod;
		FString LoadedClassPath;
		const FPrimaryAssetId DefaultExperienceId = ResolveExperienceIdFromSoftClassPath(DefaultExperiencePath, ResolveMethod, LoadedClassPath);
		DefaultExperience->SetObjectField(TEXT("primary_asset_id"), PrimaryAssetIdToJson(DefaultExperienceId));
		DefaultExperience->SetStringField(TEXT("resolve_method"), ResolveMethod);
		DefaultExperience->SetStringField(TEXT("loaded_class_path"), LoadedClassPath);
		if (bHasExpectedExperience)
		{
			DefaultExperience->SetObjectField(TEXT("expected_experience_id"), PrimaryAssetIdToJson(ExpectedExperienceId));
		}
		Contract->SetObjectField(TEXT("default_gameplay_experience"), DefaultExperience);

		const bool bRequireDefaultExperienceProperty = bRequireDefaultExperience || bRequireLyraWorldSettings;
		AddCheck(
			Checks,
			bOk,
			TEXT("default_gameplay_experience_property"),
			!bRequireDefaultExperienceProperty || bPropertyFound,
			bRequireDefaultExperienceProperty ? TEXT("error") : TEXT("warning"),
			bPropertyFound ? TEXT("DefaultGameplayExperience") : TEXT("WorldSettings has no DefaultGameplayExperience soft class property"));
		AddCheck(
			Checks,
			bOk,
			TEXT("default_gameplay_experience_set"),
			!bRequireDefaultExperience || DefaultExperiencePath.IsValid(),
			bRequireDefaultExperience ? TEXT("error") : TEXT("warning"),
			DefaultExperiencePath.ToString());
		AddCheck(
			Checks,
			bOk,
			TEXT("default_gameplay_experience_resolves"),
			!DefaultExperiencePath.IsValid() || DefaultExperienceId.IsValid(),
			TEXT("error"),
			DefaultExperienceId.IsValid() ? DefaultExperienceId.ToString() : TEXT("DefaultGameplayExperience did not resolve to a LyraExperienceDefinition primary asset id"));

		if (bHasExpectedExperience)
		{
			const bool bMatches = DefaultExperienceId.IsValid() && DefaultExperienceId == ExpectedExperienceId;
			AddCheck(
				Checks,
				bOk,
				TEXT("default_gameplay_experience_matches_expected"),
				!bRequireMatchingExperience || bMatches,
				bRequireMatchingExperience ? TEXT("error") : TEXT("warning"),
				FString::Printf(TEXT("actual=%s expected=%s"), *DefaultExperienceId.ToString(), *ExpectedExperienceId.ToString()));
			if (!bRequireMatchingExperience && !bMatches)
			{
				Warnings.Add(MakeShared<FJsonValueString>(TEXT("Map DefaultGameplayExperience differs from the expected LyraExperienceDefinition; this may be valid when the hosting request passes an explicit Experience URL option.")));
			}
		}

		return Contract;
	}

	static FString GetObjectPropertyPath(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return FString();
		}
		if (FObjectProperty* Property = FindFProperty<FObjectProperty>(Object->GetClass(), PropertyName))
		{
			UObject* Value = Property->GetObjectPropertyValue_InContainer(Object);
			return Value ? Value->GetPathName() : FString();
		}
		return FString();
	}

	static TArray<FString> GetStringArrayProperty(UObject* Object, const TCHAR* PropertyName)
	{
		TArray<FString> Values;
		if (!Object)
		{
			return Values;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Object->GetClass(), PropertyName);
		if (!ArrayProperty)
		{
			return Values;
		}
		FStrProperty* InnerString = CastField<FStrProperty>(ArrayProperty->Inner);
		if (!InnerString)
		{
			return Values;
		}
		const void* ValuePtr = ArrayProperty->ContainerPtrToValuePtr<void>(Object);
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const FString Value = InnerString->GetPropertyValue(Helper.GetRawPtr(Index));
			if (!Value.IsEmpty())
			{
				Values.Add(Value);
			}
		}
		return Values;
	}

	static TArray<UObject*> GetObjectArrayProperty(UObject* Object, const TCHAR* PropertyName)
	{
		TArray<UObject*> Values;
		if (!Object)
		{
			return Values;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Object->GetClass(), PropertyName);
		if (!ArrayProperty)
		{
			return Values;
		}
		FObjectPropertyBase* InnerObject = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
		if (!InnerObject)
		{
			return Values;
		}
		const void* ValuePtr = ArrayProperty->ContainerPtrToValuePtr<void>(Object);
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			Values.Add(InnerObject->GetObjectPropertyValue(Helper.GetRawPtr(Index)));
		}
		return Values;
	}

	static bool IsConcreteClass(const UClass* Class)
	{
		return Class && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated);
	}

	static int32 GetArrayPropertyCount(UObject* Object, const TCHAR* PropertyName);
	static UObject* GetObjectPropertyValue(UObject* Object, const TCHAR* PropertyName);
	static UClass* GetClassPropertyValue(UObject* Object, const TCHAR* PropertyName);

	static TSharedPtr<FJsonObject> ObjectRefToJson(UObject* Object)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("is_valid"), Object != nullptr);
		Result->SetStringField(TEXT("object_path"), Object ? Object->GetPathName() : FString());
		Result->SetStringField(TEXT("class_path"), Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
		if (const UPrimaryDataAsset* PrimaryDataAsset = Cast<UPrimaryDataAsset>(Object))
		{
			Result->SetStringField(TEXT("primary_asset_id"), PrimaryDataAsset->GetPrimaryAssetId().ToString());
		}
		return Result;
	}

	static FString ObjectArrayPathList(UObject* Object, const TCHAR* PropertyName)
	{
		TArray<FString> Paths;
		for (UObject* Value : GetObjectArrayProperty(Object, PropertyName))
		{
			Paths.Add(Value ? Value->GetPathName() : TEXT("<null>"));
		}
		return FString::Join(Paths, TEXT(", "));
	}

	static int32 CountNullObjectArrayEntries(UObject* Object, const TCHAR* PropertyName)
	{
		int32 NullCount = 0;
		for (UObject* Value : GetObjectArrayProperty(Object, PropertyName))
		{
			if (!Value)
			{
				++NullCount;
			}
		}
		return NullCount;
	}

	static void AddPawnDataContractChecks(
		TArray<TSharedPtr<FJsonValue>>& Checks,
		bool& bOverallOk,
		UObject* PawnData,
		bool bRequirePawnClass,
		bool bRequireAbilitySets,
		bool bRequireInputConfig,
		bool bRequireDefaultCameraMode,
		const FString& CheckPrefix)
	{
		UClass* PawnClass = GetClassPropertyValue(PawnData, TEXT("PawnClass"));
		UClass* PawnBase = LoadExpectedClass(EnginePawnClassPath);
		const FString Prefix = CheckPrefix.IsEmpty() ? FString() : CheckPrefix + TEXT(".");

		AddCheck(Checks, bOverallOk, Prefix + TEXT("pawn_class_set"), PawnClass != nullptr || !bRequirePawnClass, bRequirePawnClass ? TEXT("error") : TEXT("warning"), PawnClass ? PawnClass->GetPathName() : TEXT("PawnClass is not set"));
		AddCheck(Checks, bOverallOk, Prefix + TEXT("pawn_class_is_pawn"), PawnClass && PawnBase && PawnClass->IsChildOf(PawnBase), bRequirePawnClass ? TEXT("error") : TEXT("warning"), PawnClass ? PawnClass->GetPathName() : TEXT("PawnClass is not set"));
		AddCheck(Checks, bOverallOk, Prefix + TEXT("pawn_class_concrete"), IsConcreteClass(PawnClass), bRequirePawnClass ? TEXT("error") : TEXT("warning"), PawnClass ? PawnClass->GetPathName() : TEXT("PawnClass is not set"));

		const int32 AbilitySetCount = GetArrayPropertyCount(PawnData, TEXT("AbilitySets"));
		const int32 NullAbilitySetCount = CountNullObjectArrayEntries(PawnData, TEXT("AbilitySets"));
		AddCheck(Checks, bOverallOk, Prefix + TEXT("ability_sets_present"), AbilitySetCount > 0 || !bRequireAbilitySets, bRequireAbilitySets ? TEXT("error") : TEXT("info"), FString::Printf(TEXT("%d AbilitySets"), AbilitySetCount));
		AddCheck(Checks, bOverallOk, Prefix + TEXT("ability_sets_non_null"), NullAbilitySetCount == 0, TEXT("error"), NullAbilitySetCount == 0 ? ObjectArrayPathList(PawnData, TEXT("AbilitySets")) : FString::Printf(TEXT("%d null AbilitySets entries"), NullAbilitySetCount));

		const FString InputConfigPath = GetObjectPropertyPath(PawnData, TEXT("InputConfig"));
		AddCheck(Checks, bOverallOk, Prefix + TEXT("input_config_set"), GetObjectPropertyValue(PawnData, TEXT("InputConfig")) != nullptr || !bRequireInputConfig, bRequireInputConfig ? TEXT("error") : TEXT("info"), InputConfigPath.IsEmpty() ? TEXT("InputConfig is not set") : InputConfigPath);
		AddCheck(Checks, bOverallOk, Prefix + TEXT("default_camera_mode_set"), GetClassPropertyValue(PawnData, TEXT("DefaultCameraMode")) != nullptr || !bRequireDefaultCameraMode, bRequireDefaultCameraMode ? TEXT("error") : TEXT("info"), GetClassPropertyValue(PawnData, TEXT("DefaultCameraMode")) ? GetClassPropertyValue(PawnData, TEXT("DefaultCameraMode"))->GetPathName() : TEXT("DefaultCameraMode is not set"));
	}

	static TSharedPtr<FJsonObject> BuildGameFeaturePluginSummary(const FString& GameFeatureName)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), GameFeatureName);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(GameFeatureName);
		Row->SetBoolField(TEXT("found"), Plugin.IsValid());
		if (Plugin.IsValid())
		{
			Row->SetBoolField(TEXT("enabled"), Plugin->IsEnabled());
			Row->SetBoolField(TEXT("can_contain_content"), Plugin->CanContainContent());
			Row->SetStringField(TEXT("base_dir"), Plugin->GetBaseDir());
			Row->SetStringField(TEXT("version_name"), Plugin->GetDescriptor().VersionName);
		}
		return Row;
	}

	static void AddActionSetContractChecks(
		TArray<TSharedPtr<FJsonValue>>& Checks,
		bool& bOverallOk,
		UObject* ActionSet,
		int32 ActionSetIndex,
		bool bRequireActions,
		bool bDisallowNullActions,
		bool bValidateActionClasses,
		bool bRequireGameFeatures)
	{
		const FString Prefix = FString::Printf(TEXT("action_set[%d]"), ActionSetIndex);
		if (!ActionSet)
		{
			AddCheck(Checks, bOverallOk, Prefix + TEXT(".resolved"), false, TEXT("error"), TEXT("ActionSet entry is null"));
			return;
		}

		const TArray<UObject*> Actions = GetObjectArrayProperty(ActionSet, TEXT("Actions"));
		AddCheck(Checks, bOverallOk, Prefix + TEXT(".actions_present"), Actions.Num() > 0 || !bRequireActions, bRequireActions ? TEXT("error") : TEXT("info"), FString::Printf(TEXT("%d Actions"), Actions.Num()));

		int32 NullActionCount = 0;
		UClass* GameFeatureActionBase = bValidateActionClasses ? LoadExpectedClass(GameFeatureActionClassPath) : nullptr;
		for (int32 ActionIndex = 0; ActionIndex < Actions.Num(); ++ActionIndex)
		{
			UObject* Action = Actions[ActionIndex];
			if (!Action)
			{
				++NullActionCount;
				if (bDisallowNullActions)
				{
					AddCheck(Checks, bOverallOk, FString::Printf(TEXT("%s.action[%d].resolved"), *Prefix, ActionIndex), false, TEXT("error"), TEXT("Action entry is null"));
				}
				continue;
			}

			if (bValidateActionClasses)
			{
				const UClass* ActionClass = Action->GetClass();
				const bool bChildOfGameFeatureAction = ActionClass && GameFeatureActionBase && ActionClass->IsChildOf(GameFeatureActionBase);
				AddCheck(
					Checks,
					bOverallOk,
					FString::Printf(TEXT("%s.action[%d].class_is_game_feature_action"), *Prefix, ActionIndex),
					bChildOfGameFeatureAction,
					TEXT("error"),
					ActionClass ? ActionClass->GetPathName() : TEXT("Action class is missing"));
				AddCheck(
					Checks,
					bOverallOk,
					FString::Printf(TEXT("%s.action[%d].class_concrete"), *Prefix, ActionIndex),
					IsConcreteClass(ActionClass),
					TEXT("error"),
					ActionClass ? ActionClass->GetPathName() : TEXT("Action class is missing"));
			}
		}

		AddCheck(
			Checks,
			bOverallOk,
			Prefix + TEXT(".actions_non_null"),
			NullActionCount == 0 || !bDisallowNullActions,
			bDisallowNullActions ? TEXT("error") : TEXT("warning"),
			NullActionCount == 0 ? TEXT("All Actions entries resolve") : FString::Printf(TEXT("%d null Actions entries"), NullActionCount));

		const TArray<FString> GameFeatures = GetStringArrayProperty(ActionSet, TEXT("GameFeaturesToEnable"));
		AddCheck(
			Checks,
			bOverallOk,
			Prefix + TEXT(".game_features_present"),
			GameFeatures.Num() > 0 || !bRequireGameFeatures,
			bRequireGameFeatures ? TEXT("error") : TEXT("info"),
			FString::Printf(TEXT("%d GameFeaturesToEnable entries"), GameFeatures.Num()));
	}


	static int32 GetArrayPropertyCount(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return 0;
		}
		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Object->GetClass(), PropertyName);
		if (!ArrayProperty)
		{
			return 0;
		}
		const void* ValuePtr = ArrayProperty->ContainerPtrToValuePtr<void>(Object);
		FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
		return Helper.Num();
	}

	static int32 GetMapPropertyCount(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return 0;
		}
		FMapProperty* MapProperty = FindFProperty<FMapProperty>(Object->GetClass(), PropertyName);
		if (!MapProperty)
		{
			return 0;
		}
		const void* ValuePtr = MapProperty->ContainerPtrToValuePtr<void>(Object);
		FScriptMapHelper Helper(MapProperty, ValuePtr);
		return Helper.Num();
	}

	static UObject* GetObjectPropertyValue(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return nullptr;
		}
		if (FObjectPropertyBase* ObjectProperty = FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName))
		{
			return ObjectProperty->GetObjectPropertyValue_InContainer(Object);
		}
		return nullptr;
	}

	static UClass* GetClassPropertyValue(UObject* Object, const TCHAR* PropertyName)
	{
		if (!Object)
		{
			return nullptr;
		}
		if (FClassProperty* ClassProperty = FindFProperty<FClassProperty>(Object->GetClass(), PropertyName))
		{
			return Cast<UClass>(ClassProperty->GetObjectPropertyValue_InContainer(Object));
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> ClassRefToJson(UClass* Class, const TCHAR* ExpectedBaseClassPath = nullptr)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("is_valid"), Class != nullptr);
		Result->SetStringField(TEXT("class_path"), Class ? Class->GetPathName() : FString());
		Result->SetStringField(TEXT("generated_by"), Class && Class->ClassGeneratedBy ? Class->ClassGeneratedBy->GetPathName() : FString());
		Result->SetBoolField(TEXT("abstract"), Class ? Class->HasAnyClassFlags(CLASS_Abstract) : false);
		if (ExpectedBaseClassPath)
		{
			UClass* ExpectedBase = LoadExpectedClass(ExpectedBaseClassPath);
			Result->SetStringField(TEXT("expected_base_class_path"), ExpectedBaseClassPath);
			Result->SetBoolField(TEXT("expected_base_loaded"), ExpectedBase != nullptr);
			Result->SetBoolField(TEXT("child_of_expected_base"), Class && ExpectedBase && Class->IsChildOf(ExpectedBase));
		}
		return Result;
	}

	static bool AddPropertyIfFound(TSharedPtr<FJsonObject>& Target, UObject* Object, const TCHAR* JsonName, const TCHAR* PropertyName)
	{
		if (!Target.IsValid() || !Object)
		{
			return false;
		}
		FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
		if (!Property)
		{
			return false;
		}
		Target->SetField(JsonName, ReadPropertyValue(Object, PropertyName));
		return true;
	}

	static TSharedPtr<FJsonObject> MakeResolvedSummary(const FResolvedLyraObject& Resolved)
	{
		TSharedPtr<FJsonObject> Summary = ObjectRefToJson(Resolved.Object);
		Summary->SetStringField(TEXT("input_path"), Resolved.InputPath);
		Summary->SetStringField(TEXT("resolved_path"), Resolved.ResolvedPath);
		Summary->SetStringField(TEXT("save_target_path"), Resolved.SaveTargetPath);
		Summary->SetStringField(TEXT("source_kind"), Resolved.SourceKind);
		Summary->SetStringField(TEXT("expected_class_path"), Resolved.ExpectedClass ? Resolved.ExpectedClass->GetPathName() : FString());
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildFragmentSummary(UObject* Fragment, int32 Index)
	{
		TSharedPtr<FJsonObject> Row = ObjectRefToJson(Fragment);
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("object_name"), Fragment ? Fragment->GetName() : FString());
		Row->SetStringField(TEXT("equipment_definition_class"), GetClassPropertyValue(Fragment, TEXT("EquipmentDefinition")) ? GetClassPropertyValue(Fragment, TEXT("EquipmentDefinition"))->GetPathName() : FString());
		Row->SetBoolField(TEXT("has_equipment_definition"), GetClassPropertyValue(Fragment, TEXT("EquipmentDefinition")) != nullptr);

		TSharedPtr<FJsonObject> KnownProperties = MakeShared<FJsonObject>();
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("equipment_definition"), TEXT("EquipmentDefinition"));
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("initial_item_stats"), TEXT("InitialItemStats"));
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("brush"), TEXT("Brush"));
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("display_mesh"), TEXT("DisplayMesh"));
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("pickup_mesh"), TEXT("PickupMesh"));
		AddPropertyIfFound(KnownProperties, Fragment, TEXT("reticle_widgets"), TEXT("ReticleWidgets"));
		Row->SetObjectField(TEXT("known_properties"), KnownProperties);
		return Row;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildFragmentRows(UObject* InventoryItem)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<UObject*> Fragments = GetObjectArrayProperty(InventoryItem, TEXT("Fragments"));
		Rows.Reserve(Fragments.Num());
		for (int32 Index = 0; Index < Fragments.Num(); ++Index)
		{
			Rows.Add(MakeShared<FJsonValueObject>(BuildFragmentSummary(Fragments[Index], Index)));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> BuildInventoryItemSummary(const FResolvedLyraObject& Resolved)
	{
		UObject* Item = Resolved.Object;
		TSharedPtr<FJsonObject> Summary = MakeResolvedSummary(Resolved);
		Summary->SetField(TEXT("display_name"), ReadPropertyValue(Item, TEXT("DisplayName")));
		Summary->SetArrayField(TEXT("fragments"), BuildFragmentRows(Item));
		Summary->SetNumberField(TEXT("fragment_count"), GetArrayPropertyCount(Item, TEXT("Fragments")));
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildEquipmentDefinitionSummary(const FResolvedLyraObject& Resolved)
	{
		UObject* Equipment = Resolved.Object;
		TSharedPtr<FJsonObject> Summary = MakeResolvedSummary(Resolved);
		Summary->SetObjectField(TEXT("instance_type"), ClassRefToJson(GetClassPropertyValue(Equipment, TEXT("InstanceType"))));
		Summary->SetField(TEXT("ability_sets_to_grant"), ReadPropertyValue(Equipment, TEXT("AbilitySetsToGrant")));
		Summary->SetNumberField(TEXT("ability_set_count"), GetArrayPropertyCount(Equipment, TEXT("AbilitySetsToGrant")));
		Summary->SetField(TEXT("actors_to_spawn"), ReadPropertyValue(Equipment, TEXT("ActorsToSpawn")));
		Summary->SetNumberField(TEXT("actor_to_spawn_count"), GetArrayPropertyCount(Equipment, TEXT("ActorsToSpawn")));
		return Summary;
	}

	static UObject* GetFirstFragmentWithClassName(UObject* InventoryItem, const FString& ClassNameSubstring)
	{
		for (UObject* Fragment : GetObjectArrayProperty(InventoryItem, TEXT("Fragments")))
		{
			if (Fragment && Fragment->GetClass() && Fragment->GetClass()->GetPathName().Contains(ClassNameSubstring))
			{
				return Fragment;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> BuildPawnDataSummary(const FResolvedLyraObject& Resolved)
	{
		UObject* PawnData = Resolved.Object;
		TSharedPtr<FJsonObject> Summary = MakeResolvedSummary(Resolved);
		Summary->SetObjectField(TEXT("pawn_class"), ClassRefToJson(GetClassPropertyValue(PawnData, TEXT("PawnClass")), TEXT("/Script/Engine.Pawn")));
		Summary->SetField(TEXT("ability_sets"), ReadPropertyValue(PawnData, TEXT("AbilitySets")));
		Summary->SetNumberField(TEXT("ability_set_count"), GetArrayPropertyCount(PawnData, TEXT("AbilitySets")));
		Summary->SetObjectField(TEXT("tag_relationship_mapping"), ObjectRefToJson(GetObjectPropertyValue(PawnData, TEXT("TagRelationshipMapping"))));
		Summary->SetField(TEXT("tag_relationship_mapping_value"), ReadPropertyValue(PawnData, TEXT("TagRelationshipMapping")));
		Summary->SetObjectField(TEXT("input_config"), ObjectRefToJson(GetObjectPropertyValue(PawnData, TEXT("InputConfig"))));
		Summary->SetField(TEXT("input_config_value"), ReadPropertyValue(PawnData, TEXT("InputConfig")));
		Summary->SetObjectField(TEXT("default_camera_mode"), ClassRefToJson(GetClassPropertyValue(PawnData, TEXT("DefaultCameraMode"))));
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildTeamCreationSummary(const FResolvedLyraObject& Resolved)
	{
		UObject* Component = Resolved.Object;
		TSharedPtr<FJsonObject> Summary = MakeResolvedSummary(Resolved);
		Summary->SetField(TEXT("teams_to_create"), ReadPropertyValue(Component, TEXT("TeamsToCreate")));
		Summary->SetNumberField(TEXT("team_entry_count"), GetMapPropertyCount(Component, TEXT("TeamsToCreate")));
		Summary->SetObjectField(TEXT("public_team_info_class"), ClassRefToJson(GetClassPropertyValue(Component, TEXT("PublicTeamInfoClass")), TEXT("/Script/LyraGame.LyraTeamPublicInfo")));
		Summary->SetObjectField(TEXT("private_team_info_class"), ClassRefToJson(GetClassPropertyValue(Component, TEXT("PrivateTeamInfoClass")), TEXT("/Script/LyraGame.LyraTeamPrivateInfo")));
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildCharacterPartClassSummary(const FString& InputPath)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("input_path"), InputPath);
		UClass* ActorBase = LoadExpectedClass(EngineActorClassPath);
		UClass* Class = LoadClassPathWithGeneratedFallback(InputPath);
		const bool bChildOfActor = Class && ActorBase && Class->IsChildOf(ActorBase);
		Row->SetObjectField(TEXT("class"), ClassRefToJson(Class, EngineActorClassPath));
		Row->SetBoolField(TEXT("ok"), bChildOfActor && !Class->HasAnyClassFlags(CLASS_Abstract));
		FString Issue;
		if (!Class)
		{
			Issue = TEXT("class_not_loaded");
		}
		else if (!bChildOfActor)
		{
			Issue = TEXT("class_not_actor");
		}
		else if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			Issue = TEXT("class_is_abstract");
		}
		Row->SetStringField(TEXT("issue"), Issue);
		return Row;
	}

	static TArray<TSharedPtr<FJsonValue>> BuildActionRows(UObject* Owner)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<UObject*> Actions = GetObjectArrayProperty(Owner, TEXT("Actions"));
		Rows.Reserve(Actions.Num());
		for (int32 Index = 0; Index < Actions.Num(); ++Index)
		{
			UObject* Action = Actions[Index];
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetBoolField(TEXT("is_valid"), Action != nullptr);
			Row->SetStringField(TEXT("object_path"), Action ? Action->GetPathName() : FString());
			Row->SetStringField(TEXT("class_path"), Action && Action->GetClass() ? Action->GetClass()->GetPathName() : FString());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> BuildActionSetSummary(UObject* ActionSet)
	{
		TSharedPtr<FJsonObject> Summary = ObjectRefToJson(ActionSet);
		Summary->SetArrayField(TEXT("game_features_to_enable"), StringArrayToJson(GetStringArrayProperty(ActionSet, TEXT("GameFeaturesToEnable"))));
		Summary->SetArrayField(TEXT("actions"), BuildActionRows(ActionSet));
		Summary->SetNumberField(TEXT("action_count"), GetObjectArrayProperty(ActionSet, TEXT("Actions")).Num());
		return Summary;
	}

	static TSharedPtr<FJsonObject> BuildExperienceGraph(const FResolvedLyraObject& Resolved)
	{
		UObject* Experience = Resolved.Object;
		TSharedPtr<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetStringField(TEXT("input_path"), Resolved.InputPath);
		Graph->SetStringField(TEXT("resolved_path"), Resolved.ResolvedPath);
		Graph->SetStringField(TEXT("save_target_path"), Resolved.SaveTargetPath);
		Graph->SetStringField(TEXT("source_kind"), Resolved.SourceKind);
		Graph->SetStringField(TEXT("class_path"), Experience && Experience->GetClass() ? Experience->GetClass()->GetPathName() : FString());
		if (const UPrimaryDataAsset* PrimaryDataAsset = Cast<UPrimaryDataAsset>(Experience))
		{
			Graph->SetStringField(TEXT("primary_asset_id"), PrimaryDataAsset->GetPrimaryAssetId().ToString());
		}

		UObject* DefaultPawnData = nullptr;
		if (FObjectProperty* PawnDataProperty = Experience ? FindFProperty<FObjectProperty>(Experience->GetClass(), TEXT("DefaultPawnData")) : nullptr)
		{
			DefaultPawnData = PawnDataProperty->GetObjectPropertyValue_InContainer(Experience);
		}
		Graph->SetObjectField(TEXT("default_pawn_data"), ObjectRefToJson(DefaultPawnData));
		Graph->SetArrayField(TEXT("game_features_to_enable"), StringArrayToJson(GetStringArrayProperty(Experience, TEXT("GameFeaturesToEnable"))));
		Graph->SetArrayField(TEXT("actions"), BuildActionRows(Experience));
		Graph->SetNumberField(TEXT("action_count"), GetObjectArrayProperty(Experience, TEXT("Actions")).Num());

		TArray<UObject*> ActionSets = GetObjectArrayProperty(Experience, TEXT("ActionSets"));
		TArray<TSharedPtr<FJsonValue>> ActionSetRows;
		ActionSetRows.Reserve(ActionSets.Num());
		for (int32 Index = 0; Index < ActionSets.Num(); ++Index)
		{
			TSharedPtr<FJsonObject> Row = BuildActionSetSummary(ActionSets[Index]);
			Row->SetNumberField(TEXT("index"), Index);
			ActionSetRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Graph->SetArrayField(TEXT("action_sets"), ActionSetRows);
		Graph->SetNumberField(TEXT("action_set_count"), ActionSets.Num());
		return Graph;
	}

	static TSharedPtr<FJsonObject> PhaseAbilitySummaryToJson(const FPhaseAbilitySummary& Summary)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("input_path"), Summary.InputPath);
		Row->SetStringField(TEXT("asset_path"), Summary.AssetPath);
		Row->SetStringField(TEXT("class_path"), Summary.ClassPath);
		Row->SetStringField(TEXT("cdo_path"), Summary.CdoPath);
		Row->SetStringField(TEXT("source_kind"), Summary.SourceKind);
		Row->SetBoolField(TEXT("is_blueprint"), Summary.bIsBlueprint);
		Row->SetBoolField(TEXT("is_abstract"), Summary.bIsAbstract);
		Row->SetBoolField(TEXT("game_phase_tag_valid"), Summary.bPhaseTagValid);
		Row->SetStringField(TEXT("game_phase_tag"), Summary.PhaseTagString);
		Row->SetObjectField(TEXT("game_phase_tag_detail"), GameplayTagToJson(RequestTagNoError(Summary.PhaseTagString)));
		return Row;
	}

	static bool AddPhaseAbilitySummary(
		UClass* AbilityClass,
		const FString& InputPath,
		const FString& AssetPath,
		const FString& SourceKind,
		bool bIsBlueprint,
		TSet<FString>& SeenClassPaths,
		TArray<FPhaseAbilitySummary>& OutSummaries)
	{
		if (!AbilityClass)
		{
			return false;
		}

		const FString ClassPath = AbilityClass->GetPathName();
		const FString ClassKey = ClassPath.ToLower();
		if (SeenClassPaths.Contains(ClassKey))
		{
			return false;
		}
		SeenClassPaths.Add(ClassKey);

		UObject* CDO = AbilityClass->GetDefaultObject();
		FGameplayTag PhaseTag;
		const bool bHasPhaseTagProperty = TryGetGameplayTagProperty(CDO, TEXT("GamePhaseTag"), PhaseTag);

		FPhaseAbilitySummary Summary;
		Summary.InputPath = InputPath;
		Summary.AssetPath = AssetPath;
		Summary.ClassPath = ClassPath;
		Summary.CdoPath = CDO ? CDO->GetPathName() : FString();
		Summary.SourceKind = SourceKind;
		Summary.PhaseTagString = bHasPhaseTagProperty ? PhaseTag.ToString() : FString();
		Summary.bPhaseTagValid = bHasPhaseTagProperty && PhaseTag.IsValid();
		Summary.bIsAbstract = AbilityClass->HasAnyClassFlags(CLASS_Abstract);
		Summary.bIsBlueprint = bIsBlueprint;
		OutSummaries.Add(Summary);
		return true;
	}

	static void CollectBlueprintPhaseAbilitySummaries(
		UClass* PhaseAbilityBaseClass,
		const FString& PathFilter,
		int32 MaxAssets,
		bool& bOutTruncated,
		TSet<FString>& SeenClassPaths,
		TArray<FPhaseAbilitySummary>& OutSummaries)
	{
		bOutTruncated = false;
		if (!PhaseAbilityBaseClass)
		{
			return;
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		if (!PathFilter.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PathFilter));
			Filter.bRecursivePaths = true;
		}

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);
		Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});

		int32 VisitedMatchingAssets = 0;
		for (const FAssetData& AssetData : Assets)
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
			if (!Blueprint || !Blueprint->GeneratedClass || !Blueprint->GeneratedClass->IsChildOf(PhaseAbilityBaseClass))
			{
				continue;
			}

			if (VisitedMatchingAssets >= MaxAssets)
			{
				bOutTruncated = true;
				break;
			}
			++VisitedMatchingAssets;

			AddPhaseAbilitySummary(
				Blueprint->GeneratedClass,
				AssetData.GetObjectPathString(),
				AssetData.GetObjectPathString(),
				TEXT("blueprint_generated_class_default_object"),
				/*bIsBlueprint=*/true,
			SeenClassPaths,
			OutSummaries);
		}
	}

	static void CollectLoadedNativePhaseAbilitySummaries(
		UClass* PhaseAbilityBaseClass,
		TSet<FString>& SeenClassPaths,
		TArray<FPhaseAbilitySummary>& OutSummaries)
	{
		if (!PhaseAbilityBaseClass)
		{
			return;
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || Class == PhaseAbilityBaseClass || !Class->IsChildOf(PhaseAbilityBaseClass))
			{
				continue;
			}
			if (Class->ClassGeneratedBy)
			{
				continue;
			}

			AddPhaseAbilitySummary(
				Class,
				Class->GetPathName(),
				Class->GetPathName(),
				TEXT("loaded_native_class_default_object"),
				/*bIsBlueprint=*/false,
				SeenClassPaths,
				OutSummaries);
		}
	}

	static TSharedPtr<FJsonObject> BuildGameplayTagDomain(const FString& RootTagName, bool bIncludeChildren, int32 MaxTags)
	{
		const FGameplayTag RootTag = RequestTagNoError(RootTagName);
		TSharedPtr<FJsonObject> Domain = MakeShared<FJsonObject>();
		Domain->SetStringField(TEXT("root_tag"), RootTagName);
		Domain->SetBoolField(TEXT("root_tag_registered"), RootTag.IsValid());
		Domain->SetObjectField(TEXT("root"), GameplayTagToJson(RootTag));

		FGameplayTagContainer ChildTags;
		if (RootTag.IsValid() && bIncludeChildren)
		{
			ChildTags = UGameplayTagsManager::Get().RequestGameplayTagChildren(RootTag);
		}

		bool bChildrenTruncated = false;
		Domain->SetArrayField(TEXT("children"), GameplayTagsToJsonArray(ChildTags, MaxTags, bChildrenTruncated));
		Domain->SetNumberField(TEXT("child_count"), ChildTags.Num());
		Domain->SetBoolField(TEXT("children_truncated"), bChildrenTruncated);
		return Domain;
	}

	static bool TryReadMaxParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32 DefaultValue, int32& OutValue, FString& OutError)
	{
		OutValue = DefaultValue;
		if (!TryReadIntParam(Params, FieldName, OutValue, OutError))
		{
			return false;
		}
		if (OutValue <= 0)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be greater than zero"), FieldName);
			return false;
		}
		return true;
	}

	static bool ContainsNormalizedPath(const TArray<FString>& Values, const FString& Candidate)
	{
		const FString NormalizedCandidate = NormalizeObjectPathForCompare(Candidate);
		return Values.ContainsByPredicate([&NormalizedCandidate](const FString& Value)
		{
			return NormalizeObjectPathForCompare(Value).Equals(NormalizedCandidate, ESearchCase::IgnoreCase);
		});
	}

	static TSharedPtr<FJsonObject> BuildUserFacingSummary(const FResolvedLyraObject& Resolved)
	{
		UObject* Object = Resolved.Object;
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("input_path"), Resolved.InputPath);
		Summary->SetStringField(TEXT("resolved_path"), Resolved.ResolvedPath);
		Summary->SetStringField(TEXT("save_target_path"), Resolved.SaveTargetPath);
		Summary->SetStringField(TEXT("source_kind"), Resolved.SourceKind);
		Summary->SetStringField(TEXT("class_path"), Object && Object->GetClass() ? Object->GetClass()->GetPathName() : FString());
		if (const UPrimaryDataAsset* PrimaryDataAsset = Cast<UPrimaryDataAsset>(Object))
		{
			Summary->SetStringField(TEXT("primary_asset_id"), PrimaryDataAsset->GetPrimaryAssetId().ToString());
		}

		FPrimaryAssetId MapId;
		FPrimaryAssetId ExperienceId;
		TryGetPrimaryAssetIdProperty(Object, TEXT("MapID"), MapId);
		TryGetPrimaryAssetIdProperty(Object, TEXT("ExperienceID"), ExperienceId);
		Summary->SetObjectField(TEXT("map_id"), PrimaryAssetIdToJson(MapId));
		Summary->SetObjectField(TEXT("experience_id"), PrimaryAssetIdToJson(ExperienceId));

		const TCHAR* SimpleFields[] =
		{
			TEXT("ExtraArgs"),
			TEXT("TileTitle"),
			TEXT("TileSubTitle"),
			TEXT("TileDescription"),
			TEXT("TileIcon"),
			TEXT("LoadingScreenWidget"),
			TEXT("bIsDefaultExperience"),
			TEXT("bShowInFrontEnd"),
			TEXT("bRecordReplay"),
			TEXT("MaxPlayerCount"),
			TEXT("SessionMode"),
			TEXT("bUseLobbies"),
			TEXT("bUseLobbiesVoiceChat"),
			TEXT("bUsePresence")
		};

		for (const TCHAR* FieldName : SimpleFields)
		{
			Summary->SetField(FieldName, ReadPropertyValue(Object, FieldName));
		}

		return Summary;
	}

	static bool EnsureBlueprintBulkFillAdapter(FString& OutError)
	{
		FMonolithBulkFillRegistry& BulkFillRegistry = FMonolithBulkFillRegistry::Get();
		if (BulkFillRegistry.HasAdapter(TEXT("blueprint")))
		{
			return true;
		}

		FModuleManager::Get().LoadModule(TEXT("MonolithBlueprint"));
		if (BulkFillRegistry.HasAdapter(TEXT("blueprint")))
		{
			return true;
		}

		OutError = TEXT("Monolith blueprint bulk_fill adapter is not registered; load MonolithBlueprint before using Lyra write actions");
		return false;
	}

	static bool SaveAssetIfRequested(UObject* AssetForSave, bool bSave, bool& bOutSaved, FString& OutSavedPath, FString& OutError)
	{
		bOutSaved = false;
		OutSavedPath.Reset();
		if (!bSave)
		{
			return true;
		}
		if (!AssetForSave)
		{
			OutError = TEXT("Cannot save because the resolved Lyra target has no persistent asset object");
			return false;
		}

		UPackage* Package = AssetForSave->GetOutermost();
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Cannot save asset with no package: %s"), *AssetForSave->GetPathName());
			return false;
		}
		if (!FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), OutSavedPath, FPackageName::GetAssetPackageExtension()))
		{
			OutError = FString::Printf(TEXT("Could not convert package '%s' to an asset filename"), *Package->GetName());
			return false;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		bOutSaved = UPackage::SavePackage(Package, AssetForSave, *OutSavedPath, SaveArgs);
		if (!bOutSaved)
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *OutSavedPath);
			return false;
		}
		return true;
	}

	static bool IsReportCleanForCommit(const FDryRunReport& Report)
	{
		return Report.Errors == 0 && Report.bWouldApply;
	}

	static bool WouldChangeFromReport(const FDryRunReport& Report)
	{
		for (const FBulkFillFieldWrite& Write : Report.FieldWrites)
		{
			if (Write.bOk)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeWritePlan(const FLyraMutationOptions& Options, bool bWouldCompileBlueprint, const FString& PackageFilename)
	{
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetBoolField(TEXT("would_save_package"), Options.bSave && !PackageFilename.IsEmpty());
		Plan->SetBoolField(TEXT("would_compile_blueprint"), bWouldCompileBlueprint);
		Plan->SetStringField(TEXT("package_filename"), PackageFilename);
		return Plan;
	}

	static bool TryGetPackageFilename(UObject* AssetForSave, FString& OutFilename)
	{
		OutFilename.Reset();
		if (!AssetForSave || !AssetForSave->GetOutermost())
		{
			return false;
		}
		return FPackageName::TryConvertLongPackageNameToFilename(
			AssetForSave->GetOutermost()->GetName(),
			OutFilename,
			FPackageName::GetAssetPackageExtension());
	}

	static void AddMutationFields(
		TSharedPtr<FJsonObject>& Result,
		const FString& ActionName,
		const FResolvedLyraObject& Resolved,
		const FLyraMutationOptions& Options,
		bool bChanged,
		bool bSaved,
		const FString& SavedPath)
	{
		Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
		Result->SetStringField(TEXT("action"), ActionName);
		Result->SetStringField(TEXT("target"), Resolved.InputPath);
		Result->SetStringField(TEXT("resolved_path"), Resolved.ResolvedPath);
		Result->SetStringField(TEXT("save_target_path"), Resolved.SaveTargetPath);
		Result->SetStringField(TEXT("source_kind"), Resolved.SourceKind);
		Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
		Result->SetBoolField(TEXT("confirm_received"), Options.bConfirm);
		Result->SetBoolField(TEXT("save_requested"), Options.bSave);
		Result->SetBoolField(TEXT("changed"), bChanged);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetStringField(TEXT("saved_path"), SavedPath);
	}

	static bool AddStringFieldIfPresent(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName, TSharedPtr<FJsonObject>& Tree, const TCHAR* PropertyName, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(ParamName))
		{
			return true;
		}

		FString Value;
		if (!Params->TryGetStringField(ParamName, Value))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), ParamName);
			return false;
		}
		Tree->SetStringField(PropertyName, Value);
		return true;
	}

	static bool AddBoolFieldIfPresent(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName, TSharedPtr<FJsonObject>& Tree, const TCHAR* PropertyName, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(ParamName))
		{
			return true;
		}

		bool bValue = false;
		if (!Params->TryGetBoolField(ParamName, bValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), ParamName);
			return false;
		}
		Tree->SetBoolField(PropertyName, bValue);
		return true;
	}

	static bool AddIntFieldIfPresent(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName, TSharedPtr<FJsonObject>& Tree, const TCHAR* PropertyName, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(ParamName))
		{
			return true;
		}

		double NumberValue = 0.0;
		if (!Params->TryGetNumberField(ParamName, NumberValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a number"), ParamName);
			return false;
		}
		Tree->SetNumberField(PropertyName, static_cast<int32>(NumberValue));
		return true;
	}

	static bool AddStringArrayFieldIfPresent(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName, TSharedPtr<FJsonObject>& Tree, const TCHAR* PropertyName, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(ParamName))
		{
			return true;
		}

		TArray<FString> Values;
		if (!TryReadStringArrayParam(Params, ParamName, Values, OutError))
		{
			return false;
		}
		Tree->SetArrayField(PropertyName, StringArrayToJson(Values));
		return true;
	}

	static bool AddObjectFieldIfPresent(const TSharedPtr<FJsonObject>& Params, const TCHAR* ParamName, TSharedPtr<FJsonObject>& Tree, const TCHAR* PropertyName, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(ParamName))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* ObjectValue = nullptr;
		if (!Params->TryGetObjectField(ParamName, ObjectValue) || !ObjectValue || !ObjectValue->IsValid())
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an object"), ParamName);
			return false;
		}
		Tree->SetObjectField(PropertyName, *ObjectValue);
		return true;
	}

	static bool ValidatePrimaryAssetIdType(const TSharedPtr<FJsonObject>& Tree, const TCHAR* FieldName, const TCHAR* ExpectedType, FString& OutError)
	{
		if (!Tree.IsValid() || !Tree->HasField(FieldName))
		{
			return true;
		}

		FString Value;
		if (!Tree->TryGetStringField(FieldName, Value))
		{
			return true;
		}
		const FPrimaryAssetId AssetId = FPrimaryAssetId::FromString(Value);
		if (!AssetId.IsValid())
		{
			OutError = FString::Printf(TEXT("%s must be a valid PrimaryAssetId in Type:Name form"), FieldName);
			return false;
		}
		if (!AssetId.PrimaryAssetType.ToString().Equals(ExpectedType, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("%s must have PrimaryAssetType '%s', got '%s'"), FieldName, ExpectedType, *AssetId.PrimaryAssetType.ToString());
			return false;
		}
		return true;
	}

	static bool DispatchBlueprintBulkFill(
		const FResolvedLyraObject& Resolved,
		const TSharedPtr<FJsonObject>& Tree,
		const FLyraMutationOptions& Options,
		FDryRunReport& OutReport,
		FString& OutError)
	{
		if (!Tree.IsValid() || Tree->Values.Num() == 0)
		{
			OutError = TEXT("At least one property value must be provided");
			return false;
		}
		if (!EnsureBlueprintBulkFillAdapter(OutError))
		{
			return false;
		}

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("blueprint");
		Spec.TargetAsset = Resolved.InputPath;
		Spec.Tree = Tree;
		Spec.bDryRun = Options.bDryRun;
		Spec.bStrict = Options.bStrict;
		OutReport = FMonolithBulkFillRegistry::Get().DispatchBulkFill(Spec);
		return true;
	}

	static FString StripExportClassWrapper(FString Value)
	{
		Value.TrimStartAndEndInline();
		int32 FirstQuote = INDEX_NONE;
		int32 LastQuote = INDEX_NONE;
		if (Value.FindChar(TEXT('\''), FirstQuote) && Value.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
		{
			Value = Value.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
		}
		return Value;
	}

	static FString NormalizeReferenceForCompare(const FString& Value)
	{
		return NormalizeObjectPathForCompare(StripExportClassWrapper(Value));
	}

	static FString ExportStructFieldText(void* StructValuePtr, UScriptStruct* Struct, const TCHAR* FieldName)
	{
		if (!StructValuePtr || !Struct)
		{
			return FString();
		}
		FProperty* Property = Struct->FindPropertyByName(FieldName);
		if (!Property)
		{
			return FString();
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(StructValuePtr);
		FString Value;
		Property->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
		return StripExportClassWrapper(Value);
	}

	static bool TryGetActionsArray(UObject* Owner, FArrayProperty*& OutArrayProperty, FObjectPropertyBase*& OutObjectProperty, FString& OutError)
	{
		OutArrayProperty = nullptr;
		OutObjectProperty = nullptr;
		if (!Owner)
		{
			OutError = TEXT("Action owner is null");
			return false;
		}

		OutArrayProperty = FindFProperty<FArrayProperty>(Owner->GetClass(), TEXT("Actions"));
		if (!OutArrayProperty)
		{
			OutError = FString::Printf(TEXT("Object '%s' does not expose an Actions array"), *Owner->GetPathName());
			return false;
		}
		OutObjectProperty = CastField<FObjectPropertyBase>(OutArrayProperty->Inner);
		if (!OutObjectProperty)
		{
			OutError = FString::Printf(TEXT("Actions array on '%s' is not an object reference array"), *Owner->GetPathName());
			return false;
		}
		return true;
	}

	static UClass* LoadSubclassForParam(
		const FString& ClassPath,
		const TCHAR* ParamName,
		const TCHAR* ExpectedBaseClassPath,
		FString& OutError)
	{
		UClass* ExpectedBaseClass = LoadExpectedClass(ExpectedBaseClassPath);
		if (!ExpectedBaseClass)
		{
			OutError = FString::Printf(TEXT("Required base class '%s' is unavailable"), ExpectedBaseClassPath);
			return nullptr;
		}

		UClass* LoadedClass = LoadClassPathWithGeneratedFallback(ClassPath);
		if (!LoadedClass)
		{
			OutError = FString::Printf(TEXT("Could not load class '%s' for param '%s'"), *ClassPath, ParamName);
			return nullptr;
		}
		if (!LoadedClass->IsChildOf(ExpectedBaseClass))
		{
			OutError = FString::Printf(
				TEXT("Class '%s' for param '%s' is not a child of '%s'"),
				*LoadedClass->GetPathName(),
				ParamName,
				*ExpectedBaseClass->GetPathName());
			return nullptr;
		}
		return LoadedClass;
	}

	static UObject* FindAddComponentsAction(
		FScriptArrayHelper& ActionsHelper,
		const FObjectPropertyBase* ActionsObjectProperty,
		UClass* AddComponentsActionClass,
		const FString& RequestedActionName,
		int32& OutActionIndex,
		int32& OutAddComponentsActionCount,
		FString& OutError)
	{
		OutActionIndex = INDEX_NONE;
		OutAddComponentsActionCount = 0;
		UObject* FirstCompatibleAction = nullptr;
		int32 FirstCompatibleIndex = INDEX_NONE;
		UObject* RequestedAction = nullptr;
		int32 RequestedActionIndex = INDEX_NONE;

		for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
		{
			UObject* Action = ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index));
			if (!Action)
			{
				continue;
			}

			if (Action->IsA(AddComponentsActionClass))
			{
				++OutAddComponentsActionCount;
				if (!FirstCompatibleAction)
				{
					FirstCompatibleAction = Action;
					FirstCompatibleIndex = Index;
				}
			}

			if (!RequestedActionName.IsEmpty() && Action->GetName().Equals(RequestedActionName, ESearchCase::IgnoreCase))
			{
				if (!Action->IsA(AddComponentsActionClass))
				{
					OutError = FString::Printf(
						TEXT("Action '%s' exists at index %d but is '%s', not '%s'"),
						*RequestedActionName,
						Index,
						*Action->GetClass()->GetPathName(),
						*AddComponentsActionClass->GetPathName());
					return nullptr;
				}
				RequestedAction = Action;
				RequestedActionIndex = Index;
			}
		}

		if (!RequestedActionName.IsEmpty())
		{
			OutActionIndex = RequestedActionIndex;
			return RequestedAction;
		}
		if (RequestedActionName.IsEmpty())
		{
			OutActionIndex = FirstCompatibleIndex;
			return FirstCompatibleAction;
		}
		return nullptr;
	}

	static bool BuildComponentEntryMutationPlan(
		UObject* Action,
		UClass* AddComponentsActionClass,
		UClass* ActorClass,
		UClass* ComponentClass,
		bool bClientComponent,
		bool bServerComponent,
		int32 AdditionFlags,
		FComponentEntryMutationPlan& OutPlan,
		FString& OutError)
	{
		OutPlan = FComponentEntryMutationPlan();
		if (!AddComponentsActionClass || !ActorClass || !ComponentClass)
		{
			OutError = TEXT("AddComponents action, actor, and component classes are required");
			return false;
		}

		OutPlan.ComponentListProperty = FindFProperty<FArrayProperty>(AddComponentsActionClass, TEXT("ComponentList"));
		OutPlan.EntryStructProperty = OutPlan.ComponentListProperty
			? CastField<FStructProperty>(OutPlan.ComponentListProperty->Inner)
			: nullptr;
		if (!OutPlan.ComponentListProperty || !OutPlan.EntryStructProperty || !OutPlan.EntryStructProperty->Struct)
		{
			OutError = FString::Printf(
				TEXT("Action class '%s' must expose ComponentList as a struct array"),
				*AddComponentsActionClass->GetPathName());
			return false;
		}

		UScriptStruct* EntryStruct = OutPlan.EntryStructProperty->Struct;
		OutPlan.ActorClassProperty = FindFProperty<FSoftClassProperty>(EntryStruct, TEXT("ActorClass"));
		OutPlan.ComponentClassProperty = FindFProperty<FSoftClassProperty>(EntryStruct, TEXT("ComponentClass"));
		OutPlan.ClientProperty = FindFProperty<FBoolProperty>(EntryStruct, TEXT("bClientComponent"));
		OutPlan.ServerProperty = FindFProperty<FBoolProperty>(EntryStruct, TEXT("bServerComponent"));
		OutPlan.AdditionFlagsProperty = FindFProperty<FNumericProperty>(EntryStruct, TEXT("AdditionFlags"));
		if (!OutPlan.ActorClassProperty
			|| !OutPlan.ComponentClassProperty
			|| !OutPlan.ClientProperty
			|| !OutPlan.ServerProperty
			|| !OutPlan.AdditionFlagsProperty
			|| !OutPlan.AdditionFlagsProperty->IsInteger())
		{
			OutError = FString::Printf(
				TEXT("ComponentList entry struct '%s' must expose ActorClass, ComponentClass, bClientComponent, bServerComponent, and integer AdditionFlags fields"),
				*EntryStruct->GetName());
			return false;
		}

		if ((OutPlan.ActorClassProperty->MetaClass && !ActorClass->IsChildOf(OutPlan.ActorClassProperty->MetaClass))
			|| (OutPlan.ComponentClassProperty->MetaClass && !ComponentClass->IsChildOf(OutPlan.ComponentClassProperty->MetaClass)))
		{
			OutError = TEXT("Actor or component class is incompatible with the reflected ComponentList field contract");
			return false;
		}

		if (!Action)
		{
			OutPlan.bAdd = true;
			OutPlan.ComponentsAfter = 1;
			OutPlan.ComponentIndex = 0;
			return true;
		}

		FScriptArrayHelper ComponentHelper(
			OutPlan.ComponentListProperty,
			OutPlan.ComponentListProperty->ContainerPtrToValuePtr<void>(Action));
		OutPlan.ComponentsBefore = ComponentHelper.Num();
		OutPlan.ComponentsAfter = ComponentHelper.Num();
		int32 MatchingEntryCount = 0;
		for (int32 Index = 0; Index < ComponentHelper.Num(); ++Index)
		{
			void* EntryPtr = ComponentHelper.GetRawPtr(Index);
			const FSoftObjectPtr ExistingActorClass = OutPlan.ActorClassProperty->GetPropertyValue(
				OutPlan.ActorClassProperty->ContainerPtrToValuePtr<void>(EntryPtr));
			const FSoftObjectPtr ExistingComponentClass = OutPlan.ComponentClassProperty->GetPropertyValue(
				OutPlan.ComponentClassProperty->ContainerPtrToValuePtr<void>(EntryPtr));
			const bool bSameActor = NormalizeReferenceForCompare(ExistingActorClass.ToSoftObjectPath().ToString()).Equals(
				NormalizeReferenceForCompare(ActorClass->GetPathName()),
				ESearchCase::IgnoreCase);
			const bool bSameComponent = NormalizeReferenceForCompare(ExistingComponentClass.ToSoftObjectPath().ToString()).Equals(
				NormalizeReferenceForCompare(ComponentClass->GetPathName()),
				ESearchCase::IgnoreCase);
			if (!bSameActor || !bSameComponent)
			{
				continue;
			}

			++MatchingEntryCount;
			OutPlan.ComponentIndex = Index;
			const bool bCurrentClient = OutPlan.ClientProperty->GetPropertyValue(
				OutPlan.ClientProperty->ContainerPtrToValuePtr<void>(EntryPtr));
			const bool bCurrentServer = OutPlan.ServerProperty->GetPropertyValue(
				OutPlan.ServerProperty->ContainerPtrToValuePtr<void>(EntryPtr));
			const int64 CurrentFlags = FCString::Atoi64(*OutPlan.AdditionFlagsProperty->GetNumericPropertyValueToString(
				OutPlan.AdditionFlagsProperty->ContainerPtrToValuePtr<void>(EntryPtr)));
			OutPlan.bUpdate = bCurrentClient != bClientComponent
				|| bCurrentServer != bServerComponent
				|| CurrentFlags != AdditionFlags;
		}

		if (MatchingEntryCount > 1)
		{
			OutError = FString::Printf(
				TEXT("ComponentList contains %d duplicate entries for actor '%s' and component '%s'; remove duplicates before updating"),
				MatchingEntryCount,
				*ActorClass->GetPathName(),
				*ComponentClass->GetPathName());
			return false;
		}
		if (MatchingEntryCount == 0)
		{
			OutPlan.bAdd = true;
			OutPlan.ComponentIndex = ComponentHelper.Num();
			OutPlan.ComponentsAfter = ComponentHelper.Num() + 1;
		}
		return true;
	}

	static bool ApplyComponentEntryMutation(
		UObject* Action,
		UClass* ActorClass,
		UClass* ComponentClass,
		bool bClientComponent,
		bool bServerComponent,
		int32 AdditionFlags,
		FComponentEntryMutationPlan& Plan,
		FString& OutError)
	{
		if (!Action || !Plan.ComponentListProperty || !Plan.EntryStructProperty)
		{
			OutError = TEXT("Component entry mutation plan is incomplete");
			return false;
		}

		FScriptArrayHelper ComponentHelper(
			Plan.ComponentListProperty,
			Plan.ComponentListProperty->ContainerPtrToValuePtr<void>(Action));
		if (Plan.bAdd)
		{
			Plan.ComponentIndex = ComponentHelper.AddValue();
		}
		if (!ComponentHelper.IsValidIndex(Plan.ComponentIndex))
		{
			OutError = FString::Printf(TEXT("ComponentList index %d is invalid during mutation"), Plan.ComponentIndex);
			return false;
		}

		void* EntryPtr = ComponentHelper.GetRawPtr(Plan.ComponentIndex);
		if (Plan.bAdd)
		{
			Plan.ActorClassProperty->SetPropertyValue(
				Plan.ActorClassProperty->ContainerPtrToValuePtr<void>(EntryPtr),
				FSoftObjectPtr(FSoftObjectPath(ActorClass->GetPathName())));
			Plan.ComponentClassProperty->SetPropertyValue(
				Plan.ComponentClassProperty->ContainerPtrToValuePtr<void>(EntryPtr),
				FSoftObjectPtr(FSoftObjectPath(ComponentClass->GetPathName())));
		}
		if (Plan.bAdd || Plan.bUpdate)
		{
			Plan.ClientProperty->SetPropertyValue(
				Plan.ClientProperty->ContainerPtrToValuePtr<void>(EntryPtr),
				bClientComponent);
			Plan.ServerProperty->SetPropertyValue(
				Plan.ServerProperty->ContainerPtrToValuePtr<void>(EntryPtr),
				bServerComponent);
			Plan.AdditionFlagsProperty->SetIntPropertyValue(
				Plan.AdditionFlagsProperty->ContainerPtrToValuePtr<void>(EntryPtr),
				static_cast<uint64>(AdditionFlags));
		}
		Plan.ComponentsAfter = ComponentHelper.Num();
		return true;
	}

	static bool MatchesOptionalPathSelector(const FString& ActualPath, const FString& ExpectedPath)
	{
		return ExpectedPath.IsEmpty()
			|| NormalizeReferenceForCompare(ActualPath).Equals(NormalizeReferenceForCompare(ExpectedPath), ESearchCase::IgnoreCase);
	}

	static void CollectComponentRemovalCandidates(
		UObject* Owner,
		UObject* AssetForSave,
		const FString& OwnerPath,
		int32 ActionIndexFilter,
		const FString& ActionNameFilter,
		int32 ComponentIndexFilter,
		const FString& ActorClassFilter,
		const FString& ComponentClassFilter,
		TArray<FComponentRemovalCandidate>& OutCandidates,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		FString Error;
		FArrayProperty* ActionsProperty = nullptr;
		FObjectPropertyBase* ActionsObjectProperty = nullptr;
		if (!TryGetActionsArray(Owner, ActionsProperty, ActionsObjectProperty, Error))
		{
			OutWarnings.Add(MakeShared<FJsonValueString>(Error));
			return;
		}

		FScriptArrayHelper ActionsHelper(ActionsProperty, ActionsProperty->ContainerPtrToValuePtr<void>(Owner));
		for (int32 ActionIndex = 0; ActionIndex < ActionsHelper.Num(); ++ActionIndex)
		{
			if (ActionIndexFilter != INDEX_NONE && ActionIndex != ActionIndexFilter)
			{
				continue;
			}

			UObject* Action = ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(ActionIndex));
			if (!Action)
			{
				continue;
			}
			if (!ActionNameFilter.IsEmpty() && !Action->GetName().Equals(ActionNameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}

			FArrayProperty* ComponentListProperty = FindFProperty<FArrayProperty>(Action->GetClass(), TEXT("ComponentList"));
			FStructProperty* ComponentEntryProperty = ComponentListProperty ? CastField<FStructProperty>(ComponentListProperty->Inner) : nullptr;
			if (!ComponentListProperty || !ComponentEntryProperty || !ComponentEntryProperty->Struct)
			{
				continue;
			}

			FScriptArrayHelper ComponentHelper(ComponentListProperty, ComponentListProperty->ContainerPtrToValuePtr<void>(Action));
			for (int32 ComponentIndex = 0; ComponentIndex < ComponentHelper.Num(); ++ComponentIndex)
			{
				if (ComponentIndexFilter != INDEX_NONE && ComponentIndex != ComponentIndexFilter)
				{
					continue;
				}

				void* EntryPtr = ComponentHelper.GetRawPtr(ComponentIndex);
				const FString ActorClass = ExportStructFieldText(EntryPtr, ComponentEntryProperty->Struct, TEXT("ActorClass"));
				const FString ComponentClass = ExportStructFieldText(EntryPtr, ComponentEntryProperty->Struct, TEXT("ComponentClass"));
				if (!MatchesOptionalPathSelector(ActorClass, ActorClassFilter) || !MatchesOptionalPathSelector(ComponentClass, ComponentClassFilter))
				{
					continue;
				}

				FComponentRemovalCandidate Candidate;
				Candidate.Owner = Owner;
				Candidate.AssetForSave = AssetForSave;
				Candidate.Action = Action;
				Candidate.ComponentListProperty = ComponentListProperty;
				Candidate.ActionIndex = ActionIndex;
				Candidate.ComponentIndex = ComponentIndex;
				Candidate.OwnerPath = OwnerPath;
				Candidate.ActionPath = Action->GetPathName();
				Candidate.ActorClass = ActorClass;
				Candidate.ComponentClass = ComponentClass;
				OutCandidates.Add(Candidate);
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> ComponentCandidatesToJson(const TArray<FComponentRemovalCandidate>& Candidates)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Candidates.Num());
		for (const FComponentRemovalCandidate& Candidate : Candidates)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("owner_path"), Candidate.OwnerPath);
			Row->SetStringField(TEXT("action_path"), Candidate.ActionPath);
			Row->SetNumberField(TEXT("action_index"), Candidate.ActionIndex);
			Row->SetNumberField(TEXT("component_index"), Candidate.ComponentIndex);
			Row->SetStringField(TEXT("actor_class"), Candidate.ActorClass);
			Row->SetStringField(TEXT("component_class"), Candidate.ComponentClass);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}
}

void FMonolithLyraActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("lyra"), TEXT("get_status"),
		TEXT("Report Lyra reflected type availability and the current read-only Lyra semantic action surface"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::GetStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_experience_graph"),
		TEXT("Describe a Lyra Experience CDO/data-asset graph: GameFeaturesToEnable, DefaultPawnData, Actions, and ActionSets"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeExperienceGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("experience_path"), TEXT("Lyra experience package/object/class path"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_experience_bundle"),
		TEXT("Validate a Lyra Experience bundle contract and optional expected pawn/action-set/game-feature requirements"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateExperienceBundle),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("experience_path"), TEXT("Lyra experience package/object/class path"))
			.Optional(TEXT("require_default_pawn_data"), TEXT("bool"), TEXT("Treat a missing DefaultPawnData as an error"), TEXT("true"))
			.Optional(TEXT("require_action_sets"), TEXT("bool"), TEXT("Treat an empty ActionSets array as an error"), TEXT("false"))
			.Optional(TEXT("validate_default_pawn_data"), TEXT("bool"), TEXT("Validate DefaultPawnData internals when present."), TEXT("false"))
			.Optional(TEXT("require_pawn_class"), TEXT("bool"), TEXT("When validating DefaultPawnData, require PawnClass."), TEXT("true"))
			.Optional(TEXT("require_pawn_ability_sets"), TEXT("bool"), TEXT("When validating DefaultPawnData, require at least one AbilitySet."), TEXT("false"))
			.Optional(TEXT("require_pawn_input_config"), TEXT("bool"), TEXT("When validating DefaultPawnData, require InputConfig."), TEXT("false"))
			.Optional(TEXT("require_default_camera_mode"), TEXT("bool"), TEXT("When validating DefaultPawnData, require DefaultCameraMode."), TEXT("false"))
			.Optional(TEXT("validate_action_sets"), TEXT("bool"), TEXT("Validate composed ActionSet action entries."), TEXT("true"))
			.Optional(TEXT("require_action_set_actions"), TEXT("bool"), TEXT("Require each composed ActionSet to contain at least one Action."), TEXT("false"))
			.Optional(TEXT("disallow_null_actions"), TEXT("bool"), TEXT("Treat null Action entries in composed ActionSets as errors."), TEXT("true"))
			.Optional(TEXT("validate_action_classes"), TEXT("bool"), TEXT("Require Action entries to be concrete GameFeatureAction objects."), TEXT("true"))
			.Optional(TEXT("require_action_set_game_features"), TEXT("bool"), TEXT("Require each composed ActionSet to declare at least one GameFeaturesToEnable entry."), TEXT("false"))
			.Optional(TEXT("validate_game_feature_plugins"), TEXT("bool"), TEXT("Validate all declared GameFeaturesToEnable names against enabled project plugins."), TEXT("false"))
			.Optional(TEXT("expected_pawn_data"), TEXT("string"), TEXT("Expected DefaultPawnData package/object path"))
			.Optional(TEXT("expected_action_sets"), TEXT("array"), TEXT("Expected ActionSet package/object paths"))
			.Optional(TEXT("expected_game_features"), TEXT("array"), TEXT("Expected GameFeature plugin names"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_user_facing_experience"),
		TEXT("Describe a ULyraUserFacingExperienceDefinition hosting contract without logging credentials"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeUserFacingExperience),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("user_facing_experience_path"), TEXT("Lyra user-facing experience asset path"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_user_facing_experience"),
		TEXT("Validate a ULyraUserFacingExperienceDefinition map/experience/session hosting contract"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateUserFacingExperience),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("user_facing_experience_path"), TEXT("Lyra user-facing experience asset path"))
			.Optional(TEXT("require_resolved_primary_assets"), TEXT("bool"), TEXT("Treat unresolved MapID/ExperienceID primary asset paths as errors"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_map_default_experience"),
		TEXT("Validate a map UWorld's LyraWorldSettings DefaultGameplayExperience without mutating the map"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateMapDefaultExperience),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("map_path"), TEXT("Map package/object path or Map primary asset id"))
			.Optional(TEXT("expected_experience_id"), TEXT("string"), TEXT("Expected LyraExperienceDefinition primary asset id, e.g. LyraExperienceDefinition:B_TagChase_Experience"))
			.Optional(TEXT("require_default_experience"), TEXT("bool"), TEXT("Treat missing DefaultGameplayExperience as an error"), TEXT("true"))
			.Optional(TEXT("require_lyra_world_settings"), TEXT("bool"), TEXT("Treat non-Lyra WorldSettings as an error"), TEXT("true"))
			.Optional(TEXT("require_matching_experience"), TEXT("bool"), TEXT("When expected_experience_id is supplied, treat mismatches as errors"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_user_facing_map_reachability"),
		TEXT("Validate that a ULyraUserFacingExperienceDefinition MapID resolves to a map and optionally matches that map's DefaultGameplayExperience"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateUserFacingMapReachability),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("user_facing_experience_path"), TEXT("Lyra user-facing experience asset path"))
			.Optional(TEXT("require_resolved_primary_assets"), TEXT("bool"), TEXT("Treat unresolved MapID/ExperienceID primary asset paths as errors"), TEXT("false"))
			.Optional(TEXT("require_map_default_experience"), TEXT("bool"), TEXT("Treat missing map DefaultGameplayExperience as an error"), TEXT("false"))
			.Optional(TEXT("require_lyra_world_settings"), TEXT("bool"), TEXT("Treat non-Lyra WorldSettings as an error"), TEXT("true"))
			.Optional(TEXT("require_matching_map_default_experience"), TEXT("bool"), TEXT("Require map DefaultGameplayExperience to match the user-facing ExperienceID"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_gameplay_tag_domain"),
		TEXT("Describe a GameplayTag domain such as GamePhase, including root registration, children, source metadata, and comments"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeGameplayTagDomain),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("root_tag"), TEXT("string"), TEXT("GameplayTag root to inspect"), TEXT("GamePhase"))
			.Optional(TEXT("include_children"), TEXT("boolean"), TEXT("Include child tags under root_tag"), TEXT("true"))
			.Optional(TEXT("max_tags"), TEXT("integer"), TEXT("Maximum child tags to return before truncation"), TEXT("256"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_game_phase_flow"),
		TEXT("Validate Lyra game phase ability classes and their GamePhaseTag contract without mutating runtime or assets"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateGamePhaseFlow),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("root_tag"), TEXT("string"), TEXT("GameplayTag root expected for phase tags"), TEXT("GamePhase"))
			.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Optional content root such as /Game or /SpeedCore for Blueprint phase ability discovery"))
			.Optional(TEXT("phase_ability_paths"), TEXT("array"), TEXT("Optional explicit LyraGamePhaseAbility class or Blueprint asset paths"))
			.Optional(TEXT("expected_phase_tags"), TEXT("array"), TEXT("Optional exact phase tags expected to be backed by a phase ability"))
			.Optional(TEXT("disallow_duplicate_tags"), TEXT("boolean"), TEXT("Treat duplicate exact phase tags as errors instead of a reported ambiguity"), TEXT("false"))
			.Optional(TEXT("max_assets"), TEXT("integer"), TEXT("Maximum discovered Blueprint phase ability assets to inspect"), TEXT("256"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_team_setup"),
		TEXT("Describe reflected Lyra team creation defaults, public/private info classes, and TeamsToCreate map without runtime mutation"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeTeamSetup),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("team_creation_component_class"), TEXT("string"), TEXT("LyraTeamCreationComponent class or Blueprint class path"), MonolithLyra::LyraTeamCreationComponentClassPath)
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_inventory_item"),
		TEXT("Describe a Lyra inventory item definition CDO, display name, and instanced fragments"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeInventoryItem),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("item_definition_path"), TEXT("Lyra inventory item definition class or Blueprint path"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_equipment_definition"),
		TEXT("Describe a Lyra equipment definition CDO: instance type, granted ability sets, and spawned actors"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeEquipmentDefinition),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("equipment_definition_path"), TEXT("Lyra equipment definition class or Blueprint path"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_weapon_definition"),
		TEXT("Describe a weapon-like Lyra inventory item by following its EquippableItem fragment to an equipment definition and weapon instance type"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeWeaponDefinition),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("item_definition_path"), TEXT("Lyra inventory item definition class or Blueprint path"))
			.Optional(TEXT("require_equippable_fragment"), TEXT("boolean"), TEXT("Treat missing InventoryFragment_EquippableItem as ok=false"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_pawn_initialization_graph"),
		TEXT("Describe a Lyra PawnData initialization graph: PawnClass, AbilitySets, InputConfig, tag mapping, and camera mode"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribePawnInitializationGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("pawn_data_path"), TEXT("Lyra PawnData asset path"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_pawn_data_contract"),
		TEXT("Validate a Lyra PawnData contract with explicit required fields and expected pawn class checks"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidatePawnDataContract),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("pawn_data_path"), TEXT("Lyra PawnData asset path"))
			.Optional(TEXT("require_pawn_class"), TEXT("boolean"), TEXT("Treat missing PawnClass as an error"), TEXT("true"))
			.Optional(TEXT("require_ability_sets"), TEXT("boolean"), TEXT("Treat empty AbilitySets as an error"), TEXT("false"))
			.Optional(TEXT("require_input_config"), TEXT("boolean"), TEXT("Treat missing InputConfig as an error"), TEXT("false"))
			.Optional(TEXT("require_default_camera_mode"), TEXT("boolean"), TEXT("Treat missing DefaultCameraMode as an error"), TEXT("false"))
			.Optional(TEXT("expected_pawn_class"), TEXT("string"), TEXT("Expected pawn class path; actual PawnClass must match or derive from it"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("describe_character_part_graph"),
		TEXT("Describe Lyra cosmetic character-part reflected classes, developer settings, and optional actor part classes"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::DescribeCharacterPartGraph),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("part_classes"), TEXT("array"), TEXT("Optional character part actor class paths to describe"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("validate_character_part_assets"),
		TEXT("Validate supplied Lyra character part actor classes as loadable, concrete AActor classes"),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::ValidateCharacterPartAssets),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("part_classes"), TEXT("array"), TEXT("Character part actor class paths to validate"))
			.Optional(TEXT("require_non_empty"), TEXT("boolean"), TEXT("Treat an empty part_classes list as an error"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("set_experience_defaults"),
		TEXT("Set Lyra Experience reflected defaults: DefaultPawnData, ActionSets, and GameFeaturesToEnable. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::SetExperienceDefaults),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("experience_path"), TEXT("Lyra experience package/object/class path"))
			.Optional(TEXT("default_pawn_data"), TEXT("string"), TEXT("DefaultPawnData asset object path"))
			.Optional(TEXT("action_sets"), TEXT("array"), TEXT("Replacement ActionSets asset paths"))
			.Optional(TEXT("game_features_to_enable"), TEXT("array"), TEXT("Replacement GameFeaturesToEnable plugin names"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview reflected writes without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for mutation when dry_run=false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the owning package after a clean committed write"), TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("Treat unknown or rejected reflected fields as apply-blocking errors"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("add_experience_component_entry"),
		TEXT("Idempotently add or update one GameFeatureAction_AddComponents ComponentList entry on a Lyra Experience or explicit ExperienceActionSet. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::AddExperienceComponentEntry),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("experience_path"), TEXT("string"), TEXT("Lyra Experience package/object path; mutually exclusive with action_set_path"))
			.Optional(TEXT("action_set_path"), TEXT("string"), TEXT("Explicit Lyra ExperienceActionSet package/object path; mutually exclusive with experience_path"))
			.Required(TEXT("actor_class"), TEXT("string"), TEXT("Actor subclass that should receive the component"))
			.Required(TEXT("component_class"), TEXT("string"), TEXT("ActorComponent subclass to add through ModularGameplay"))
			.Optional(TEXT("action_name"), TEXT("string"), TEXT("Exact instanced AddComponents action object name to reuse or create"))
			.Optional(TEXT("client_component"), TEXT("boolean"), TEXT("Request the component on clients"), TEXT("true"))
			.Optional(TEXT("server_component"), TEXT("boolean"), TEXT("Request the component on servers"), TEXT("true"))
			.Optional(TEXT("addition_flags"), TEXT("integer"), TEXT("EGameFrameworkAddComponentFlags bitmask in the uint8 range"), TEXT("0"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview action creation and ComponentList add/update without mutation"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for mutation when dry_run=false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the owner package after a confirmed change"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("remove_experience_component_entry"),
		TEXT("Remove reflected GameFeatureAction_AddComponents ComponentList entries from a Lyra Experience or ActionSet. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::RemoveExperienceComponentEntry),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("experience_path"), TEXT("string"), TEXT("Lyra experience package/object/class path to scan"))
			.Optional(TEXT("action_set_path"), TEXT("string"), TEXT("Specific Lyra ExperienceActionSet path to scan instead of or in addition to experience_path"))
			.Optional(TEXT("action_index"), TEXT("integer"), TEXT("Optional Actions array index filter"))
			.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional instanced action object name filter"))
			.Optional(TEXT("actor_class"), TEXT("string"), TEXT("Optional ActorClass soft class path filter"))
			.Optional(TEXT("component_class"), TEXT("string"), TEXT("Optional ComponentClass soft class path filter"))
			.Optional(TEXT("component_index"), TEXT("integer"), TEXT("Optional ComponentList index filter"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview removals without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for mutation when dry_run=false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save affected owner packages after a clean committed write"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("lyra"), TEXT("set_user_facing_experience"),
		TEXT("Set ULyraUserFacingExperienceDefinition map/experience/session/UI fields. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithLyraActions::SetUserFacingExperience),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("user_facing_experience_path"), TEXT("Lyra user-facing experience asset path"))
			.Optional(TEXT("map_id"), TEXT("string"), TEXT("Map primary asset id, e.g. Map:/Game/Maps/L_Playground"))
			.Optional(TEXT("experience_id"), TEXT("string"), TEXT("LyraExperienceDefinition primary asset id, e.g. LyraExperienceDefinition:B_Experience"))
			.Optional(TEXT("extra_args"), TEXT("object"), TEXT("Replacement ExtraArgs map"))
			.Optional(TEXT("tile_title"), TEXT("string"), TEXT("TileTitle FText import text or plain text"))
			.Optional(TEXT("tile_subtitle"), TEXT("string"), TEXT("TileSubTitle FText import text or plain text"))
			.Optional(TEXT("tile_description"), TEXT("string"), TEXT("TileDescription FText import text or plain text"))
			.Optional(TEXT("tile_icon"), TEXT("string"), TEXT("TileIcon Texture2D object path"))
			.Optional(TEXT("loading_screen_widget"), TEXT("string"), TEXT("LoadingScreenWidget soft class path"))
			.Optional(TEXT("is_default_experience"), TEXT("boolean"), TEXT("Set bIsDefaultExperience"))
			.Optional(TEXT("show_in_front_end"), TEXT("boolean"), TEXT("Set bShowInFrontEnd"))
			.Optional(TEXT("record_replay"), TEXT("boolean"), TEXT("Set bRecordReplay"))
			.Optional(TEXT("max_player_count"), TEXT("integer"), TEXT("Set MaxPlayerCount"))
			.Optional(TEXT("session_mode"), TEXT("string"), TEXT("SessionMode enum token: Offline, LAN, or Online"))
			.Optional(TEXT("use_lobbies"), TEXT("boolean"), TEXT("Set bUseLobbies"))
			.Optional(TEXT("use_lobbies_voice_chat"), TEXT("boolean"), TEXT("Set bUseLobbiesVoiceChat"))
			.Optional(TEXT("use_presence"), TEXT("boolean"), TEXT("Set bUsePresence"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Preview reflected writes without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required true for mutation when dry_run=false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Save the owning package after a clean committed write"), TEXT("false"))
			.Optional(TEXT("strict"), TEXT("boolean"), TEXT("Treat unknown or rejected reflected fields as apply-blocking errors"), TEXT("true"))
			.Build());

	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_experience_graph"),
		{ TEXT("Lyra Experience"), TEXT("DefaultPawnData"), TEXT("ActionSets"), TEXT("GameFeaturesToEnable") },
		{ TEXT("inspect Lyra experience"), TEXT("experience graph") },
		{ TEXT("describe /ShooterCore/Experiences/B_ShooterGame_Elimination") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_experience_bundle"),
		{ TEXT("Lyra Experience validation"), TEXT("PawnData"), TEXT("ActionSet"), TEXT("GameFeature") },
		{ TEXT("validate experience"), TEXT("experience contract") },
		{ TEXT("validate that a Lyra experience has the expected pawn data and action set") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_user_facing_experience"),
		{ TEXT("Lyra UserFacingExperience"), TEXT("CommonSession"), TEXT("hosting"), TEXT("MapID"), TEXT("ExperienceID") },
		{ TEXT("validate hosting contract"), TEXT("validate front end session") },
		{ TEXT("validate a Lyra user-facing experience before hosting online") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_map_default_experience"),
		{ TEXT("Lyra map validation"), TEXT("ALyraWorldSettings"), TEXT("DefaultGameplayExperience"), TEXT("Map primary asset") },
		{ TEXT("validate map default experience"), TEXT("check world settings experience") },
		{ TEXT("validate that a map loads and its LyraWorldSettings DefaultGameplayExperience resolves") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_user_facing_map_reachability"),
		{ TEXT("Lyra UserFacingExperience"), TEXT("MapID"), TEXT("ExperienceID"), TEXT("ALyraWorldSettings"), TEXT("playlist reachability") },
		{ TEXT("validate playlist map"), TEXT("check user facing map reachability") },
		{ TEXT("validate that a front-end playlist MapID loads and optionally matches the map default experience") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_gameplay_tag_domain"),
		{ TEXT("GameplayTag"), TEXT("GamePhase"), TEXT("tag domain"), TEXT("tag source") },
		{ TEXT("describe game phase tags"), TEXT("list gameplay tag children") },
		{ TEXT("describe the GamePhase tag domain and tag sources") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_game_phase_flow"),
		{ TEXT("Lyra GamePhase"), TEXT("ULyraGamePhaseAbility"), TEXT("GamePhaseTag"), TEXT("phase flow") },
		{ TEXT("validate game phase flow"), TEXT("check phase ability tags") },
		{ TEXT("validate that LyraGamePhaseAbility classes have registered GamePhase tags") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_team_setup"),
		{ TEXT("Lyra teams"), TEXT("ULyraTeamCreationComponent"), TEXT("TeamsToCreate") },
		{ TEXT("describe team setup"), TEXT("inspect Lyra teams") },
		{ TEXT("describe Lyra team creation component defaults") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_inventory_item"),
		{ TEXT("Lyra inventory"), TEXT("ULyraInventoryItemDefinition"), TEXT("fragments") },
		{ TEXT("describe inventory item"), TEXT("inspect item fragments") },
		{ TEXT("describe a Lyra inventory item and its fragments") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_equipment_definition"),
		{ TEXT("Lyra equipment"), TEXT("ULyraEquipmentDefinition"), TEXT("AbilitySetsToGrant"), TEXT("ActorsToSpawn") },
		{ TEXT("describe equipment"), TEXT("inspect equipment definition") },
		{ TEXT("describe a Lyra equipment definition") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_weapon_definition"),
		{ TEXT("Lyra weapon"), TEXT("InventoryFragment_EquippableItem"), TEXT("ULyraWeaponInstance") },
		{ TEXT("describe weapon"), TEXT("inspect weapon item") },
		{ TEXT("describe a Lyra weapon item by following its equippable fragment") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_pawn_initialization_graph"),
		{ TEXT("Lyra PawnData"), TEXT("PawnClass"), TEXT("InputConfig"), TEXT("AbilitySets") },
		{ TEXT("describe pawn data"), TEXT("inspect pawn initialization") },
		{ TEXT("describe a Lyra PawnData initialization graph") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_pawn_data_contract"),
		{ TEXT("Lyra PawnData validation"), TEXT("PawnClass"), TEXT("InputConfig"), TEXT("CameraMode") },
		{ TEXT("validate pawn data"), TEXT("check pawn initialization") },
		{ TEXT("validate a Lyra PawnData contract") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("describe_character_part_graph"),
		{ TEXT("Lyra cosmetics"), TEXT("character parts"), TEXT("ULyraPawnComponent_CharacterParts") },
		{ TEXT("describe character parts"), TEXT("inspect cosmetics graph") },
		{ TEXT("describe Lyra character-part classes and optional part actors") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("validate_character_part_assets"),
		{ TEXT("Lyra character part validation"), TEXT("AActor"), TEXT("cosmetics") },
		{ TEXT("validate character parts"), TEXT("check cosmetic actor classes") },
		{ TEXT("validate Lyra character part actor class paths") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("set_experience_defaults"),
		{ TEXT("Lyra Experience write"), TEXT("DefaultPawnData"), TEXT("ActionSets"), TEXT("GameFeaturesToEnable") },
		{ TEXT("set experience defaults"), TEXT("write Lyra experience") },
		{ TEXT("set DefaultPawnData and ActionSets on a Lyra Experience with dry_run first") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("add_experience_component_entry"),
		{ TEXT("Lyra Experience component authoring"), TEXT("GameFeatureAction_AddComponents"), TEXT("ComponentList"), TEXT("ExperienceActionSet") },
		{ TEXT("add experience component entry"), TEXT("attach component to LyraGameState") },
		{ TEXT("add or update an ActorClass and ComponentClass pair on a Lyra Experience ActionSet with dry_run first") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("remove_experience_component_entry"),
		{ TEXT("Lyra Experience cleanup"), TEXT("GameFeatureAction_AddComponents"), TEXT("ComponentList") },
		{ TEXT("remove component entry"), TEXT("cleanup add components action") },
		{ TEXT("remove a reflected ComponentList entry from a Lyra Experience ActionSet") });
	Registry.SetActionSearchMetadata(TEXT("lyra"), TEXT("set_user_facing_experience"),
		{ TEXT("Lyra UserFacingExperience write"), TEXT("MapID"), TEXT("ExperienceID"), TEXT("SessionMode") },
		{ TEXT("set user facing experience"), TEXT("configure hosting tile") },
		{ TEXT("set MapID, ExperienceID, LAN/Online session fields for a front-end playlist") });

	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_experience_graph"),
		TEXT("unreal-lyra"),
		{ TEXT("A Lyra Experience asset or generated class path is required") },
		{ TEXT("Bounded graph JSON with pawn data, action sets, actions, and feature plugin names") },
		{ TEXT("lyra.validate_experience_bundle"), TEXT("gamefeatures.describe_action_set") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_experience_bundle"),
		TEXT("unreal-lyra"),
		{ TEXT("A Lyra Experience asset or generated class path is required") },
		{ TEXT("ok flag, checks array, warnings array, and graph payload") },
		{ TEXT("gamefeatures.add_game_feature_data_widgets"), TEXT("editor.set_world_settings_property") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_user_facing_experience"),
		TEXT("unreal-lyra"),
		{ TEXT("A ULyraUserFacingExperienceDefinition asset path is required") },
		{ TEXT("MapID, ExperienceID, session mode, lobby/presence flags, and UI metadata") },
		{ TEXT("lyra.validate_user_facing_experience") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_user_facing_experience"),
		TEXT("unreal-lyra"),
		{ TEXT("A ULyraUserFacingExperienceDefinition asset path is required") },
		{ TEXT("ok flag, checks array, warnings array, and user-facing experience payload") },
		{ TEXT("lyra.validate_user_facing_map_reachability"), TEXT("online.validate_common_session_schema") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_map_default_experience"),
		TEXT("unreal-lyra"),
		{ TEXT("A map package/object path or Map primary asset id is required") },
		{ TEXT("ok flag, checks array, warnings array, map payload, world settings class, and DefaultGameplayExperience primary asset id") },
		{ TEXT("lyra.validate_user_facing_map_reachability"), TEXT("editor.set_world_settings_property") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_user_facing_map_reachability"),
		TEXT("unreal-lyra"),
		{ TEXT("A ULyraUserFacingExperienceDefinition asset path is required") },
		{ TEXT("ok flag, checks array, warnings array, user-facing experience payload, and nested map contract") },
		{ TEXT("lyra.validate_user_facing_experience"), TEXT("online.validate_user_facing_session") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_gameplay_tag_domain"),
		TEXT("unreal-lyra"),
		{ TEXT("GameplayTags module must be available; root_tag defaults to GamePhase") },
		{ TEXT("root tag registration, source metadata, child tag rows, and truncation status") },
		{ TEXT("lyra.validate_game_phase_flow") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_game_phase_flow"),
		TEXT("unreal-lyra"),
		{ TEXT("LyraGamePhaseAbility and LyraGamePhaseSubsystem reflected classes must be loadable") },
		{ TEXT("ok flag, checks array, phase ability summaries, duplicate tag groups, expected-tag gaps, and tag-domain payload") },
		{ TEXT("lyra.describe_gameplay_tag_domain"), TEXT("gameplay_message.trace_channel_usage") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_team_setup"),
		TEXT("unreal-lyra"),
		{ TEXT("LyraTeamCreationComponent reflected class must be loadable") },
		{ TEXT("team creation CDO defaults, TeamsToCreate, and public/private info classes") },
		{ TEXT("modular.validate_add_component_targets") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_inventory_item"),
		TEXT("unreal-lyra"),
		{ TEXT("A LyraInventoryItemDefinition class or Blueprint path is required") },
		{ TEXT("display name, fragment rows, known fragment properties, and fragment count") },
		{ TEXT("lyra.describe_weapon_definition"), TEXT("lyra.describe_equipment_definition") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_equipment_definition"),
		TEXT("unreal-lyra"),
		{ TEXT("A LyraEquipmentDefinition class or Blueprint path is required") },
		{ TEXT("instance type, ability sets to grant, actors to spawn, and counts") },
		{ TEXT("gas.validate_ability_set") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_weapon_definition"),
		TEXT("unreal-lyra"),
		{ TEXT("A LyraInventoryItemDefinition with an equippable fragment is required") },
		{ TEXT("inventory item summary, equipment summary, weapon-instance compatibility, and checks") },
		{ TEXT("lyra.describe_inventory_item"), TEXT("lyra.describe_equipment_definition") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_pawn_initialization_graph"),
		TEXT("unreal-lyra"),
		{ TEXT("A LyraPawnData asset path is required") },
		{ TEXT("PawnClass, AbilitySets, TagRelationshipMapping, InputConfig, DefaultCameraMode") },
		{ TEXT("lyra.validate_pawn_data_contract") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_pawn_data_contract"),
		TEXT("unreal-lyra"),
		{ TEXT("A LyraPawnData asset path is required") },
		{ TEXT("ok flag, checks array, issues, and pawn data graph") },
		{ TEXT("lyra.describe_pawn_initialization_graph"), TEXT("input.validate_mappings") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("describe_character_part_graph"),
		TEXT("unreal-lyra"),
		{ TEXT("Lyra character-part reflected classes should be loadable; optional part class list may be supplied") },
		{ TEXT("class availability, developer settings defaults, optional part actor rows") },
		{ TEXT("lyra.validate_character_part_assets") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("validate_character_part_assets"),
		TEXT("unreal-lyra"),
		{ TEXT("part_classes must be an array when supplied") },
		{ TEXT("ok flag, checks array, and part actor class rows") },
		{ TEXT("lyra.describe_character_part_graph") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("set_experience_defaults"),
		TEXT("unreal-lyra"),
		{ TEXT("dry_run=true or confirm=true is required"), TEXT("A Lyra Experience path is required") },
		{ TEXT("bulk-fill field writes, current graph, write plan, and save status") },
		{ TEXT("lyra.validate_experience_bundle"), TEXT("gamefeatures.describe_action_set") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("add_experience_component_entry"),
		TEXT("unreal-lyra"),
		{ TEXT("dry_run=true or confirm=true is required"), TEXT("Exactly one of experience_path or action_set_path is required"), TEXT("actor_class and component_class must be loadable subclasses") },
		{ TEXT("action create/reuse plan, component add/update plan, indices, counts, and save status") },
		{ TEXT("lyra.validate_experience_bundle"), TEXT("gamefeatures.describe_action_set"), TEXT("lyra.remove_experience_component_entry") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("remove_experience_component_entry"),
		TEXT("unreal-lyra"),
		{ TEXT("dry_run=true or confirm=true is required"), TEXT("At least one selector must identify ComponentList entries") },
		{ TEXT("candidate entries, removed count, save status, and owner paths") },
		{ TEXT("lyra.validate_experience_bundle"), TEXT("gamefeatures.describe_action_set") });
	Registry.SetActionPlanningMetadata(TEXT("lyra"), TEXT("set_user_facing_experience"),
		TEXT("unreal-lyra"),
		{ TEXT("dry_run=true or confirm=true is required"), TEXT("A ULyraUserFacingExperienceDefinition asset path is required") },
		{ TEXT("bulk-fill field writes, current summary, write plan, and save status") },
		{ TEXT("lyra.validate_user_facing_experience"), TEXT("online.validate_common_session_schema") });
}

FMonolithActionResult FMonolithLyraActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("mode"), TEXT("semantic_validation_and_guarded_writes"));
	Result->SetBoolField(TEXT("namespace_registered"), FMonolithToolRegistry::Get().HasNamespace(TEXT("lyra")));
	Result->SetStringField(TEXT("sample_utc"), FDateTime::UtcNow().ToIso8601());

	TArray<TSharedPtr<FJsonValue>> Types;
	const TCHAR* TypePaths[] =
	{
		MonolithLyra::LyraExperienceDefinitionClassPath,
		MonolithLyra::LyraExperienceActionSetClassPath,
		MonolithLyra::LyraUserFacingExperienceClassPath,
		MonolithLyra::LyraGamePhaseAbilityClassPath,
		MonolithLyra::LyraGamePhaseSubsystemClassPath,
		MonolithLyra::LyraTeamCreationComponentClassPath,
		MonolithLyra::LyraInventoryItemDefinitionClassPath,
		MonolithLyra::LyraInventoryItemFragmentClassPath,
		MonolithLyra::LyraEquipmentDefinitionClassPath,
		MonolithLyra::LyraWeaponInstanceClassPath,
		MonolithLyra::LyraPawnDataClassPath,
		MonolithLyra::LyraControllerCharacterPartsClassPath,
		MonolithLyra::LyraPawnCharacterPartsClassPath,
		MonolithLyra::LyraCosmeticDeveloperSettingsClassPath
	};
	for (const TCHAR* TypePath : TypePaths)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_path"), TypePath);
		Row->SetBoolField(TEXT("loaded"), StaticLoadClass(UObject::StaticClass(), nullptr, TypePath) != nullptr);
		Types.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("reflected_types"), Types);

	Result->SetArrayField(TEXT("current_actions"), MonolithLyra::StringArrayToJson({
		TEXT("lyra.get_status"),
		TEXT("lyra.describe_experience_graph"),
		TEXT("lyra.validate_experience_bundle"),
		TEXT("lyra.describe_user_facing_experience"),
		TEXT("lyra.validate_user_facing_experience"),
		TEXT("lyra.validate_map_default_experience"),
		TEXT("lyra.validate_user_facing_map_reachability"),
		TEXT("lyra.describe_gameplay_tag_domain"),
		TEXT("lyra.validate_game_phase_flow"),
		TEXT("lyra.describe_team_setup"),
		TEXT("lyra.describe_inventory_item"),
		TEXT("lyra.describe_equipment_definition"),
		TEXT("lyra.describe_weapon_definition"),
		TEXT("lyra.describe_pawn_initialization_graph"),
		TEXT("lyra.validate_pawn_data_contract"),
		TEXT("lyra.describe_character_part_graph"),
		TEXT("lyra.validate_character_part_assets"),
		TEXT("lyra.set_experience_defaults"),
		TEXT("lyra.add_experience_component_entry"),
		TEXT("lyra.remove_experience_component_entry"),
		TEXT("lyra.set_user_facing_experience")
	}));
	Result->SetArrayField(TEXT("future_actions"), TArray<TSharedPtr<FJsonValue>>());
	Result->SetArrayField(TEXT("notes"), MonolithLyra::StringArrayToJson({
		TEXT("This slice uses reflection only and does not hard-link LyraGame headers."),
		TEXT("No runtime CommonGame or PrimaryGameLayout code is modified by these actions.")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeExperienceGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString ExperiencePath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("experience_path"), ExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(ExperiencePath, MonolithLyra::LyraExperienceDefinitionClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetObjectField(TEXT("experience_graph"), MonolithLyra::BuildExperienceGraph(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateExperienceBundle(const TSharedPtr<FJsonObject>& Params)
{
	FString ExperiencePath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("experience_path"), ExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bRequireDefaultPawnData = true;
	bool bRequireActionSets = false;
	bool bValidateDefaultPawnData = false;
	bool bRequirePawnClass = true;
	bool bRequirePawnAbilitySets = false;
	bool bRequirePawnInputConfig = false;
	bool bRequireDefaultCameraMode = false;
	bool bValidateActionSets = true;
	bool bRequireActionSetActions = false;
	bool bDisallowNullActions = true;
	bool bValidateActionClasses = true;
	bool bRequireActionSetGameFeatures = false;
	bool bValidateGameFeaturePlugins = false;
	if (!MonolithLyra::TryReadBoolParam(Params, TEXT("require_default_pawn_data"), bRequireDefaultPawnData, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_action_sets"), bRequireActionSets, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("validate_default_pawn_data"), bValidateDefaultPawnData, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_pawn_class"), bRequirePawnClass, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_pawn_ability_sets"), bRequirePawnAbilitySets, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_pawn_input_config"), bRequirePawnInputConfig, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_default_camera_mode"), bRequireDefaultCameraMode, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("validate_action_sets"), bValidateActionSets, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_action_set_actions"), bRequireActionSetActions, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("disallow_null_actions"), bDisallowNullActions, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("validate_action_classes"), bValidateActionClasses, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_action_set_game_features"), bRequireActionSetGameFeatures, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("validate_game_feature_plugins"), bValidateGameFeaturePlugins, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FString ExpectedPawnData;
	Params->TryGetStringField(TEXT("expected_pawn_data"), ExpectedPawnData);
	ExpectedPawnData.TrimStartAndEndInline();

	TArray<FString> ExpectedActionSets;
	TArray<FString> ExpectedGameFeatures;
	if (!MonolithLyra::TryReadStringArrayParam(Params, TEXT("expected_action_sets"), ExpectedActionSets, Error)
		|| !MonolithLyra::TryReadStringArrayParam(Params, TEXT("expected_game_features"), ExpectedGameFeatures, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> GameFeaturePluginRows;
	TSharedPtr<FJsonObject> Graph;
	TSharedPtr<FJsonObject> DefaultPawnDataSummary;

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(ExperiencePath, MonolithLyra::LyraExperienceDefinitionClassPath, Resolved, Error))
	{
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), false, TEXT("error"), Error);
	}
	else
	{
		Graph = MonolithLyra::BuildExperienceGraph(Resolved);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), true, TEXT("error"), Resolved.ResolvedPath);

		const FString DefaultPawnDataPath = MonolithLyra::GetObjectPropertyPath(Resolved.Object, TEXT("DefaultPawnData"));
		UObject* DefaultPawnDataObject = MonolithLyra::GetObjectPropertyValue(Resolved.Object, TEXT("DefaultPawnData"));
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("default_pawn_data"),
			!bRequireDefaultPawnData || !DefaultPawnDataPath.IsEmpty(),
			TEXT("error"),
			DefaultPawnDataPath.IsEmpty() ? TEXT("DefaultPawnData is not set") : DefaultPawnDataPath);

		if (!ExpectedPawnData.IsEmpty())
		{
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("expected_pawn_data"),
				MonolithLyra::NormalizeObjectPathForCompare(DefaultPawnDataPath).Equals(MonolithLyra::NormalizeObjectPathForCompare(ExpectedPawnData), ESearchCase::IgnoreCase),
				TEXT("error"),
				FString::Printf(TEXT("expected=%s actual=%s"), *ExpectedPawnData, *DefaultPawnDataPath));
		}

		if (bValidateDefaultPawnData && DefaultPawnDataObject)
		{
			UClass* PawnDataBase = MonolithLyra::LoadExpectedClass(MonolithLyra::LyraPawnDataClassPath);
			const bool bPawnDataTypeOk = PawnDataBase && DefaultPawnDataObject->GetClass() && DefaultPawnDataObject->GetClass()->IsChildOf(PawnDataBase);
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("default_pawn_data_is_lyra_pawn_data"),
				bPawnDataTypeOk,
				TEXT("error"),
				DefaultPawnDataObject->GetClass() ? DefaultPawnDataObject->GetClass()->GetPathName() : TEXT("DefaultPawnData class is missing"));
			MonolithLyra::AddPawnDataContractChecks(
				Checks,
				bOk,
				DefaultPawnDataObject,
				bRequirePawnClass,
				bRequirePawnAbilitySets,
				bRequirePawnInputConfig,
				bRequireDefaultCameraMode,
				TEXT("default_pawn_data"));

			MonolithLyra::FResolvedLyraObject PawnDataResolved;
			PawnDataResolved.Object = DefaultPawnDataObject;
			PawnDataResolved.AssetForSave = DefaultPawnDataObject;
			PawnDataResolved.ExpectedClass = PawnDataBase;
			PawnDataResolved.InputPath = DefaultPawnDataPath;
			PawnDataResolved.ResolvedPath = DefaultPawnDataObject->GetPathName();
			PawnDataResolved.SaveTargetPath = DefaultPawnDataObject->GetPathName();
			PawnDataResolved.SourceKind = TEXT("experience_default_pawn_data");
			DefaultPawnDataSummary = MonolithLyra::BuildPawnDataSummary(PawnDataResolved);
		}
		else if (bValidateDefaultPawnData && !DefaultPawnDataObject)
		{
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("default_pawn_data_validated"),
				!bRequireDefaultPawnData,
				bRequireDefaultPawnData ? TEXT("error") : TEXT("warning"),
				TEXT("DefaultPawnData is not set; nested PawnData validation skipped"));
		}

		const TArray<UObject*> ActionSets = MonolithLyra::GetObjectArrayProperty(Resolved.Object, TEXT("ActionSets"));
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("action_sets_present"),
			!bRequireActionSets || ActionSets.Num() > 0,
			TEXT("error"),
			FString::Printf(TEXT("%d action set entries"), ActionSets.Num()));

		TArray<FString> ActualActionSetPaths;
		for (UObject* ActionSet : ActionSets)
		{
			if (ActionSet)
			{
				ActualActionSetPaths.Add(ActionSet->GetPathName());
			}
		}

		int32 NullActionSetCount = 0;
		for (UObject* ActionSet : ActionSets)
		{
			if (!ActionSet)
			{
				++NullActionSetCount;
			}
		}
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("action_sets_non_null"),
			NullActionSetCount == 0,
			TEXT("error"),
			NullActionSetCount == 0 ? TEXT("All ActionSets entries resolve") : FString::Printf(TEXT("%d null ActionSets entries"), NullActionSetCount));

		for (const FString& ExpectedActionSet : ExpectedActionSets)
		{
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("expected_action_set"),
				MonolithLyra::ContainsNormalizedPath(ActualActionSetPaths, ExpectedActionSet),
				TEXT("error"),
				ExpectedActionSet);
		}

		if (bValidateActionSets)
		{
			for (int32 ActionSetIndex = 0; ActionSetIndex < ActionSets.Num(); ++ActionSetIndex)
			{
				MonolithLyra::AddActionSetContractChecks(
					Checks,
					bOk,
					ActionSets[ActionSetIndex],
					ActionSetIndex,
					bRequireActionSetActions,
					bDisallowNullActions,
					bValidateActionClasses,
					bRequireActionSetGameFeatures);
			}
		}

		TArray<FString> ActualGameFeaturesRaw = MonolithLyra::GetStringArrayProperty(Resolved.Object, TEXT("GameFeaturesToEnable"));
		TArray<FString> ActualGameFeatures = ActualGameFeaturesRaw;
		for (UObject* ActionSet : ActionSets)
		{
			for (const FString& GameFeature : MonolithLyra::GetStringArrayProperty(ActionSet, TEXT("GameFeaturesToEnable")))
			{
				ActualGameFeaturesRaw.Add(GameFeature);
				ActualGameFeatures.AddUnique(GameFeature);
			}
		}

		TSet<FString> SeenGameFeatureNames;
		TArray<FString> DuplicateGameFeatures;
		for (const FString& GameFeature : ActualGameFeaturesRaw)
		{
			const FString Normalized = GameFeature.ToLower();
			if (SeenGameFeatureNames.Contains(Normalized))
			{
				DuplicateGameFeatures.AddUnique(GameFeature);
			}
			SeenGameFeatureNames.Add(Normalized);
		}
		if (!DuplicateGameFeatures.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Duplicate GameFeaturesToEnable entries found: %s"), *FString::Join(DuplicateGameFeatures, TEXT(", ")))));
		}

		for (const FString& ExpectedGameFeature : ExpectedGameFeatures)
		{
			const bool bFound = ActualGameFeatures.ContainsByPredicate([&ExpectedGameFeature](const FString& Actual)
			{
				return Actual.Equals(ExpectedGameFeature, ESearchCase::IgnoreCase);
			});
			MonolithLyra::AddCheck(Checks, bOk, TEXT("expected_game_feature"), bFound, TEXT("error"), ExpectedGameFeature);
		}

		if (bValidateGameFeaturePlugins)
		{
			for (const FString& GameFeature : ActualGameFeatures)
			{
				TSharedPtr<FJsonObject> PluginRow = MonolithLyra::BuildGameFeaturePluginSummary(GameFeature);
				const bool bPluginFound = PluginRow->GetBoolField(TEXT("found"));
				const bool bPluginEnabled = bPluginFound && PluginRow->GetBoolField(TEXT("enabled"));
				MonolithLyra::AddCheck(
					Checks,
					bOk,
					TEXT("game_feature_plugin_enabled"),
					bPluginEnabled,
					TEXT("error"),
					GameFeature);
				GameFeaturePluginRows.Add(MakeShared<FJsonValueObject>(PluginRow));
			}
		}

		if (ActualGameFeatures.IsEmpty())
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("Experience and composed ActionSets declare no GameFeaturesToEnable entries.")));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("game_feature_plugins"), GameFeaturePluginRows);
	Result->SetBoolField(TEXT("validate_default_pawn_data"), bValidateDefaultPawnData);
	Result->SetBoolField(TEXT("validate_action_sets"), bValidateActionSets);
	Result->SetBoolField(TEXT("validate_game_feature_plugins"), bValidateGameFeaturePlugins);
	if (Graph.IsValid())
	{
		Result->SetObjectField(TEXT("experience_graph"), Graph);
	}
	if (DefaultPawnDataSummary.IsValid())
	{
		Result->SetObjectField(TEXT("default_pawn_data_contract"), DefaultPawnDataSummary);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeUserFacingExperience(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("user_facing_experience_path"), AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(AssetPath, MonolithLyra::LyraUserFacingExperienceClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetObjectField(TEXT("user_facing_experience"), MonolithLyra::BuildUserFacingSummary(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateUserFacingExperience(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("user_facing_experience_path"), AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bRequireResolvedPrimaryAssets = false;
	if (!MonolithLyra::TryReadBoolParam(Params, TEXT("require_resolved_primary_assets"), bRequireResolvedPrimaryAssets, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TSharedPtr<FJsonObject> Summary;

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(AssetPath, MonolithLyra::LyraUserFacingExperienceClassPath, Resolved, Error))
	{
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), false, TEXT("error"), Error);
	}
	else
	{
		Summary = MonolithLyra::BuildUserFacingSummary(Resolved);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), true, TEXT("error"), Resolved.ResolvedPath);

		FPrimaryAssetId MapId;
		FPrimaryAssetId ExperienceId;
		MonolithLyra::TryGetPrimaryAssetIdProperty(Resolved.Object, TEXT("MapID"), MapId);
		MonolithLyra::TryGetPrimaryAssetIdProperty(Resolved.Object, TEXT("ExperienceID"), ExperienceId);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("map_id_valid"), MapId.IsValid(), TEXT("error"), MapId.ToString());
		MonolithLyra::AddCheck(Checks, bOk, TEXT("experience_id_valid"), ExperienceId.IsValid(), TEXT("error"), ExperienceId.ToString());
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("experience_id_type"),
			!ExperienceId.IsValid() || ExperienceId.PrimaryAssetType.ToString().Equals(TEXT("LyraExperienceDefinition"), ESearchCase::IgnoreCase),
			TEXT("error"),
			ExperienceId.PrimaryAssetType.ToString());

		const FString MapResolvedPath = MapId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(MapId).ToString()
			: FString();
		const FString ExperienceResolvedPath = ExperienceId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(ExperienceId).ToString()
			: FString();
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("map_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !MapResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			MapResolvedPath.IsEmpty() ? TEXT("MapID did not resolve through AssetManager") : MapResolvedPath);
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("experience_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !ExperienceResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			ExperienceResolvedPath.IsEmpty() ? TEXT("ExperienceID did not resolve through AssetManager") : ExperienceResolvedPath);

		int32 MaxPlayerCount = 0;
		MonolithLyra::TryGetIntProperty(Resolved.Object, TEXT("MaxPlayerCount"), MaxPlayerCount);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("max_player_count_positive"), MaxPlayerCount > 0, TEXT("error"), FString::FromInt(MaxPlayerCount));

		const FString SessionMode = MonolithLyra::ExportPropertyText(Resolved.Object, TEXT("SessionMode"));
		bool bUseLobbies = false;
		bool bUseLobbiesVoiceChat = false;
		bool bUsePresence = false;
		MonolithLyra::TryGetBoolProperty(Resolved.Object, TEXT("bUseLobbies"), bUseLobbies);
		MonolithLyra::TryGetBoolProperty(Resolved.Object, TEXT("bUseLobbiesVoiceChat"), bUseLobbiesVoiceChat);
		MonolithLyra::TryGetBoolProperty(Resolved.Object, TEXT("bUsePresence"), bUsePresence);

		const bool bOnline = SessionMode.Contains(TEXT("Online"), ESearchCase::IgnoreCase);
		if (!bOnline && (bUseLobbies || bUseLobbiesVoiceChat || bUsePresence))
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("LAN/offline user-facing experiences ignore lobby, voice, and presence flags; consider clearing them for an unambiguous hosting contract.")));
		}
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("lobby_voice_requires_lobbies"),
			!bUseLobbiesVoiceChat || bUseLobbies,
			TEXT("error"),
			FString::Printf(TEXT("bUseLobbies=%s bUseLobbiesVoiceChat=%s"), bUseLobbies ? TEXT("true") : TEXT("false"), bUseLobbiesVoiceChat ? TEXT("true") : TEXT("false")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	if (Summary.IsValid())
	{
		Result->SetObjectField(TEXT("user_facing_experience"), Summary);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateMapDefaultExperience(const TSharedPtr<FJsonObject>& Params)
{
	FString MapPath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("map_path"), MapPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bHasExpectedExperience = false;
	FPrimaryAssetId ExpectedExperienceId;
	bool bRequireDefaultExperience = true;
	bool bRequireLyraWorldSettings = true;
	bool bRequireMatchingExperience = true;
	if (!MonolithLyra::TryParseExpectedExperienceId(Params, TEXT("expected_experience_id"), bHasExpectedExperience, ExpectedExperienceId, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_default_experience"), bRequireDefaultExperience, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_lyra_world_settings"), bRequireLyraWorldSettings, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_matching_experience"), bRequireMatchingExperience, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TSharedPtr<FJsonObject> MapContract = MonolithLyra::BuildMapDefaultExperienceContract(
		MapPath,
		ExpectedExperienceId,
		bHasExpectedExperience,
		bRequireDefaultExperience,
		bRequireLyraWorldSettings,
		bRequireMatchingExperience,
		Checks,
		Warnings,
		bOk);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("validate_map_default_experience"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetObjectField(TEXT("map"), MapContract);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetBoolField(TEXT("require_default_experience"), bRequireDefaultExperience);
	Result->SetBoolField(TEXT("require_lyra_world_settings"), bRequireLyraWorldSettings);
	Result->SetBoolField(TEXT("require_matching_experience"), bRequireMatchingExperience);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateUserFacingMapReachability(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	FString Error;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("user_facing_experience_path"), AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bRequireResolvedPrimaryAssets = false;
	bool bRequireMapDefaultExperience = false;
	bool bRequireLyraWorldSettings = true;
	bool bRequireMatchingMapDefaultExperience = false;
	if (!MonolithLyra::TryReadBoolParam(Params, TEXT("require_resolved_primary_assets"), bRequireResolvedPrimaryAssets, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_map_default_experience"), bRequireMapDefaultExperience, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_lyra_world_settings"), bRequireLyraWorldSettings, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_matching_map_default_experience"), bRequireMatchingMapDefaultExperience, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TSharedPtr<FJsonObject> Summary;
	TSharedPtr<FJsonObject> MapContract = MakeShared<FJsonObject>();

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(AssetPath, MonolithLyra::LyraUserFacingExperienceClassPath, Resolved, Error))
	{
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), false, TEXT("error"), Error);
	}
	else
	{
		Summary = MonolithLyra::BuildUserFacingSummary(Resolved);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("asset_resolved"), true, TEXT("error"), Resolved.ResolvedPath);

		FPrimaryAssetId MapId;
		FPrimaryAssetId ExperienceId;
		MonolithLyra::TryGetPrimaryAssetIdProperty(Resolved.Object, TEXT("MapID"), MapId);
		MonolithLyra::TryGetPrimaryAssetIdProperty(Resolved.Object, TEXT("ExperienceID"), ExperienceId);
		MonolithLyra::AddCheck(Checks, bOk, TEXT("map_id_valid"), MapId.IsValid(), TEXT("error"), MapId.ToString());
		MonolithLyra::AddCheck(Checks, bOk, TEXT("experience_id_valid"), ExperienceId.IsValid(), TEXT("error"), ExperienceId.ToString());
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("map_id_type"),
			!MapId.IsValid() || MapId.PrimaryAssetType.ToString().Equals(TEXT("Map"), ESearchCase::IgnoreCase),
			TEXT("error"),
			MapId.PrimaryAssetType.ToString());
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("experience_id_type"),
			!ExperienceId.IsValid() || ExperienceId.PrimaryAssetType.ToString().Equals(TEXT("LyraExperienceDefinition"), ESearchCase::IgnoreCase),
			TEXT("error"),
			ExperienceId.PrimaryAssetType.ToString());

		const FString MapResolvedPath = MapId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(MapId).ToString()
			: FString();
		const FString ExperienceResolvedPath = ExperienceId.IsValid() && UAssetManager::IsInitialized()
			? UAssetManager::Get().GetPrimaryAssetPath(ExperienceId).ToString()
			: FString();
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("map_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !MapResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			MapResolvedPath.IsEmpty() ? TEXT("MapID did not resolve through AssetManager; falling back to MapID name as package path") : MapResolvedPath);
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("experience_primary_asset_resolves"),
			!bRequireResolvedPrimaryAssets || !ExperienceResolvedPath.IsEmpty(),
			bRequireResolvedPrimaryAssets ? TEXT("error") : TEXT("warning"),
			ExperienceResolvedPath.IsEmpty() ? TEXT("ExperienceID did not resolve through AssetManager") : ExperienceResolvedPath);

		if (MapId.IsValid())
		{
			MapContract = MonolithLyra::BuildMapDefaultExperienceContract(
				MapId.ToString(),
				ExperienceId,
				ExperienceId.IsValid(),
				bRequireMapDefaultExperience,
				bRequireLyraWorldSettings,
				bRequireMatchingMapDefaultExperience,
				Checks,
				Warnings,
				bOk);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("validate_user_facing_map_reachability"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetBoolField(TEXT("require_resolved_primary_assets"), bRequireResolvedPrimaryAssets);
	Result->SetBoolField(TEXT("require_map_default_experience"), bRequireMapDefaultExperience);
	Result->SetBoolField(TEXT("require_lyra_world_settings"), bRequireLyraWorldSettings);
	Result->SetBoolField(TEXT("require_matching_map_default_experience"), bRequireMatchingMapDefaultExperience);
	if (Summary.IsValid())
	{
		Result->SetObjectField(TEXT("user_facing_experience"), Summary);
	}
	Result->SetObjectField(TEXT("map"), MapContract);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeGameplayTagDomain(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString RootTagName = TEXT("GamePhase");
	bool bIncludeChildren = true;
	int32 MaxTags = 256;
	if (!MonolithLyra::TryGetOptionalStringParam(Params, TEXT("root_tag"), RootTagName, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("include_children"), bIncludeChildren, Error)
		|| !MonolithLyra::TryReadMaxParam(Params, TEXT("max_tags"), 256, MaxTags, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (RootTagName.IsEmpty())
	{
		RootTagName = TEXT("GamePhase");
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_gameplay_tag_domain"));
	Result->SetObjectField(TEXT("tag_domain"), MonolithLyra::BuildGameplayTagDomain(RootTagName, bIncludeChildren, MaxTags));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateGamePhaseFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString RootTagName = TEXT("GamePhase");
	FString PathFilter;
	bool bDisallowDuplicateTags = false;
	int32 MaxAssets = 256;
	TArray<FString> PhaseAbilityPaths;
	TArray<FString> ExpectedPhaseTags;
	if (!MonolithLyra::TryGetOptionalStringParam(Params, TEXT("root_tag"), RootTagName, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("path_filter"), PathFilter, Error)
		|| !MonolithLyra::TryReadStringArrayParam(Params, TEXT("phase_ability_paths"), PhaseAbilityPaths, Error)
		|| !MonolithLyra::TryReadStringArrayParam(Params, TEXT("expected_phase_tags"), ExpectedPhaseTags, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("disallow_duplicate_tags"), bDisallowDuplicateTags, Error)
		|| !MonolithLyra::TryReadMaxParam(Params, TEXT("max_assets"), 256, MaxAssets, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (RootTagName.IsEmpty())
	{
		RootTagName = TEXT("GamePhase");
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> ExplicitPathErrors;

	UClass* PhaseAbilityBaseClass = StaticLoadClass(UObject::StaticClass(), nullptr, MonolithLyra::LyraGamePhaseAbilityClassPath);
	UClass* PhaseSubsystemClass = StaticLoadClass(UObject::StaticClass(), nullptr, MonolithLyra::LyraGamePhaseSubsystemClassPath);
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("lyra_game_phase_ability_type"),
		PhaseAbilityBaseClass != nullptr,
		TEXT("error"),
		PhaseAbilityBaseClass ? PhaseAbilityBaseClass->GetPathName() : FString::Printf(TEXT("Missing %s"), MonolithLyra::LyraGamePhaseAbilityClassPath));
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("lyra_game_phase_subsystem_type"),
		PhaseSubsystemClass != nullptr,
		TEXT("error"),
		PhaseSubsystemClass ? PhaseSubsystemClass->GetPathName() : FString::Printf(TEXT("Missing %s"), MonolithLyra::LyraGamePhaseSubsystemClassPath));

	const FGameplayTag RootTag = MonolithLyra::RequestTagNoError(RootTagName);
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("root_tag_registered"),
		RootTag.IsValid(),
		TEXT("error"),
		RootTag.IsValid() ? RootTag.ToString() : FString::Printf(TEXT("GameplayTag '%s' is not registered"), *RootTagName));

	TArray<MonolithLyra::FPhaseAbilitySummary> PhaseAbilities;
	TSet<FString> SeenClassPaths;
	for (const FString& PhaseAbilityPath : PhaseAbilityPaths)
	{
		MonolithLyra::FResolvedLyraObject Resolved;
		if (!MonolithLyra::TryResolveLyraObject(PhaseAbilityPath, MonolithLyra::LyraGamePhaseAbilityClassPath, Resolved, Error))
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("input_path"), PhaseAbilityPath);
			Row->SetStringField(TEXT("error"), Error);
			ExplicitPathErrors.Add(MakeShared<FJsonValueObject>(Row));
			MonolithLyra::AddCheck(Checks, bOk, TEXT("explicit_phase_ability_path_resolved"), false, TEXT("error"), Error);
			continue;
		}

		UClass* AbilityClass = Resolved.Object ? Resolved.Object->GetClass() : nullptr;
		MonolithLyra::AddPhaseAbilitySummary(
			AbilityClass,
			PhaseAbilityPath,
			Resolved.SaveTargetPath,
			Resolved.SourceKind,
			Cast<UBlueprint>(Resolved.AssetForSave) != nullptr,
			SeenClassPaths,
			PhaseAbilities);
	}

	bool bDiscoveryTruncated = false;
	MonolithLyra::CollectLoadedNativePhaseAbilitySummaries(
		PhaseAbilityBaseClass,
		SeenClassPaths,
		PhaseAbilities);
	if (!PathFilter.IsEmpty())
	{
		MonolithLyra::CollectBlueprintPhaseAbilitySummaries(
			PhaseAbilityBaseClass,
			PathFilter,
			MaxAssets,
			bDiscoveryTruncated,
			SeenClassPaths,
			PhaseAbilities);
	}
	else
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("check"), TEXT("blueprint_phase_ability_discovery_skipped"));
		Warning->SetStringField(TEXT("severity"), TEXT("info"));
		Warning->SetStringField(TEXT("message"), TEXT("Pass path_filter to scan Blueprint LyraGamePhaseAbility assets; default validation only inspects loaded native classes and explicit phase_ability_paths."));
		Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	}

	TArray<TSharedPtr<FJsonValue>> PhaseAbilityRows;
	TMap<FString, TArray<FString>> ClassesByPhaseTag;
	int32 InvalidTagCount = 0;
	int32 OutsideRootCount = 0;
	for (const MonolithLyra::FPhaseAbilitySummary& Summary : PhaseAbilities)
	{
		PhaseAbilityRows.Add(MakeShared<FJsonValueObject>(MonolithLyra::PhaseAbilitySummaryToJson(Summary)));
		if (Summary.bIsAbstract)
		{
			continue;
		}

		if (!Summary.bPhaseTagValid)
		{
			++InvalidTagCount;
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("phase_ability_has_game_phase_tag"),
				false,
				TEXT("error"),
				FString::Printf(TEXT("%s has an empty or missing GamePhaseTag"), *Summary.ClassPath));
			continue;
		}

		const FGameplayTag PhaseTag = MonolithLyra::RequestTagNoError(Summary.PhaseTagString);
		ClassesByPhaseTag.FindOrAdd(Summary.PhaseTagString).Add(Summary.ClassPath);
		if (RootTag.IsValid() && !PhaseTag.MatchesTag(RootTag))
		{
			++OutsideRootCount;
			MonolithLyra::AddCheck(
				Checks,
				bOk,
				TEXT("phase_tag_inside_root_domain"),
				false,
				TEXT("error"),
				FString::Printf(TEXT("%s uses %s outside root %s"), *Summary.ClassPath, *Summary.PhaseTagString, *RootTagName));
		}
	}

	TArray<TSharedPtr<FJsonValue>> DuplicateGroups;
	for (const TPair<FString, TArray<FString>>& Pair : ClassesByPhaseTag)
	{
		if (Pair.Value.Num() <= 1)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Group = MakeShared<FJsonObject>();
		Group->SetStringField(TEXT("phase_tag"), Pair.Key);
		Group->SetArrayField(TEXT("classes"), MonolithLyra::StringArrayToJson(Pair.Value));
		Group->SetNumberField(TEXT("class_count"), Pair.Value.Num());
		DuplicateGroups.Add(MakeShared<FJsonValueObject>(Group));

		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("duplicate_phase_tag"),
			!bDisallowDuplicateTags,
			bDisallowDuplicateTags ? TEXT("error") : TEXT("warning"),
			FString::Printf(TEXT("%d phase ability classes share %s"), Pair.Value.Num(), *Pair.Key));
	}

	TArray<TSharedPtr<FJsonValue>> ExpectedRows;
	for (const FString& ExpectedTagName : ExpectedPhaseTags)
	{
		const FGameplayTag ExpectedTag = MonolithLyra::RequestTagNoError(ExpectedTagName);
		const bool bRegistered = ExpectedTag.IsValid();
		const bool bBackedByAbility = ClassesByPhaseTag.Contains(ExpectedTagName);
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("phase_tag"), ExpectedTagName);
		Row->SetBoolField(TEXT("registered"), bRegistered);
		Row->SetBoolField(TEXT("backed_by_phase_ability"), bBackedByAbility);
		ExpectedRows.Add(MakeShared<FJsonValueObject>(Row));

		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("expected_phase_tag_registered"),
			bRegistered,
			TEXT("error"),
			bRegistered ? ExpectedTagName : FString::Printf(TEXT("Expected phase tag '%s' is not registered"), *ExpectedTagName));
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("expected_phase_tag_backed_by_ability"),
			bBackedByAbility,
			TEXT("error"),
			bBackedByAbility ? ExpectedTagName : FString::Printf(TEXT("Expected phase tag '%s' has no discovered LyraGamePhaseAbility"), *ExpectedTagName));
	}

	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("phase_ability_count"),
		PhaseAbilities.Num() > 0,
		ExpectedPhaseTags.Num() > 0 ? TEXT("error") : TEXT("warning"),
		FString::Printf(TEXT("%d Lyra game phase ability classes discovered"), PhaseAbilities.Num()));

	if (bDiscoveryTruncated)
	{
		Warnings.Add(MakeShared<FJsonValueObject>(MonolithLyra::MakeCheck(
			TEXT("phase_ability_discovery_truncated"),
			false,
			TEXT("warning"),
			FString::Printf(TEXT("Discovery stopped after max_assets=%d matching Blueprint phase abilities"), MaxAssets))));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("validate_game_phase_flow"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("root_tag"), RootTagName);
	Result->SetStringField(TEXT("path_filter"), PathFilter);
	Result->SetObjectField(TEXT("tag_domain"), MonolithLyra::BuildGameplayTagDomain(RootTagName, /*bIncludeChildren=*/true, MaxAssets));
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("explicit_path_errors"), ExplicitPathErrors);
	Result->SetArrayField(TEXT("phase_abilities"), PhaseAbilityRows);
	Result->SetNumberField(TEXT("phase_ability_count"), PhaseAbilities.Num());
	Result->SetNumberField(TEXT("invalid_phase_tag_count"), InvalidTagCount);
	Result->SetNumberField(TEXT("outside_root_tag_count"), OutsideRootCount);
	Result->SetArrayField(TEXT("duplicate_phase_tag_groups"), DuplicateGroups);
	Result->SetArrayField(TEXT("expected_phase_tags"), ExpectedRows);
	Result->SetBoolField(TEXT("discovery_truncated"), bDiscoveryTruncated);
	Result->SetArrayField(TEXT("notes"), MonolithLyra::StringArrayToJson({
		TEXT("Lyra allows multiple phase abilities to share an exact phase tag; duplicates are reported unless disallow_duplicate_tags=true."),
		TEXT("The validator uses reflected CDO data and does not mutate assets or runtime CommonGame code.")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeTeamSetup(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString ComponentClassPath;
	if (!MonolithLyra::TryGetOptionalStringParam(Params, TEXT("team_creation_component_class"), ComponentClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (ComponentClassPath.IsEmpty())
	{
		ComponentClassPath = MonolithLyra::LyraTeamCreationComponentClassPath;
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(ComponentClassPath, MonolithLyra::LyraTeamCreationComponentClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_team_setup"));
	Result->SetObjectField(TEXT("team_setup"), MonolithLyra::BuildTeamCreationSummary(Resolved));
	Result->SetArrayField(TEXT("notes"), MonolithLyra::StringArrayToJson({
		TEXT("This action reads the TeamCreationComponent class default object only; it does not spawn GameState components or create teams."),
		TEXT("TeamsToCreate is reflected from private UPROPERTY data so projects can validate team defaults without runtime code changes.")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeInventoryItem(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString ItemPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("item_definition_path"), ItemPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(ItemPath, MonolithLyra::LyraInventoryItemDefinitionClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_inventory_item"));
	Result->SetObjectField(TEXT("inventory_item"), MonolithLyra::BuildInventoryItemSummary(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeEquipmentDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString EquipmentPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("equipment_definition_path"), EquipmentPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(EquipmentPath, MonolithLyra::LyraEquipmentDefinitionClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_equipment_definition"));
	Result->SetObjectField(TEXT("equipment_definition"), MonolithLyra::BuildEquipmentDefinitionSummary(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeWeaponDefinition(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString ItemPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("item_definition_path"), ItemPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bRequireEquippableFragment = true;
	if (!MonolithLyra::TryReadBoolParam(Params, TEXT("require_equippable_fragment"), bRequireEquippableFragment, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject ItemResolved;
	if (!MonolithLyra::TryResolveLyraObject(ItemPath, MonolithLyra::LyraInventoryItemDefinitionClassPath, ItemResolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	UObject* EquippableFragment = MonolithLyra::GetFirstFragmentWithClassName(ItemResolved.Object, TEXT("InventoryFragment_EquippableItem"));
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("has_equippable_fragment"),
		EquippableFragment != nullptr || !bRequireEquippableFragment,
		bRequireEquippableFragment ? TEXT("error") : TEXT("warning"),
		EquippableFragment ? EquippableFragment->GetPathName() : TEXT("No InventoryFragment_EquippableItem fragment found"));

	UClass* EquipmentClass = MonolithLyra::GetClassPropertyValue(EquippableFragment, TEXT("EquipmentDefinition"));
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("equipment_definition_set"),
		EquipmentClass != nullptr || !bRequireEquippableFragment,
		bRequireEquippableFragment ? TEXT("error") : TEXT("warning"),
		EquipmentClass ? EquipmentClass->GetPathName() : TEXT("Equippable fragment has no EquipmentDefinition"));

	TSharedPtr<FJsonObject> EquipmentSummary = MakeShared<FJsonObject>();
	UClass* InstanceType = nullptr;
	if (EquipmentClass)
	{
		MonolithLyra::FResolvedLyraObject EquipmentResolved;
		if (MonolithLyra::TryResolveLyraObject(EquipmentClass->GetPathName(), MonolithLyra::LyraEquipmentDefinitionClassPath, EquipmentResolved, Error))
		{
			EquipmentSummary = MonolithLyra::BuildEquipmentDefinitionSummary(EquipmentResolved);
			InstanceType = MonolithLyra::GetClassPropertyValue(EquipmentResolved.Object, TEXT("InstanceType"));
		}
		else
		{
			MonolithLyra::AddCheck(Checks, bOk, TEXT("equipment_definition_resolves"), false, TEXT("error"), Error);
		}
	}

	UClass* WeaponInstanceBase = MonolithLyra::LoadExpectedClass(MonolithLyra::LyraWeaponInstanceClassPath);
	const bool bWeaponInstance = InstanceType && WeaponInstanceBase && InstanceType->IsChildOf(WeaponInstanceBase);
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("equipment_instance_is_weapon"),
		bWeaponInstance,
		bRequireEquippableFragment ? TEXT("error") : TEXT("warning"),
		InstanceType ? InstanceType->GetPathName() : TEXT("Equipment InstanceType is not set"));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_weapon_definition"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetObjectField(TEXT("inventory_item"), MonolithLyra::BuildInventoryItemSummary(ItemResolved));
	Result->SetObjectField(TEXT("equippable_fragment"), MonolithLyra::BuildFragmentSummary(EquippableFragment, INDEX_NONE));
	Result->SetObjectField(TEXT("equipment_definition"), EquipmentSummary);
	Result->SetObjectField(TEXT("weapon_instance_base"), MonolithLyra::ClassRefToJson(WeaponInstanceBase));
	Result->SetObjectField(TEXT("resolved_instance_type"), MonolithLyra::ClassRefToJson(InstanceType, MonolithLyra::LyraWeaponInstanceClassPath));
	Result->SetArrayField(TEXT("checks"), Checks);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribePawnInitializationGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString PawnDataPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("pawn_data_path"), PawnDataPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(PawnDataPath, MonolithLyra::LyraPawnDataClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_pawn_initialization_graph"));
	Result->SetObjectField(TEXT("pawn_data"), MonolithLyra::BuildPawnDataSummary(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidatePawnDataContract(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString PawnDataPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("pawn_data_path"), PawnDataPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bRequirePawnClass = true;
	bool bRequireAbilitySets = false;
	bool bRequireInputConfig = false;
	bool bRequireDefaultCameraMode = false;
	FString ExpectedPawnClassPath;
	if (!MonolithLyra::TryReadBoolParam(Params, TEXT("require_pawn_class"), bRequirePawnClass, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_ability_sets"), bRequireAbilitySets, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_input_config"), bRequireInputConfig, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_default_camera_mode"), bRequireDefaultCameraMode, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("expected_pawn_class"), ExpectedPawnClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(PawnDataPath, MonolithLyra::LyraPawnDataClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	UClass* PawnClass = MonolithLyra::GetClassPropertyValue(Resolved.Object, TEXT("PawnClass"));
	MonolithLyra::AddPawnDataContractChecks(
		Checks,
		bOk,
		Resolved.Object,
		bRequirePawnClass,
		bRequireAbilitySets,
		bRequireInputConfig,
		bRequireDefaultCameraMode,
		FString());

	if (!ExpectedPawnClassPath.IsEmpty())
	{
		UClass* ExpectedPawnClass = MonolithLyra::LoadClassPathWithGeneratedFallback(ExpectedPawnClassPath);
		const bool bMatchesExpected = PawnClass && ExpectedPawnClass && PawnClass->IsChildOf(ExpectedPawnClass);
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("expected_pawn_class"),
			bMatchesExpected,
			TEXT("error"),
			bMatchesExpected ? PawnClass->GetPathName() : FString::Printf(TEXT("PawnClass '%s' is not a child of expected '%s'"), PawnClass ? *PawnClass->GetPathName() : TEXT("<unset>"), *ExpectedPawnClassPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("validate_pawn_data_contract"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetObjectField(TEXT("pawn_data"), MonolithLyra::BuildPawnDataSummary(Resolved));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::DescribeCharacterPartGraph(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	TArray<FString> PartClasses;
	if (!MonolithLyra::TryReadStringArrayParam(Params, TEXT("part_classes"), PartClasses, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TArray<TSharedPtr<FJsonValue>> ReflectedTypes;
	for (const TCHAR* TypePath : {
		MonolithLyra::LyraControllerCharacterPartsClassPath,
		MonolithLyra::LyraPawnCharacterPartsClassPath,
		MonolithLyra::LyraCosmeticDeveloperSettingsClassPath
	})
	{
		UClass* Class = MonolithLyra::LoadExpectedClass(TypePath);
		TSharedPtr<FJsonObject> Row = MonolithLyra::ClassRefToJson(Class);
		Row->SetStringField(TEXT("requested_class_path"), TypePath);
		ReflectedTypes.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> DeveloperSettings = MakeShared<FJsonObject>();
	MonolithLyra::FResolvedLyraObject SettingsResolved;
	if (MonolithLyra::TryResolveLyraObject(MonolithLyra::LyraCosmeticDeveloperSettingsClassPath, MonolithLyra::LyraCosmeticDeveloperSettingsClassPath, SettingsResolved, Error))
	{
		DeveloperSettings = MonolithLyra::MakeResolvedSummary(SettingsResolved);
		DeveloperSettings->SetField(TEXT("cheat_cosmetic_character_parts"), MonolithLyra::ReadPropertyValue(SettingsResolved.Object, TEXT("CheatCosmeticCharacterParts")));
		DeveloperSettings->SetNumberField(TEXT("cheat_part_count"), MonolithLyra::GetArrayPropertyCount(SettingsResolved.Object, TEXT("CheatCosmeticCharacterParts")));
	}
	else
	{
		DeveloperSettings->SetStringField(TEXT("error"), Error);
	}

	TArray<TSharedPtr<FJsonValue>> PartRows;
	for (const FString& PartClass : PartClasses)
	{
		PartRows.Add(MakeShared<FJsonValueObject>(MonolithLyra::BuildCharacterPartClassSummary(PartClass)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("describe_character_part_graph"));
	Result->SetArrayField(TEXT("reflected_types"), ReflectedTypes);
	Result->SetObjectField(TEXT("developer_settings"), DeveloperSettings);
	Result->SetArrayField(TEXT("part_classes"), PartRows);
	Result->SetNumberField(TEXT("part_class_count"), PartRows.Num());
	Result->SetArrayField(TEXT("notes"), MonolithLyra::StringArrayToJson({
		TEXT("FLyraCharacterPart is a struct, not a standalone asset; pass part_classes to validate actor classes used by character-part entries."),
		TEXT("This action does not spawn cosmetic actors or modify controller/pawn character-part components.")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::ValidateCharacterPartAssets(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	TArray<FString> PartClasses;
	bool bRequireNonEmpty = true;
	if (!MonolithLyra::TryReadStringArrayParam(Params, TEXT("part_classes"), PartClasses, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("require_non_empty"), bRequireNonEmpty, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bOk = true;
	TArray<TSharedPtr<FJsonValue>> Checks;
	MonolithLyra::AddCheck(
		Checks,
		bOk,
		TEXT("part_classes_non_empty"),
		PartClasses.Num() > 0 || !bRequireNonEmpty,
		bRequireNonEmpty ? TEXT("error") : TEXT("info"),
		FString::Printf(TEXT("%d part_classes supplied"), PartClasses.Num()));

	TArray<TSharedPtr<FJsonValue>> PartRows;
	for (const FString& PartClass : PartClasses)
	{
		TSharedPtr<FJsonObject> Row = MonolithLyra::BuildCharacterPartClassSummary(PartClass);
		const bool bPartOk = Row->GetBoolField(TEXT("ok"));
		MonolithLyra::AddCheck(
			Checks,
			bOk,
			TEXT("part_class_loadable_concrete_actor"),
			bPartOk,
			TEXT("error"),
			PartClass);
		PartRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("validate_character_part_assets"));
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("part_classes"), PartRows);
	Result->SetNumberField(TEXT("part_class_count"), PartRows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::SetExperienceDefaults(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	MonolithLyra::FLyraMutationOptions Options;
	if (!MonolithLyra::TryReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FString ExperiencePath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("experience_path"), ExperiencePath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(ExperiencePath, MonolithLyra::LyraExperienceDefinitionClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	if (!MonolithLyra::AddStringFieldIfPresent(Params, TEXT("default_pawn_data"), Tree, TEXT("DefaultPawnData"), Error)
		|| !MonolithLyra::AddStringArrayFieldIfPresent(Params, TEXT("action_sets"), Tree, TEXT("ActionSets"), Error)
		|| !MonolithLyra::AddStringArrayFieldIfPresent(Params, TEXT("game_features_to_enable"), Tree, TEXT("GameFeaturesToEnable"), Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FDryRunReport Report;
	if (!MonolithLyra::DispatchBlueprintBulkFill(Resolved, Tree, Options, Report, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bSaved = false;
	FString SavedPath;
	if (!Options.bDryRun && Options.bSave && MonolithLyra::IsReportCleanForCommit(Report))
	{
		if (!MonolithLyra::SaveAssetIfRequested(Resolved.AssetForSave, true, bSaved, SavedPath, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	FString PackageFilename;
	MonolithLyra::TryGetPackageFilename(Resolved.AssetForSave, PackageFilename);

	TSharedPtr<FJsonObject> Result = FMonolithDryRunGuard::ReportToJson(Report);
	MonolithLyra::AddMutationFields(
		Result,
		TEXT("set_experience_defaults"),
		Resolved,
		Options,
		!Options.bDryRun && MonolithLyra::IsReportCleanForCommit(Report),
		bSaved,
		SavedPath);
	Result->SetObjectField(TEXT("write_plan"), MonolithLyra::MakeWritePlan(Options, Resolved.SourceKind.Contains(TEXT("blueprint")), PackageFilename));
	Result->SetObjectField(TEXT("current_experience_graph"), MonolithLyra::BuildExperienceGraph(Resolved));
	Result->SetObjectField(TEXT("requested_tree"), Tree);
	Result->SetBoolField(TEXT("would_change"), MonolithLyra::WouldChangeFromReport(Report));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::AddExperienceComponentEntry(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	MonolithLyra::FLyraMutationOptions Options;
	if (!MonolithLyra::TryReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FString ExperiencePath;
	FString ActionSetPath;
	FString ActorClassPath;
	FString ComponentClassPath;
	FString ActionName;
	bool bClientComponent = true;
	bool bServerComponent = true;
	int32 AdditionFlags = 0;
	if (!MonolithLyra::TryGetOptionalStringParam(Params, TEXT("experience_path"), ExperiencePath, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("action_set_path"), ActionSetPath, Error)
		|| !MonolithLyra::TryGetRequiredStringParam(Params, TEXT("actor_class"), ActorClassPath, Error)
		|| !MonolithLyra::TryGetRequiredStringParam(Params, TEXT("component_class"), ComponentClassPath, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("action_name"), ActionName, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("client_component"), bClientComponent, Error)
		|| !MonolithLyra::TryReadBoolParam(Params, TEXT("server_component"), bServerComponent, Error)
		|| !MonolithLyra::TryReadIntParam(Params, TEXT("addition_flags"), AdditionFlags, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	const bool bHasExperiencePath = !ExperiencePath.IsEmpty();
	const bool bHasActionSetPath = !ActionSetPath.IsEmpty();
	if (bHasExperiencePath == bHasActionSetPath)
	{
		return FMonolithActionResult::Error(
			TEXT("Exactly one of 'experience_path' or 'action_set_path' is required"),
			MonolithLyra::ErrInvalidParams);
	}
	if (!ActionName.IsEmpty() && !FName(*ActionName).IsValidXName())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Param 'action_name' is not a valid UObject name: '%s'"), *ActionName),
			MonolithLyra::ErrInvalidParams);
	}
	if (AdditionFlags < 0 || AdditionFlags > MAX_uint8)
	{
		return FMonolithActionResult::Error(
			TEXT("Param 'addition_flags' must be between 0 and 255"),
			MonolithLyra::ErrInvalidParams);
	}
	if (!bClientComponent && !bServerComponent)
	{
		return FMonolithActionResult::Error(
			TEXT("At least one of 'client_component' or 'server_component' must be true"),
			MonolithLyra::ErrInvalidParams);
	}

	UClass* ActorClass = MonolithLyra::LoadSubclassForParam(
		ActorClassPath,
		TEXT("actor_class"),
		MonolithLyra::EngineActorClassPath,
		Error);
	if (!ActorClass)
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	UClass* ComponentClass = MonolithLyra::LoadSubclassForParam(
		ComponentClassPath,
		TEXT("component_class"),
		MonolithLyra::EngineActorComponentClassPath,
		Error);
	if (!ComponentClass)
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	UClass* AddComponentsActionClass = MonolithLyra::LoadExpectedClass(MonolithLyra::GameFeatureActionAddComponentsClassPath);
	UClass* GameFeatureActionBaseClass = MonolithLyra::LoadExpectedClass(MonolithLyra::GameFeatureActionClassPath);
	if (!AddComponentsActionClass
		|| !GameFeatureActionBaseClass
		|| !AddComponentsActionClass->IsChildOf(GameFeatureActionBaseClass)
		|| AddComponentsActionClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return FMonolithActionResult::Error(
			TEXT("Concrete GameFeatureAction_AddComponents reflected class is unavailable"),
			MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	const FString& TargetInputPath = bHasExperiencePath ? ExperiencePath : ActionSetPath;
	const TCHAR* ExpectedTargetClassPath = bHasExperiencePath
		? MonolithLyra::LyraExperienceDefinitionClassPath
		: MonolithLyra::LyraExperienceActionSetClassPath;
	if (!MonolithLyra::TryResolveLyraObject(TargetInputPath, ExpectedTargetClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FArrayProperty* ActionsProperty = nullptr;
	FObjectPropertyBase* ActionsObjectProperty = nullptr;
	if (!MonolithLyra::TryGetActionsArray(Resolved.Object, ActionsProperty, ActionsObjectProperty, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (ActionsObjectProperty->PropertyClass && !AddComponentsActionClass->IsChildOf(ActionsObjectProperty->PropertyClass))
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("AddComponents action class '%s' is incompatible with Actions element class '%s'"),
				*AddComponentsActionClass->GetPathName(),
				*ActionsObjectProperty->PropertyClass->GetPathName()),
			MonolithLyra::ErrInvalidParams);
	}

	FScriptArrayHelper ActionsHelper(ActionsProperty, ActionsProperty->ContainerPtrToValuePtr<void>(Resolved.Object));
	const int32 ActionsBefore = ActionsHelper.Num();
	int32 ActionIndex = INDEX_NONE;
	int32 ExistingAddComponentsActionCount = 0;
	UObject* Action = MonolithLyra::FindAddComponentsAction(
		ActionsHelper,
		ActionsObjectProperty,
		AddComponentsActionClass,
		ActionName,
		ActionIndex,
		ExistingAddComponentsActionCount,
		Error);
	if (!Error.IsEmpty())
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	const FName RequestedActionObjectName = ActionName.IsEmpty() ? NAME_None : FName(*ActionName);
	if (!RequestedActionObjectName.IsNone())
	{
		UObject* DirectChildWithRequestedName = StaticFindObjectFast(
			UObject::StaticClass(),
			Resolved.Object,
			RequestedActionObjectName);
		if (DirectChildWithRequestedName && DirectChildWithRequestedName != Action)
		{
			const bool bCompatibleOrphan = DirectChildWithRequestedName->IsA(AddComponentsActionClass);
			return FMonolithActionResult::Error(
				bCompatibleOrphan
					? FString::Printf(
						TEXT("Owner '%s' already has direct child '%s' of AddComponents class '%s', but that object is not the requested entry in its Actions array; repair or rename the orphan before using action_name"),
						*Resolved.Object->GetPathName(),
						*RequestedActionObjectName.ToString(),
						*DirectChildWithRequestedName->GetClass()->GetPathName())
					: FString::Printf(
						TEXT("Owner '%s' already has direct child '%s' with incompatible class '%s'; expected '%s' for action_name"),
						*Resolved.Object->GetPathName(),
						*RequestedActionObjectName.ToString(),
						*DirectChildWithRequestedName->GetClass()->GetPathName(),
						*AddComponentsActionClass->GetPathName()),
				MonolithLyra::ErrInvalidParams);
		}
	}

	MonolithLyra::FComponentEntryMutationPlan EntryPlan;
	UObject* PairOwningAction = nullptr;
	int32 PairOwningActionIndex = INDEX_NONE;
	MonolithLyra::FComponentEntryMutationPlan PairOwningPlan;
	int32 PairOwningActionCount = 0;
	for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
	{
		UObject* CandidateAction = ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index));
		if (!CandidateAction || !CandidateAction->IsA(AddComponentsActionClass))
		{
			continue;
		}

		MonolithLyra::FComponentEntryMutationPlan CandidatePlan;
		if (!MonolithLyra::BuildComponentEntryMutationPlan(
			CandidateAction,
			AddComponentsActionClass,
			ActorClass,
			ComponentClass,
			bClientComponent,
			bServerComponent,
			AdditionFlags,
			CandidatePlan,
			Error))
		{
			return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
		}
		if (!CandidatePlan.bAdd)
		{
			++PairOwningActionCount;
			PairOwningAction = CandidateAction;
			PairOwningActionIndex = Index;
			PairOwningPlan = CandidatePlan;
		}
	}
	if (PairOwningActionCount > 1)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Actor/component pair exists in %d AddComponents actions; remove duplicate entries before updating"),
				PairOwningActionCount),
			MonolithLyra::ErrInvalidParams);
	}
	if (PairOwningAction && !ActionName.IsEmpty() && PairOwningAction != Action)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Actor/component pair already belongs to action '%s' at index %d, not requested action '%s'"),
				*PairOwningAction->GetName(),
				PairOwningActionIndex,
				*ActionName),
			MonolithLyra::ErrInvalidParams);
	}
	if (PairOwningAction)
	{
		Action = PairOwningAction;
		ActionIndex = PairOwningActionIndex;
		EntryPlan = PairOwningPlan;
	}
	else if (!MonolithLyra::BuildComponentEntryMutationPlan(
		Action,
		AddComponentsActionClass,
		ActorClass,
		ComponentClass,
		bClientComponent,
		bServerComponent,
		AdditionFlags,
		EntryPlan,
		Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	const bool bWouldCreateAction = Action == nullptr;
	const bool bWouldChange = bWouldCreateAction || EntryPlan.bAdd || EntryPlan.bUpdate;
	bool bCreatedAction = false;
	bool bAddedComponent = false;
	bool bUpdatedComponent = false;
	if (!Options.bDryRun && bWouldChange)
	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithLyra", "AddExperienceComponentEntry", "Monolith Lyra Add Experience Component Entry"));
		Resolved.Object->Modify();
		if (!Action)
		{
			const FName NewActionObjectName = ActionName.IsEmpty()
				? MakeUniqueObjectName(Resolved.Object, AddComponentsActionClass, AddComponentsActionClass->GetFName())
				: RequestedActionObjectName;
			Action = NewObject<UObject>(Resolved.Object, AddComponentsActionClass, NewActionObjectName, RF_Transactional);
			if (!Action)
			{
				return FMonolithActionResult::Error(TEXT("Failed to create instanced GameFeatureAction_AddComponents object"));
			}
			Action->Modify();
			ActionIndex = ActionsHelper.AddValue();
			ActionsObjectProperty->SetObjectPropertyValue(ActionsHelper.GetRawPtr(ActionIndex), Action);
			bCreatedAction = true;
		}
		else
		{
			Action->Modify();
		}

		if (!MonolithLyra::ApplyComponentEntryMutation(
			Action,
			ActorClass,
			ComponentClass,
			bClientComponent,
			bServerComponent,
			AdditionFlags,
			EntryPlan,
			Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		bAddedComponent = EntryPlan.bAdd;
		bUpdatedComponent = EntryPlan.bUpdate;
		Action->MarkPackageDirty();
		if (Resolved.AssetForSave)
		{
			Resolved.AssetForSave->MarkPackageDirty();
		}
	}

	const int32 ActionsAfter = Options.bDryRun
		? ActionsBefore + (bWouldCreateAction ? 1 : 0)
		: ActionsHelper.Num();
	const bool bChanged = bCreatedAction || bAddedComponent || bUpdatedComponent;
	bool bSaved = false;
	FString SavedPath;
	if (!Options.bDryRun && Options.bSave && bChanged)
	{
		UObject* SaveTarget = Resolved.AssetForSave ? Resolved.AssetForSave : Resolved.Object;
		if (!MonolithLyra::SaveAssetIfRequested(SaveTarget, true, bSaved, SavedPath, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (ActionName.IsEmpty() && ExistingAddComponentsActionCount > 1)
	{
		Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Target contains %d AddComponents actions; selected index %d by preferring the existing actor/component pair, otherwise the first compatible action"),
			ExistingAddComponentsActionCount,
			ActionIndex)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("add_experience_component_entry"));
	Result->SetStringField(TEXT("target_kind"), bHasExperiencePath ? TEXT("experience") : TEXT("action_set"));
	Result->SetStringField(TEXT("target_input_path"), TargetInputPath);
	Result->SetStringField(TEXT("target_object_path"), Resolved.ResolvedPath);
	Result->SetStringField(TEXT("actor_class"), ActorClass->GetPathName());
	Result->SetStringField(TEXT("component_class"), ComponentClass->GetPathName());
	Result->SetStringField(TEXT("requested_action_name"), ActionName);
	Result->SetStringField(TEXT("action_class"), AddComponentsActionClass->GetPathName());
	Result->SetBoolField(TEXT("client_component"), bClientComponent);
	Result->SetBoolField(TEXT("server_component"), bServerComponent);
	Result->SetNumberField(TEXT("addition_flags"), AdditionFlags);
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("confirm_received"), Options.bConfirm);
	Result->SetBoolField(TEXT("save_requested"), Options.bSave);
	Result->SetBoolField(TEXT("would_create_action"), bWouldCreateAction);
	Result->SetBoolField(TEXT("would_add_component"), EntryPlan.bAdd);
	Result->SetBoolField(TEXT("would_update_component"), EntryPlan.bUpdate);
	Result->SetBoolField(TEXT("would_change"), bWouldChange);
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetBoolField(TEXT("reused_action"), !bWouldCreateAction);
	Result->SetBoolField(TEXT("added_component"), bAddedComponent);
	Result->SetBoolField(TEXT("updated_component"), bUpdatedComponent);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("saved_path"), SavedPath);
	Result->SetNumberField(TEXT("existing_add_components_action_count"), ExistingAddComponentsActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionsBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionsAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex == INDEX_NONE ? ActionsBefore : ActionIndex);
	Result->SetNumberField(TEXT("components_before"), EntryPlan.ComponentsBefore);
	Result->SetNumberField(TEXT("components_after"), EntryPlan.ComponentsAfter);
	Result->SetNumberField(TEXT("component_index"), EntryPlan.ComponentIndex);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	if (Action)
	{
		Result->SetStringField(TEXT("action_object_path"), Action->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), Action->GetName());
	}
	else
	{
		Result->SetStringField(TEXT("planned_action_name"), ActionName.IsEmpty() ? AddComponentsActionClass->GetName() : ActionName);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::SetUserFacingExperience(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	MonolithLyra::FLyraMutationOptions Options;
	if (!MonolithLyra::TryReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FString AssetPath;
	if (!MonolithLyra::TryGetRequiredStringParam(Params, TEXT("user_facing_experience_path"), AssetPath, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	MonolithLyra::FResolvedLyraObject Resolved;
	if (!MonolithLyra::TryResolveLyraObject(AssetPath, MonolithLyra::LyraUserFacingExperienceClassPath, Resolved, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	if (!MonolithLyra::AddStringFieldIfPresent(Params, TEXT("map_id"), Tree, TEXT("MapID"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("experience_id"), Tree, TEXT("ExperienceID"), Error)
		|| !MonolithLyra::AddObjectFieldIfPresent(Params, TEXT("extra_args"), Tree, TEXT("ExtraArgs"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("tile_title"), Tree, TEXT("TileTitle"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("tile_subtitle"), Tree, TEXT("TileSubTitle"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("tile_description"), Tree, TEXT("TileDescription"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("tile_icon"), Tree, TEXT("TileIcon"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("loading_screen_widget"), Tree, TEXT("LoadingScreenWidget"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("is_default_experience"), Tree, TEXT("bIsDefaultExperience"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("show_in_front_end"), Tree, TEXT("bShowInFrontEnd"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("record_replay"), Tree, TEXT("bRecordReplay"), Error)
		|| !MonolithLyra::AddIntFieldIfPresent(Params, TEXT("max_player_count"), Tree, TEXT("MaxPlayerCount"), Error)
		|| !MonolithLyra::AddStringFieldIfPresent(Params, TEXT("session_mode"), Tree, TEXT("SessionMode"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("use_lobbies"), Tree, TEXT("bUseLobbies"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("use_lobbies_voice_chat"), Tree, TEXT("bUseLobbiesVoiceChat"), Error)
		|| !MonolithLyra::AddBoolFieldIfPresent(Params, TEXT("use_presence"), Tree, TEXT("bUsePresence"), Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (!MonolithLyra::ValidatePrimaryAssetIdType(Tree, TEXT("MapID"), TEXT("Map"), Error)
		|| !MonolithLyra::ValidatePrimaryAssetIdType(Tree, TEXT("ExperienceID"), TEXT("LyraExperienceDefinition"), Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FDryRunReport Report;
	if (!MonolithLyra::DispatchBlueprintBulkFill(Resolved, Tree, Options, Report, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	bool bSaved = false;
	FString SavedPath;
	if (!Options.bDryRun && Options.bSave && MonolithLyra::IsReportCleanForCommit(Report))
	{
		if (!MonolithLyra::SaveAssetIfRequested(Resolved.AssetForSave, true, bSaved, SavedPath, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
	}

	FString PackageFilename;
	MonolithLyra::TryGetPackageFilename(Resolved.AssetForSave, PackageFilename);

	TSharedPtr<FJsonObject> Result = FMonolithDryRunGuard::ReportToJson(Report);
	MonolithLyra::AddMutationFields(
		Result,
		TEXT("set_user_facing_experience"),
		Resolved,
		Options,
		!Options.bDryRun && MonolithLyra::IsReportCleanForCommit(Report),
		bSaved,
		SavedPath);
	Result->SetObjectField(TEXT("write_plan"), MonolithLyra::MakeWritePlan(Options, Resolved.SourceKind.Contains(TEXT("blueprint")), PackageFilename));
	Result->SetObjectField(TEXT("current_user_facing_experience"), MonolithLyra::BuildUserFacingSummary(Resolved));
	Result->SetObjectField(TEXT("requested_tree"), Tree);
	Result->SetBoolField(TEXT("would_change"), MonolithLyra::WouldChangeFromReport(Report));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLyraActions::RemoveExperienceComponentEntry(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	MonolithLyra::FLyraMutationOptions Options;
	if (!MonolithLyra::TryReadMutationOptions(Params, Options, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}

	FString ExperiencePath;
	FString ActionSetPath;
	FString ActionName;
	FString ActorClass;
	FString ComponentClass;
	int32 ActionIndex = INDEX_NONE;
	int32 ComponentIndex = INDEX_NONE;
	if (!MonolithLyra::TryGetOptionalStringParam(Params, TEXT("experience_path"), ExperiencePath, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("action_set_path"), ActionSetPath, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("action_name"), ActionName, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("actor_class"), ActorClass, Error)
		|| !MonolithLyra::TryGetOptionalStringParam(Params, TEXT("component_class"), ComponentClass, Error)
		|| !MonolithLyra::TryReadIntParam(Params, TEXT("action_index"), ActionIndex, Error)
		|| !MonolithLyra::TryReadIntParam(Params, TEXT("component_index"), ComponentIndex, Error))
	{
		return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
	}
	if (ExperiencePath.IsEmpty() && ActionSetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Param 'experience_path' or 'action_set_path' is required"), MonolithLyra::ErrInvalidParams);
	}
	if (ActorClass.IsEmpty() && ComponentClass.IsEmpty() && ComponentIndex == INDEX_NONE)
	{
		return FMonolithActionResult::Error(TEXT("A removal selector is required: actor_class, component_class, or component_index"), MonolithLyra::ErrInvalidParams);
	}

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<MonolithLyra::FComponentRemovalCandidate> Candidates;
	TArray<UObject*> SaveTargets;

	if (!ExperiencePath.IsEmpty())
	{
		MonolithLyra::FResolvedLyraObject Experience;
		if (!MonolithLyra::TryResolveLyraObject(ExperiencePath, MonolithLyra::LyraExperienceDefinitionClassPath, Experience, Error))
		{
			return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
		}
		MonolithLyra::CollectComponentRemovalCandidates(
			Experience.Object,
			Experience.AssetForSave,
			Experience.ResolvedPath,
			ActionIndex,
			ActionName,
			ComponentIndex,
			ActorClass,
			ComponentClass,
			Candidates,
			Warnings);

		for (UObject* ActionSet : MonolithLyra::GetObjectArrayProperty(Experience.Object, TEXT("ActionSets")))
		{
			if (!ActionSet)
			{
				continue;
			}
			MonolithLyra::CollectComponentRemovalCandidates(
				ActionSet,
				ActionSet,
				ActionSet->GetPathName(),
				ActionIndex,
				ActionName,
				ComponentIndex,
				ActorClass,
				ComponentClass,
				Candidates,
				Warnings);
		}
	}

	if (!ActionSetPath.IsEmpty())
	{
		MonolithLyra::FResolvedLyraObject ActionSet;
		if (!MonolithLyra::TryResolveLyraObject(ActionSetPath, MonolithLyra::LyraExperienceActionSetClassPath, ActionSet, Error))
		{
			return FMonolithActionResult::Error(Error, MonolithLyra::ErrInvalidParams);
		}
		MonolithLyra::CollectComponentRemovalCandidates(
			ActionSet.Object,
			ActionSet.AssetForSave ? ActionSet.AssetForSave : ActionSet.Object,
			ActionSet.ResolvedPath,
			ActionIndex,
			ActionName,
			ComponentIndex,
			ActorClass,
			ComponentClass,
			Candidates,
			Warnings);
	}

	int32 RemovedCount = 0;
	if (!Options.bDryRun && Candidates.Num() > 0)
	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithLyra", "RemoveExperienceComponentEntry", "Monolith Lyra Remove Experience Component Entry"));
		Candidates.Sort([](const MonolithLyra::FComponentRemovalCandidate& A, const MonolithLyra::FComponentRemovalCandidate& B)
		{
			if (A.Action != B.Action)
			{
				return A.Action < B.Action;
			}
			return A.ComponentIndex > B.ComponentIndex;
		});

		for (const MonolithLyra::FComponentRemovalCandidate& Candidate : Candidates)
		{
			if (!Candidate.Action || !Candidate.Owner || !Candidate.ComponentListProperty)
			{
				continue;
			}
			FScriptArrayHelper Helper(Candidate.ComponentListProperty, Candidate.ComponentListProperty->ContainerPtrToValuePtr<void>(Candidate.Action));
			if (!Helper.IsValidIndex(Candidate.ComponentIndex))
			{
				continue;
			}
			Candidate.Owner->Modify();
			Candidate.Action->Modify();
			Helper.RemoveValues(Candidate.ComponentIndex);
			Candidate.Action->MarkPackageDirty();
			if (Candidate.AssetForSave)
			{
				Candidate.AssetForSave->MarkPackageDirty();
				SaveTargets.AddUnique(Candidate.AssetForSave);
			}
			++RemovedCount;
		}
	}

	bool bAnySaved = false;
	TArray<TSharedPtr<FJsonValue>> SavedRows;
	if (!Options.bDryRun && Options.bSave && RemovedCount > 0)
	{
		for (UObject* SaveTarget : SaveTargets)
		{
			bool bSaved = false;
			FString SavedPath;
			if (!MonolithLyra::SaveAssetIfRequested(SaveTarget, true, bSaved, SavedPath, Error))
			{
				return FMonolithActionResult::Error(Error);
			}
			bAnySaved |= bSaved;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("asset_path"), SaveTarget ? SaveTarget->GetPathName() : FString());
			Row->SetBoolField(TEXT("saved"), bSaved);
			Row->SetStringField(TEXT("saved_path"), SavedPath);
			SavedRows.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("lyra"));
	Result->SetStringField(TEXT("action"), TEXT("remove_experience_component_entry"));
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetBoolField(TEXT("confirm_received"), Options.bConfirm);
	Result->SetBoolField(TEXT("save_requested"), Options.bSave);
	Result->SetBoolField(TEXT("changed"), !Options.bDryRun && RemovedCount > 0);
	Result->SetBoolField(TEXT("saved"), bAnySaved);
	Result->SetNumberField(TEXT("candidate_count"), Candidates.Num());
	Result->SetNumberField(TEXT("removed_count"), RemovedCount);
	Result->SetArrayField(TEXT("candidates"), MonolithLyra::ComponentCandidatesToJson(Candidates));
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("saved_packages"), SavedRows);
	return FMonolithActionResult::Success(Result);
}
