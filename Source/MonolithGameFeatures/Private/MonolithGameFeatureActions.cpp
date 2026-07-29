#include "MonolithGameFeatureActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Modules/ModuleManager.h"
#include "MonolithParamSchema.h"

#ifndef WITH_MONOLITH_GAMEFEATURES
#define WITH_MONOLITH_GAMEFEATURES 0
#endif

#if WITH_MONOLITH_GAMEFEATURES

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/AssetManagerTypes.h"
#include "GameFeatureData.h"
#include "GameplayTagContainer.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MonolithSettings.h"
#include "PluginReferenceDescriptor.h"
#include "Serialization/JsonSerializer.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace MonolithGameFeatures
{
	static const FTopLevelAssetPath GameFeatureDataClassPath(TEXT("/Script/GameFeatures"), TEXT("GameFeatureData"));

	struct FGameFeaturePluginInfo
	{
		FString Name;
		FString FriendlyName;
		FString DescriptorPath;
		FString BaseDir;
		FString ContentDir;
		FString MountedAssetPath;
		bool bEnabled = false;
		bool bCanContainContent = false;
		bool bFromEngine = false;
		bool bLooksLikeGameFeature = false;
		bool bDeclaresGameFeaturesDependency = false;
		bool bDescriptorHasGameFeatureState = false;
		TSharedPtr<FJsonObject> DescriptorJson;
	};

	struct FAttributeSetGrantSpec
	{
		FString AttributeSetClass;
		FString InitializationData;
	};

	static int32 ClampLimit(double LimitValue)
	{
		return FMath::Clamp(static_cast<int32>(LimitValue), 1, 200);
	}

	static FString NormalizeFilename(FString Path)
	{
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	static FString RedactPath(const FString& Path)
	{
		FString Normalized = NormalizeFilename(Path);
		const FString ProjectDir = NormalizeFilename(FPaths::ProjectDir());
		const FString EngineDir = NormalizeFilename(FPaths::EngineDir());

		// FPaths::MakePathRelativeTo returns true even when the path is not a
		// descendant of the base (it emits leading "../" segments). Only treat
		// the result as project/engine-relative when it stays inside that root;
		// otherwise host filesystem segments (e.g. /Users/<name>/...) leak to
		// MCP clients. Anything outside both roots is replaced with a
		// non-revealing placeholder rather than the absolute path.
		FString Relative = Normalized;
		if (FPaths::MakePathRelativeTo(Relative, *ProjectDir) && !Relative.StartsWith(TEXT("..")))
		{
			return TEXT("<project>/") + Relative;
		}
		Relative = Normalized;
		if (FPaths::MakePathRelativeTo(Relative, *EngineDir) && !Relative.StartsWith(TEXT("..")))
		{
			return TEXT("<engine>/") + Relative;
		}
		return TEXT("<external>");
	}

	static bool JsonHasGameFeatureState(const TSharedPtr<FJsonObject>& DescriptorJson)
	{
		return DescriptorJson.IsValid()
			&& (DescriptorJson->HasField(TEXT("BuiltInInitialFeatureState"))
				|| DescriptorJson->HasField(TEXT("GameFeatureData"))
				|| DescriptorJson->HasField(TEXT("ExplicitlyLoaded")));
	}

	static bool DescriptorDeclaresGameFeaturesDependency(const TSharedRef<IPlugin>& Plugin)
	{
		for (const FPluginReferenceDescriptor& Dep : Plugin->GetDescriptor().Plugins)
		{
			if (Dep.Name == TEXT("GameFeatures") && Dep.bEnabled)
			{
				return true;
			}
		}
		return false;
	}

	static bool PathLooksLikeGameFeature(const FString& Path)
	{
		const FString Normalized = NormalizeFilename(Path).ToLower();
		return Normalized.Contains(TEXT("/plugins/gamefeatures/"));
	}

	static FGameFeaturePluginInfo MakePluginInfo(const TSharedRef<IPlugin>& Plugin)
	{
		FGameFeaturePluginInfo Info;
		Info.Name = Plugin->GetName();
		Info.FriendlyName = Plugin->GetFriendlyName();
		Info.DescriptorPath = Plugin->GetDescriptorFileName();
		Info.BaseDir = Plugin->GetBaseDir();
		Info.ContentDir = Plugin->GetContentDir();
		Info.MountedAssetPath = Plugin->GetMountedAssetPath();
		Info.bEnabled = Plugin->IsEnabled();
		Info.bCanContainContent = Plugin->CanContainContent();
		Info.bFromEngine = Plugin->GetLoadedFrom() == EPluginLoadedFrom::Engine;
#if WITH_EDITOR
		Info.DescriptorJson = Plugin->GetDescriptorJson();
#endif
		Info.bDeclaresGameFeaturesDependency = DescriptorDeclaresGameFeaturesDependency(Plugin);
		Info.bDescriptorHasGameFeatureState = JsonHasGameFeatureState(Info.DescriptorJson);
		Info.bLooksLikeGameFeature = Info.bDeclaresGameFeaturesDependency
			|| Info.bDescriptorHasGameFeatureState
			|| PathLooksLikeGameFeature(Info.DescriptorPath)
			|| PathLooksLikeGameFeature(Info.BaseDir);
		return Info;
	}

	static TArray<FGameFeaturePluginInfo> DiscoverPlugins(bool bIncludeEngine)
	{
		TArray<FGameFeaturePluginInfo> Plugins;
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			FGameFeaturePluginInfo Info = MakePluginInfo(Plugin);
			if (!Info.bLooksLikeGameFeature)
			{
				continue;
			}
			if (Info.bFromEngine && !bIncludeEngine)
			{
				continue;
			}
			Plugins.Add(MoveTemp(Info));
		}
		Plugins.Sort([](const FGameFeaturePluginInfo& A, const FGameFeaturePluginInfo& B)
		{
			return A.Name < B.Name;
		});
		return Plugins;
	}

	static TArray<FAssetData> GetGameFeatureDataAssets(const FString& PackagePath = FString())
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(GameFeatureDataClassPath);
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		if (!PackagePath.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*PackagePath));
		}

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssets(Filter, Assets);

		// Filter.bRecursiveClasses already restricts results to GameFeatureData
		// and its subclasses. An exact-class post-filter would drop project
		// defined UGameFeatureData subclasses, making valid plugins look like
		// they ship no GameFeatureData asset, so no further filtering is done.

		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});
		return Assets;
	}

	static FString NormalizeAssetPath(FString AssetPath)
	{
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty())
		{
			return FString();
		}
		if (AssetPath.Contains(TEXT(".")))
		{
			FString PackagePart;
			FString ObjectPart;
			AssetPath.Split(TEXT("."), &PackagePart, &ObjectPart, ESearchCase::CaseSensitive, ESearchDir::FromStart);
			return PackagePart;
		}
		return AssetPath;
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

	static FString NormalizeSoftObjectPathForCompare(const FString& AssetPath)
	{
		const FString ObjectPath = NormalizeObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		return FSoftObjectPath(ObjectPath).GetAssetPathString().ToLower();
	}

	static bool TryGetRequiredStringParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		FString& OutValue,
		FString& OutError)
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

	static bool TryReadOptionalBoolParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool& InOutValue,
		FString& OutError)
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

	static bool TryReadOptionalIntParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32& InOutValue,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double NumberValue = 0.0;
		if (!Params->TryGetNumberField(FieldName, NumberValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an integer"), FieldName);
			return false;
		}
		const int32 IntegerValue = static_cast<int32>(NumberValue);
		if (!FMath::IsNearlyEqual(NumberValue, static_cast<double>(IntegerValue)))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an integer"), FieldName);
			return false;
		}
		InOutValue = IntegerValue;
		return true;
	}

	static bool TryReadStringArrayParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
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

	static bool TryReadAttributeSetGrantArrayParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<FAttributeSetGrantSpec>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of objects"), FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of objects"), FieldName);
				return false;
			}

			FAttributeSetGrantSpec Spec;
			if (!TryGetRequiredStringParam(Entry, TEXT("attribute_set_class"), Spec.AttributeSetClass, OutError))
			{
				return false;
			}
			Entry->TryGetStringField(TEXT("initialization_data"), Spec.InitializationData);
			Spec.InitializationData.TrimStartAndEndInline();

			const bool bAlreadyPresent = OutValues.ContainsByPredicate([&Spec](const FAttributeSetGrantSpec& Existing)
			{
				return Existing.AttributeSetClass.Equals(Spec.AttributeSetClass, ESearchCase::IgnoreCase)
					&& Existing.InitializationData.Equals(Spec.InitializationData, ESearchCase::IgnoreCase);
			});
			if (!bAlreadyPresent)
			{
				OutValues.Add(MoveTemp(Spec));
			}
		}
		return true;
	}

	static FString NormalizeSoftClassPath(FString ClassPath)
	{
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty() || ClassPath.StartsWith(TEXT("/Script/")))
		{
			return ClassPath;
		}
		if (!ClassPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(ClassPath);
			if (!AssetName.IsEmpty())
			{
				ClassPath = ClassPath + TEXT(".") + AssetName;
			}
		}
		if (ClassPath.Contains(TEXT(".")) && !ClassPath.EndsWith(TEXT("_C")))
		{
			ClassPath += TEXT("_C");
		}
		return ClassPath;
	}

	static UClass* LoadClassForParam(
		const FString& ClassPath,
		const TCHAR* ParamName,
		const TCHAR* ExpectedBaseClassPath,
		FString& OutError)
	{
		const FString DesiredPath = NormalizeSoftClassPath(ClassPath);
		if (DesiredPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Class path for '%s' must not be empty"), ParamName);
			return nullptr;
		}

		UClass* LoadedClass = StaticLoadClass(UObject::StaticClass(), nullptr, *DesiredPath);
		if (!LoadedClass)
		{
			OutError = FString::Printf(TEXT("Could not load class '%s' for '%s'"), *DesiredPath, ParamName);
			return nullptr;
		}

		if (ExpectedBaseClassPath && *ExpectedBaseClassPath)
		{
			if (UClass* BaseClass = StaticLoadClass(UObject::StaticClass(), nullptr, ExpectedBaseClassPath))
			{
				if (!LoadedClass->IsChildOf(BaseClass))
				{
					OutError = FString::Printf(
						TEXT("Class '%s' for '%s' is not a child of '%s'"),
						*LoadedClass->GetPathName(),
						ParamName,
						*BaseClass->GetPathName());
					return nullptr;
				}
			}
		}

		return LoadedClass;
	}

	static bool TrySetGameplayTagProperty(
		void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		const FString& TagName,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		FStructProperty* TagProperty = FindFProperty<FStructProperty>(Struct, FieldName);
		if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose FGameplayTag field '%s'"), *Struct->GetName(), FieldName);
			return false;
		}
		FGameplayTag DesiredTag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
		if (!DesiredTag.IsValid())
		{
			OutError = FString::Printf(TEXT("Gameplay tag '%s' is not registered"), *TagName);
			return false;
		}
		FGameplayTag* ExistingTag = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(StructPtr);
		if (*ExistingTag != DesiredTag)
		{
			*ExistingTag = DesiredTag;
			bOutChanged = true;
		}
		return true;
	}

	static bool TrySetSoftClassProperty(
		void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		const FString& ClassPath,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		const FString DesiredPath = NormalizeSoftClassPath(ClassPath);
		if (DesiredPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Class path for '%s' must not be empty"), FieldName);
			return false;
		}
		if (!StaticLoadClass(UObject::StaticClass(), nullptr, *DesiredPath))
		{
			OutError = FString::Printf(TEXT("Could not load class '%s'"), *DesiredPath);
			return false;
		}

		if (FSoftClassProperty* SoftClassProperty = FindFProperty<FSoftClassProperty>(Struct, FieldName))
		{
			void* ValuePtr = SoftClassProperty->ContainerPtrToValuePtr<void>(StructPtr);
			const FSoftObjectPtr ExistingPtr = SoftClassProperty->GetPropertyValue(ValuePtr);
			if (!ExistingPtr.ToSoftObjectPath().ToString().Equals(DesiredPath, ESearchCase::IgnoreCase))
			{
				SoftClassProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(DesiredPath)));
				bOutChanged = true;
			}
			return true;
		}

		if (FSoftObjectProperty* SoftObjectProperty = FindFProperty<FSoftObjectProperty>(Struct, FieldName))
		{
			void* ValuePtr = SoftObjectProperty->ContainerPtrToValuePtr<void>(StructPtr);
			const FSoftObjectPtr ExistingPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			if (!ExistingPtr.ToSoftObjectPath().ToString().Equals(DesiredPath, ESearchCase::IgnoreCase))
			{
				SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(DesiredPath)));
				bOutChanged = true;
			}
			return true;
		}

		OutError = FString::Printf(TEXT("Struct '%s' must expose soft class/object field '%s'"), *Struct->GetName(), FieldName);
		return false;
	}

	static bool TrySetSoftObjectProperty(
		void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		const FString& ObjectPath,
		bool bAllowEmpty,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		FSoftObjectProperty* SoftObjectProperty = FindFProperty<FSoftObjectProperty>(Struct, FieldName);
		if (!SoftObjectProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose soft object field '%s'"), *Struct->GetName(), FieldName);
			return false;
		}

		const FString DesiredPath = NormalizeObjectPath(ObjectPath);
		if (DesiredPath.IsEmpty())
		{
			if (!bAllowEmpty)
			{
				OutError = FString::Printf(TEXT("Object path for '%s' must not be empty"), FieldName);
				return false;
			}

			void* ValuePtr = SoftObjectProperty->ContainerPtrToValuePtr<void>(StructPtr);
			const FSoftObjectPtr ExistingPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			if (!ExistingPtr.IsNull())
			{
				SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr());
				bOutChanged = true;
			}
			return true;
		}

		UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *DesiredPath);
		if (!LoadedObject)
		{
			OutError = FString::Printf(TEXT("Could not load object '%s'"), *DesiredPath);
			return false;
		}
		if (SoftObjectProperty->PropertyClass && !LoadedObject->IsA(SoftObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(
				TEXT("Object '%s' for '%s' is not a '%s'"),
				*LoadedObject->GetPathName(),
				FieldName,
				*SoftObjectProperty->PropertyClass->GetPathName());
			return false;
		}

		void* ValuePtr = SoftObjectProperty->ContainerPtrToValuePtr<void>(StructPtr);
		const FSoftObjectPtr ExistingPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
		if (!NormalizeSoftObjectPathForCompare(ExistingPtr.ToSoftObjectPath().ToString()).Equals(
			NormalizeSoftObjectPathForCompare(DesiredPath),
			ESearchCase::IgnoreCase))
		{
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(DesiredPath)));
			bOutChanged = true;
		}
		return true;
	}

	static bool TryGetSoftClassPropertyPath(
		const void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		FString& OutPath,
		FString& OutError)
	{
		OutPath.Reset();
		if (FSoftClassProperty* SoftClassProperty = FindFProperty<FSoftClassProperty>(Struct, FieldName))
		{
			const void* ValuePtr = SoftClassProperty->ContainerPtrToValuePtr<void>(StructPtr);
			OutPath = SoftClassProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString();
			return true;
		}
		if (FSoftObjectProperty* SoftObjectProperty = FindFProperty<FSoftObjectProperty>(Struct, FieldName))
		{
			const void* ValuePtr = SoftObjectProperty->ContainerPtrToValuePtr<void>(StructPtr);
			OutPath = SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString();
			return true;
		}
		OutError = FString::Printf(TEXT("Struct '%s' must expose soft class/object field '%s'"), *Struct->GetName(), FieldName);
		return false;
	}

	static bool TryGetSoftObjectPropertyPath(
		const void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		FString& OutPath,
		FString& OutError)
	{
		OutPath.Reset();
		if (FSoftObjectProperty* SoftObjectProperty = FindFProperty<FSoftObjectProperty>(Struct, FieldName))
		{
			const void* ValuePtr = SoftObjectProperty->ContainerPtrToValuePtr<void>(StructPtr);
			OutPath = SoftObjectProperty->GetPropertyValue(ValuePtr).ToSoftObjectPath().ToString();
			return true;
		}
		OutError = FString::Printf(TEXT("Struct '%s' must expose soft object field '%s'"), *Struct->GetName(), FieldName);
		return false;
	}

	static bool TrySetBoolProperty(
		void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		bool bDesiredValue,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		FBoolProperty* BoolProperty = FindFProperty<FBoolProperty>(Struct, FieldName);
		if (!BoolProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose bool field '%s'"), *Struct->GetName(), FieldName);
			return false;
		}

		void* ValuePtr = BoolProperty->ContainerPtrToValuePtr<void>(StructPtr);
		const bool bCurrentValue = BoolProperty->GetPropertyValue(ValuePtr);
		if (bCurrentValue != bDesiredValue)
		{
			BoolProperty->SetPropertyValue(ValuePtr, bDesiredValue);
			bOutChanged = true;
		}
		return true;
	}

	static bool TrySetIntegerProperty(
		void* StructPtr,
		UStruct* Struct,
		const TCHAR* FieldName,
		int32 DesiredValue,
		bool& bOutChanged,
		FString& OutError)
	{
		bOutChanged = false;
		FNumericProperty* NumericProperty = FindFProperty<FNumericProperty>(Struct, FieldName);
		if (!NumericProperty || !NumericProperty->IsInteger())
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose integer field '%s'"), *Struct->GetName(), FieldName);
			return false;
		}

		void* ValuePtr = NumericProperty->ContainerPtrToValuePtr<void>(StructPtr);
		const int64 CurrentValue = FCString::Atoi64(*NumericProperty->GetNumericPropertyValueToString(ValuePtr));
		if (CurrentValue != DesiredValue)
		{
			if (DesiredValue < 0)
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(DesiredValue));
			}
			else
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<uint64>(DesiredValue));
			}
			bOutChanged = true;
		}
		return true;
	}



	static UObject* LoadAssetObject(const FString& AssetPath, FString& OutError)
	{
		const FString ObjectPath = NormalizeObjectPath(AssetPath);
		UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Could not load asset object '%s'"), *AssetPath);
			return nullptr;
		}
		return Asset;
	}

	static UGameFeatureData* LoadGameFeatureDataAsset(const FString& AssetPath, FString& OutError)
	{
		UObject* Asset = LoadAssetObject(AssetPath, OutError);
		UGameFeatureData* GameFeatureData = Cast<UGameFeatureData>(Asset);
		if (!GameFeatureData && OutError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a UGameFeatureData"), *AssetPath);
		}
		return GameFeatureData;
	}

	static UClass* LoadActionClass(const FString& ClassPath, FString& OutError)
	{
		UClass* ActionClass = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassPath);
		if (!ActionClass)
		{
			OutError = FString::Printf(TEXT("Could not load GameFeatureAction class '%s'"), *ClassPath);
			return nullptr;
		}
		if (ActionClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = FString::Printf(TEXT("GameFeatureAction class '%s' is abstract"), *ClassPath);
			return nullptr;
		}
		return ActionClass;
	}

	static bool SaveAssetIfRequested(UObject* Asset, bool bSave, bool& bSaved, FString& OutError)
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
			OutError = FString::Printf(TEXT("Asset has no package: %s"), *Asset->GetPathName());
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

	static bool TryGetActionsArray(
		UObject* ActionSet,
		FArrayProperty*& OutArrayProperty,
		FObjectPropertyBase*& OutObjectProperty,
		FString& OutError)
	{
		if (!ActionSet)
		{
			OutError = TEXT("Action set asset is null");
			return false;
		}

		OutArrayProperty = FindFProperty<FArrayProperty>(ActionSet->GetClass(), TEXT("Actions"));
		if (!OutArrayProperty)
		{
			OutError = FString::Printf(TEXT("Asset '%s' does not expose an Actions array"), *ActionSet->GetPathName());
			return false;
		}

		OutObjectProperty = CastField<FObjectPropertyBase>(OutArrayProperty->Inner);
		if (!OutObjectProperty)
		{
			OutError = FString::Printf(TEXT("Actions array on '%s' is not an object reference array"), *ActionSet->GetPathName());
			return false;
		}

		return true;
	}

	static int32 RemoveNullActions(FScriptArrayHelper& Helper, const FObjectPropertyBase* ObjectProperty)
	{
		int32 RemovedCount = 0;
		for (int32 Index = Helper.Num() - 1; Index >= 0; --Index)
		{
			if (!ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
			{
				Helper.RemoveValues(Index);
				++RemovedCount;
			}
		}
		return RemovedCount;
	}

	static UObject* FindExistingAction(
		FScriptArrayHelper& Helper,
		const FObjectPropertyBase* ObjectProperty,
		UClass* ActionClass,
		const FString& RequestedActionName,
		int32& OutIndex)
	{
		OutIndex = INDEX_NONE;

		if (!RequestedActionName.IsEmpty())
		{
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				UObject* ActionObject = ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index));
				if (ActionObject && ActionObject->GetName().Equals(RequestedActionName, ESearchCase::IgnoreCase))
				{
					OutIndex = Index;
					return ActionObject;
				}
			}
		}

		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			UObject* ActionObject = ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index));
			if (ActionObject && ActionObject->IsA(ActionClass))
			{
				OutIndex = Index;
				return ActionObject;
			}
		}

		return nullptr;
	}

	static bool EnsureInstancedActionObject(
		UObject* ActionOwner,
		UClass* ActionClass,
		const FString& ActionName,
		bool bRemoveNullActions,
		bool bDryRun,
		UObject*& OutActionObject,
		bool& bOutCreatedAction,
		int32& OutActionIndex,
		int32& OutRemovedNullActionCount,
		int32& OutActionCountBefore,
		int32& OutActionCountAfter,
		FString& OutError)
	{
		OutActionObject = nullptr;
		bOutCreatedAction = false;
		OutActionIndex = INDEX_NONE;
		OutRemovedNullActionCount = 0;
		OutActionCountBefore = 0;
		OutActionCountAfter = 0;

		if (!ActionOwner || !ActionClass)
		{
			OutError = TEXT("Action owner and class are required");
			return false;
		}

		FArrayProperty* ActionsArrayProperty = nullptr;
		FObjectPropertyBase* ActionsObjectProperty = nullptr;
		if (!TryGetActionsArray(ActionOwner, ActionsArrayProperty, ActionsObjectProperty, OutError))
		{
			return false;
		}
		if (ActionsObjectProperty->PropertyClass && !ActionClass->IsChildOf(ActionsObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(
				TEXT("Action class '%s' is not compatible with Actions element class '%s'"),
				*ActionClass->GetPathName(),
				*ActionsObjectProperty->PropertyClass->GetPathName());
			return false;
		}

		void* ActionsArrayPtr = ActionsArrayProperty->ContainerPtrToValuePtr<void>(ActionOwner);
		FScriptArrayHelper ActionsHelper(ActionsArrayProperty, ActionsArrayPtr);
		OutActionCountBefore = ActionsHelper.Num();

		OutActionObject = FindExistingAction(ActionsHelper, ActionsObjectProperty, ActionClass, ActionName, OutActionIndex);
		if (!OutActionObject)
		{
			bOutCreatedAction = true;
			if (!bDryRun)
			{
				ActionOwner->Modify();
				const FName BaseName = ActionName.IsEmpty() ? FName(*ActionClass->GetName()) : FName(*ActionName);
				const FName UniqueName = MakeUniqueObjectName(ActionOwner, ActionClass, BaseName);
				OutActionObject = NewObject<UObject>(ActionOwner, ActionClass, UniqueName, RF_Transactional);
				if (!OutActionObject)
				{
					OutError = TEXT("Failed to create instanced GameFeatureAction object");
					return false;
				}
				OutActionObject->Modify();
				const int32 NewIndex = ActionsHelper.AddValue();
				ActionsObjectProperty->SetObjectPropertyValue(ActionsHelper.GetRawPtr(NewIndex), OutActionObject);
				OutActionIndex = NewIndex;
			}
		}

		if (bRemoveNullActions && !bDryRun)
		{
			ActionOwner->Modify();
			OutRemovedNullActionCount = RemoveNullActions(ActionsHelper, ActionsObjectProperty);
			if (OutRemovedNullActionCount > 0 && OutActionObject)
			{
				OutActionIndex = INDEX_NONE;
				for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
				{
					if (ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index)) == OutActionObject)
					{
						OutActionIndex = Index;
						break;
					}
				}
			}
		}
		else if (bRemoveNullActions)
		{
			for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
			{
				if (!ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index)))
				{
					++OutRemovedNullActionCount;
				}
			}
		}

		OutActionCountAfter = bDryRun
			? OutActionCountBefore + (bOutCreatedAction ? 1 : 0) - OutRemovedNullActionCount
			: ActionsHelper.Num();
		return true;
	}

	static bool EnsureInputMappingEntry(
		UObject* ActionObject,
		const FString& MappingContextPath,
		int32 Priority,
		bool bDryRun,
		bool& bAddedMapping,
		bool& bUpdatedPriority,
		int32& MappingCountBefore,
		int32& MappingCountAfter,
		FString& OutError)
	{
		bAddedMapping = false;
		bUpdatedPriority = false;
		MappingCountBefore = 0;
		MappingCountAfter = 0;

		if (!ActionObject)
		{
			OutError = TEXT("Action object is null");
			return false;
		}

		FArrayProperty* InputMappingsProperty = FindFProperty<FArrayProperty>(ActionObject->GetClass(), TEXT("InputMappings"));
		if (!InputMappingsProperty)
		{
			OutError = FString::Printf(TEXT("Action class '%s' does not expose an InputMappings array"), *ActionObject->GetClass()->GetPathName());
			return false;
		}

		FStructProperty* MappingStructProperty = CastField<FStructProperty>(InputMappingsProperty->Inner);
		if (!MappingStructProperty)
		{
			OutError = FString::Printf(TEXT("InputMappings on '%s' is not a struct array"), *ActionObject->GetPathName());
			return false;
		}

		FSoftObjectProperty* InputMappingProperty = FindFProperty<FSoftObjectProperty>(MappingStructProperty->Struct, TEXT("InputMapping"));
		FNumericProperty* PriorityProperty = FindFProperty<FNumericProperty>(MappingStructProperty->Struct, TEXT("Priority"));
		if (!InputMappingProperty || !PriorityProperty)
		{
			OutError = FString::Printf(TEXT("InputMappings struct on '%s' must expose InputMapping soft object and Priority integer fields"), *ActionObject->GetPathName());
			return false;
		}
		if (!PriorityProperty->IsInteger())
		{
			OutError = FString::Printf(TEXT("InputMappings Priority field on '%s' is not an integer"), *ActionObject->GetPathName());
			return false;
		}

		const FString DesiredObjectPath = NormalizeObjectPath(MappingContextPath);
		const FString DesiredComparePath = NormalizeSoftObjectPathForCompare(DesiredObjectPath);
		if (DesiredComparePath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid mapping_context_path '%s'"), *MappingContextPath);
			return false;
		}

		UObject* MappingContext = StaticLoadObject(UObject::StaticClass(), nullptr, *DesiredObjectPath);
		if (!MappingContext)
		{
			OutError = FString::Printf(TEXT("Could not load InputMappingContext '%s'"), *MappingContextPath);
			return false;
		}

		if (UClass* InputMappingContextClass = StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Script/EnhancedInput.InputMappingContext")))
		{
			if (!MappingContext->IsA(InputMappingContextClass))
			{
				OutError = FString::Printf(TEXT("Asset '%s' is not an InputMappingContext"), *MappingContext->GetPathName());
				return false;
			}
		}

		void* ArrayPtr = InputMappingsProperty->ContainerPtrToValuePtr<void>(ActionObject);
		FScriptArrayHelper Helper(InputMappingsProperty, ArrayPtr);
		MappingCountBefore = Helper.Num();

		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* StructPtr = Helper.GetRawPtr(Index);
			void* InputValuePtr = InputMappingProperty->ContainerPtrToValuePtr<void>(StructPtr);
			const FSoftObjectPtr ExistingPtr = InputMappingProperty->GetPropertyValue(InputValuePtr);
			const FString ExistingComparePath = NormalizeSoftObjectPathForCompare(ExistingPtr.ToSoftObjectPath().ToString());
			if (!ExistingComparePath.Equals(DesiredComparePath, ESearchCase::IgnoreCase))
			{
				continue;
			}

			void* PriorityValuePtr = PriorityProperty->ContainerPtrToValuePtr<void>(StructPtr);
			const int64 CurrentPriority = PriorityProperty->GetSignedIntPropertyValue(PriorityValuePtr);
			if (CurrentPriority != Priority)
			{
				bUpdatedPriority = true;
				if (!bDryRun)
				{
					ActionObject->Modify();
					PriorityProperty->SetIntPropertyValue(PriorityValuePtr, static_cast<int64>(Priority));
				}
			}
			MappingCountAfter = Helper.Num();
			return true;
		}

		bAddedMapping = true;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* StructPtr = Helper.GetRawPtr(NewIndex);
			void* InputValuePtr = InputMappingProperty->ContainerPtrToValuePtr<void>(StructPtr);
			void* PriorityValuePtr = PriorityProperty->ContainerPtrToValuePtr<void>(StructPtr);
			InputMappingProperty->SetPropertyValue(InputValuePtr, FSoftObjectPtr(FSoftObjectPath(DesiredObjectPath)));
			PriorityProperty->SetIntPropertyValue(PriorityValuePtr, static_cast<int64>(Priority));
			MappingCountAfter = Helper.Num();
		}
		else
		{
			MappingCountAfter = Helper.Num() + 1;
		}

		return true;
	}

	static bool ReadWidgetEntryArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<TSharedPtr<FJsonObject>>& OutEntries,
		FString& OutError)
	{
		OutEntries.Reset();
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of objects"), FieldName);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of objects"), FieldName);
				return false;
			}
			OutEntries.Add(Entry);
		}
		return true;
	}

	static bool EnsureSoftClassTagStructEntry(
		UObject* ActionObject,
		const TCHAR* ArrayPropertyName,
		const TCHAR* ClassPropertyName,
		const TCHAR* TagPropertyName,
		const FString& ClassPath,
		const FString& TagName,
		bool bDryRun,
		bool& bOutAdded,
		bool& bOutUpdated,
		int32& OutCountBefore,
		int32& OutCountAfter,
		FString& OutError)
	{
		bOutAdded = false;
		bOutUpdated = false;
		OutCountBefore = 0;
		OutCountAfter = 0;

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(ActionObject->GetClass(), ArrayPropertyName);
		if (!ArrayProperty)
		{
			OutError = FString::Printf(TEXT("Action class '%s' does not expose array '%s'"), *ActionObject->GetClass()->GetPathName(), ArrayPropertyName);
			return false;
		}
		FStructProperty* StructProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		if (!StructProperty)
		{
			OutError = FString::Printf(TEXT("Action array '%s' on '%s' is not a struct array"), ArrayPropertyName, *ActionObject->GetPathName());
			return false;
		}

		const FString DesiredClassPath = NormalizeSoftClassPath(ClassPath);
		const FGameplayTag DesiredTag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
		if (!DesiredTag.IsValid())
		{
			OutError = FString::Printf(TEXT("Gameplay tag '%s' is not registered"), *TagName);
			return false;
		}

		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(ActionObject);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		OutCountBefore = Helper.Num();

		FStructProperty* TagProperty = FindFProperty<FStructProperty>(StructProperty->Struct, TagPropertyName);
		if (!TagProperty || TagProperty->Struct != FGameplayTag::StaticStruct())
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose FGameplayTag field '%s'"), *StructProperty->Struct->GetName(), TagPropertyName);
			return false;
		}

		FProperty* ClassProperty = FindFProperty<FProperty>(StructProperty->Struct, ClassPropertyName);
		if (!ClassProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose field '%s'"), *StructProperty->Struct->GetName(), ClassPropertyName);
			return false;
		}

		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* StructPtr = Helper.GetRawPtr(Index);
			const FGameplayTag* ExistingTag = TagProperty->ContainerPtrToValuePtr<FGameplayTag>(StructPtr);
			if (!ExistingTag || *ExistingTag != DesiredTag)
			{
				continue;
			}

			FString ExistingClassPath;
			if (!TryGetSoftClassPropertyPath(StructPtr, StructProperty->Struct, ClassPropertyName, ExistingClassPath, OutError))
			{
				return false;
			}
			const bool bClassChanged = !ExistingClassPath.Equals(DesiredClassPath, ESearchCase::IgnoreCase);
			bOutUpdated = bClassChanged;
			OutCountAfter = Helper.Num();
			if (bClassChanged && !bDryRun)
			{
				ActionObject->Modify();
				bool bAppliedClassChange = false;
				if (!TrySetSoftClassProperty(StructPtr, StructProperty->Struct, ClassPropertyName, DesiredClassPath, bAppliedClassChange, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bOutAdded = true;
		OutCountAfter = Helper.Num() + 1;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* StructPtr = Helper.GetRawPtr(NewIndex);
			StructProperty->InitializeValue(StructPtr);

			bool bClassChanged = false;
			bool bTagChanged = false;
			if (!TrySetSoftClassProperty(StructPtr, StructProperty->Struct, ClassPropertyName, DesiredClassPath, bClassChanged, OutError)
				|| !TrySetGameplayTagProperty(StructPtr, StructProperty->Struct, TagPropertyName, TagName, bTagChanged, OutError))
			{
				return false;
			}
			OutCountAfter = Helper.Num();
		}
		return true;
	}

	static bool EnsureComponentListEntry(
		UObject* ActionObject,
		const FString& ActorClassPath,
		const FString& ComponentClassPath,
		bool bClientComponent,
		bool bServerComponent,
		int32 AdditionFlags,
		bool bDryRun,
		bool& bOutAdded,
		bool& bOutUpdated,
		int32& OutCountBefore,
		int32& OutCountAfter,
		FString& OutError)
	{
		bOutAdded = false;
		bOutUpdated = false;
		OutCountBefore = 0;
		OutCountAfter = 0;

		if (!ActionObject)
		{
			OutError = TEXT("Action object is null");
			return false;
		}

		FArrayProperty* ComponentListProperty = FindFProperty<FArrayProperty>(ActionObject->GetClass(), TEXT("ComponentList"));
		if (!ComponentListProperty)
		{
			OutError = FString::Printf(TEXT("Action class '%s' does not expose a ComponentList array"), *ActionObject->GetClass()->GetPathName());
			return false;
		}
		FStructProperty* EntryStructProperty = CastField<FStructProperty>(ComponentListProperty->Inner);
		if (!EntryStructProperty)
		{
			OutError = FString::Printf(TEXT("ComponentList on '%s' is not a struct array"), *ActionObject->GetPathName());
			return false;
		}

		const FString DesiredActorClassPath = NormalizeSoftClassPath(ActorClassPath);
		const FString DesiredComponentClassPath = NormalizeSoftClassPath(ComponentClassPath);
		if (!LoadClassForParam(DesiredActorClassPath, TEXT("actor_class"), TEXT("/Script/Engine.Actor"), OutError)
			|| !LoadClassForParam(DesiredComponentClassPath, TEXT("component_class"), TEXT("/Script/Engine.ActorComponent"), OutError))
		{
			return false;
		}

		void* ArrayPtr = ComponentListProperty->ContainerPtrToValuePtr<void>(ActionObject);
		FScriptArrayHelper Helper(ComponentListProperty, ArrayPtr);
		OutCountBefore = Helper.Num();
		OutCountAfter = Helper.Num();

		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* EntryPtr = Helper.GetRawPtr(Index);
			FString ExistingActorClassPath;
			FString ExistingComponentClassPath;
			if (!TryGetSoftClassPropertyPath(EntryPtr, EntryStructProperty->Struct, TEXT("ActorClass"), ExistingActorClassPath, OutError)
				|| !TryGetSoftClassPropertyPath(EntryPtr, EntryStructProperty->Struct, TEXT("ComponentClass"), ExistingComponentClassPath, OutError))
			{
				return false;
			}

			const bool bSameActor = NormalizeSoftObjectPathForCompare(ExistingActorClassPath).Equals(
				NormalizeSoftObjectPathForCompare(DesiredActorClassPath),
				ESearchCase::IgnoreCase);
			const bool bSameComponent = NormalizeSoftObjectPathForCompare(ExistingComponentClassPath).Equals(
				NormalizeSoftObjectPathForCompare(DesiredComponentClassPath),
				ESearchCase::IgnoreCase);
			if (!bSameActor || !bSameComponent)
			{
				continue;
			}

			FBoolProperty* ClientProperty = FindFProperty<FBoolProperty>(EntryStructProperty->Struct, TEXT("bClientComponent"));
			FBoolProperty* ServerProperty = FindFProperty<FBoolProperty>(EntryStructProperty->Struct, TEXT("bServerComponent"));
			FNumericProperty* FlagsProperty = FindFProperty<FNumericProperty>(EntryStructProperty->Struct, TEXT("AdditionFlags"));
			if (!ClientProperty || !ServerProperty || !FlagsProperty || !FlagsProperty->IsInteger())
			{
				OutError = FString::Printf(TEXT("Struct '%s' must expose bClientComponent, bServerComponent, and AdditionFlags fields"), *EntryStructProperty->Struct->GetName());
				return false;
			}

			const bool bClientChanged = ClientProperty->GetPropertyValue(ClientProperty->ContainerPtrToValuePtr<void>(EntryPtr)) != bClientComponent;
			const bool bServerChanged = ServerProperty->GetPropertyValue(ServerProperty->ContainerPtrToValuePtr<void>(EntryPtr)) != bServerComponent;
			const bool bFlagsChanged = FCString::Atoi64(*FlagsProperty->GetNumericPropertyValueToString(FlagsProperty->ContainerPtrToValuePtr<void>(EntryPtr))) != AdditionFlags;
			bOutUpdated = bClientChanged || bServerChanged || bFlagsChanged;
			if (bOutUpdated && !bDryRun)
			{
				ActionObject->Modify();
				bool bAppliedClient = false;
				bool bAppliedServer = false;
				bool bAppliedFlags = false;
				if (!TrySetBoolProperty(EntryPtr, EntryStructProperty->Struct, TEXT("bClientComponent"), bClientComponent, bAppliedClient, OutError)
					|| !TrySetBoolProperty(EntryPtr, EntryStructProperty->Struct, TEXT("bServerComponent"), bServerComponent, bAppliedServer, OutError)
					|| !TrySetIntegerProperty(EntryPtr, EntryStructProperty->Struct, TEXT("AdditionFlags"), AdditionFlags, bAppliedFlags, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bOutAdded = true;
		OutCountAfter = Helper.Num() + 1;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* EntryPtr = Helper.GetRawPtr(NewIndex);
			EntryStructProperty->InitializeValue(EntryPtr);

			bool bActorChanged = false;
			bool bComponentChanged = false;
			bool bClientChanged = false;
			bool bServerChanged = false;
			bool bFlagsChanged = false;
			if (!TrySetSoftClassProperty(EntryPtr, EntryStructProperty->Struct, TEXT("ActorClass"), DesiredActorClassPath, bActorChanged, OutError)
				|| !TrySetSoftClassProperty(EntryPtr, EntryStructProperty->Struct, TEXT("ComponentClass"), DesiredComponentClassPath, bComponentChanged, OutError)
				|| !TrySetBoolProperty(EntryPtr, EntryStructProperty->Struct, TEXT("bClientComponent"), bClientComponent, bClientChanged, OutError)
				|| !TrySetBoolProperty(EntryPtr, EntryStructProperty->Struct, TEXT("bServerComponent"), bServerComponent, bServerChanged, OutError)
				|| !TrySetIntegerProperty(EntryPtr, EntryStructProperty->Struct, TEXT("AdditionFlags"), AdditionFlags, bFlagsChanged, OutError))
			{
				return false;
			}
			OutCountAfter = Helper.Num();
		}
		return true;
	}

	static bool EnsureDirectoryPathArrayValues(
		UObject* ActionObject,
		const FString& ArrayPropertyName,
		const TArray<FString>& DirectoryPaths,
		bool bDryRun,
		int32& OutAddedCount,
		int32& OutCountBefore,
		int32& OutCountAfter,
		TArray<TSharedPtr<FJsonValue>>& OutEntries,
		FString& OutError)
	{
		OutAddedCount = 0;
		OutCountBefore = 0;
		OutCountAfter = 0;
		OutEntries.Reset();

		FArrayProperty* DirectoryArrayProperty = FindFProperty<FArrayProperty>(ActionObject->GetClass(), *ArrayPropertyName);
		if (!DirectoryArrayProperty)
		{
			OutError = FString::Printf(TEXT("Action class '%s' does not expose array '%s'"), *ActionObject->GetClass()->GetPathName(), *ArrayPropertyName);
			return false;
		}
		FStructProperty* DirectoryStructProperty = CastField<FStructProperty>(DirectoryArrayProperty->Inner);
		if (!DirectoryStructProperty)
		{
			OutError = FString::Printf(TEXT("Array '%s' on '%s' is not a struct array"), *ArrayPropertyName, *ActionObject->GetPathName());
			return false;
		}
		FStrProperty* PathProperty = FindFProperty<FStrProperty>(DirectoryStructProperty->Struct, TEXT("Path"));
		if (!PathProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose string field 'Path'"), *DirectoryStructProperty->Struct->GetName());
			return false;
		}

		void* ArrayPtr = DirectoryArrayProperty->ContainerPtrToValuePtr<void>(ActionObject);
		FScriptArrayHelper Helper(DirectoryArrayProperty, ArrayPtr);
		OutCountBefore = Helper.Num();
		OutCountAfter = Helper.Num();

		TSet<FString> PlannedPaths;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const void* EntryPtr = Helper.GetRawPtr(Index);
			const FString ExistingPath = PathProperty->GetPropertyValue_InContainer(EntryPtr);
			PlannedPaths.Add(ExistingPath.ToLower());
		}

		for (FString DirectoryPath : DirectoryPaths)
		{
			DirectoryPath.TrimStartAndEndInline();
			if (DirectoryPath.IsEmpty() || !DirectoryPath.StartsWith(TEXT("/")))
			{
				OutError = FString::Printf(TEXT("GameplayCue directory path '%s' must be a slash-prefixed content path"), *DirectoryPath);
				return false;
			}

			const FString ComparePath = DirectoryPath.ToLower();
			const bool bAlreadyPresent = PlannedPaths.Contains(ComparePath);
			if (!bAlreadyPresent)
			{
				PlannedPaths.Add(ComparePath);
				++OutAddedCount;
				++OutCountAfter;
				if (!bDryRun)
				{
					ActionObject->Modify();
					const int32 NewIndex = Helper.AddValue();
					void* EntryPtr = Helper.GetRawPtr(NewIndex);
					DirectoryStructProperty->InitializeValue(EntryPtr);
					PathProperty->SetPropertyValue_InContainer(EntryPtr, DirectoryPath);
				}
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("directory_path"), DirectoryPath);
			Row->SetBoolField(TEXT("added"), !bAlreadyPresent);
			OutEntries.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (!bDryRun)
		{
			OutCountAfter = Helper.Num();
		}
		return true;
	}

	static bool EnsureSoftClassStructArrayValue(
		UObject* ActionObject,
		void* OwnerStructPtr,
		UStruct* OwnerStruct,
		const TCHAR* ArrayPropertyName,
		const TCHAR* ClassPropertyName,
		const FString& ClassPath,
		const TCHAR* ExpectedBaseClassPath,
		bool bDryRun,
		bool& bOutAdded,
		FString& OutError)
	{
		bOutAdded = false;

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(OwnerStruct, ArrayPropertyName);
		if (!ArrayProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose array '%s'"), *OwnerStruct->GetName(), ArrayPropertyName);
			return false;
		}
		FStructProperty* StructProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		if (!StructProperty)
		{
			OutError = FString::Printf(TEXT("Array '%s' on '%s' is not a struct array"), ArrayPropertyName, *OwnerStruct->GetName());
			return false;
		}

		const FString DesiredClassPath = NormalizeSoftClassPath(ClassPath);
		if (!LoadClassForParam(DesiredClassPath, ClassPropertyName, ExpectedBaseClassPath, OutError))
		{
			return false;
		}

		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(OwnerStructPtr);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const FString DesiredComparePath = NormalizeSoftObjectPathForCompare(DesiredClassPath);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* EntryPtr = Helper.GetRawPtr(Index);
			FString ExistingPath;
			if (!TryGetSoftClassPropertyPath(EntryPtr, StructProperty->Struct, ClassPropertyName, ExistingPath, OutError))
			{
				return false;
			}
			if (NormalizeSoftObjectPathForCompare(ExistingPath).Equals(DesiredComparePath, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		bOutAdded = true;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* EntryPtr = Helper.GetRawPtr(NewIndex);
			StructProperty->InitializeValue(EntryPtr);
			bool bChanged = false;
			if (!TrySetSoftClassProperty(EntryPtr, StructProperty->Struct, ClassPropertyName, DesiredClassPath, bChanged, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool EnsureAttributeSetGrantValue(
		UObject* ActionObject,
		void* OwnerStructPtr,
		UStruct* OwnerStruct,
		const FAttributeSetGrantSpec& Spec,
		bool bDryRun,
		bool& bOutAdded,
		bool& bOutUpdated,
		FString& OutError)
	{
		bOutAdded = false;
		bOutUpdated = false;

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(OwnerStruct, TEXT("GrantedAttributes"));
		if (!ArrayProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose array 'GrantedAttributes'"), *OwnerStruct->GetName());
			return false;
		}
		FStructProperty* StructProperty = CastField<FStructProperty>(ArrayProperty->Inner);
		if (!StructProperty)
		{
			OutError = FString::Printf(TEXT("GrantedAttributes on '%s' is not a struct array"), *OwnerStruct->GetName());
			return false;
		}

		const FString DesiredClassPath = NormalizeSoftClassPath(Spec.AttributeSetClass);
		if (!LoadClassForParam(DesiredClassPath, TEXT("attribute_set_class"), TEXT("/Script/GameplayAbilities.AttributeSet"), OutError))
		{
			return false;
		}
		if (!Spec.InitializationData.IsEmpty())
		{
			const FString DesiredInitializationData = NormalizeObjectPath(Spec.InitializationData);
			UObject* InitializationData = StaticLoadObject(UObject::StaticClass(), nullptr, *DesiredInitializationData);
			if (!InitializationData)
			{
				OutError = FString::Printf(TEXT("Could not load initialization_data '%s'"), *DesiredInitializationData);
				return false;
			}
			if (UClass* DataTableClass = StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Script/Engine.DataTable")))
			{
				if (!InitializationData->IsA(DataTableClass))
				{
					OutError = FString::Printf(TEXT("initialization_data '%s' is not a DataTable"), *InitializationData->GetPathName());
					return false;
				}
			}
		}

		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(OwnerStructPtr);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const FString DesiredComparePath = NormalizeSoftObjectPathForCompare(DesiredClassPath);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* EntryPtr = Helper.GetRawPtr(Index);
			FString ExistingPath;
			if (!TryGetSoftClassPropertyPath(EntryPtr, StructProperty->Struct, TEXT("AttributeSetType"), ExistingPath, OutError))
			{
				return false;
			}
			if (!NormalizeSoftObjectPathForCompare(ExistingPath).Equals(DesiredComparePath, ESearchCase::IgnoreCase))
			{
				continue;
			}

			if (!Spec.InitializationData.IsEmpty())
			{
				FString ExistingInitializationData;
				if (!TryGetSoftObjectPropertyPath(EntryPtr, StructProperty->Struct, TEXT("InitializationData"), ExistingInitializationData, OutError))
				{
					return false;
				}
				if (!NormalizeSoftObjectPathForCompare(ExistingInitializationData).Equals(
					NormalizeSoftObjectPathForCompare(Spec.InitializationData),
					ESearchCase::IgnoreCase))
				{
					bOutUpdated = true;
					if (!bDryRun)
					{
						ActionObject->Modify();
						bool bChanged = false;
						if (!TrySetSoftObjectProperty(EntryPtr, StructProperty->Struct, TEXT("InitializationData"), Spec.InitializationData, true, bChanged, OutError))
						{
							return false;
						}
					}
				}
			}
			return true;
		}

		bOutAdded = true;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* EntryPtr = Helper.GetRawPtr(NewIndex);
			StructProperty->InitializeValue(EntryPtr);

			bool bClassChanged = false;
			bool bObjectChanged = false;
			if (!TrySetSoftClassProperty(EntryPtr, StructProperty->Struct, TEXT("AttributeSetType"), DesiredClassPath, bClassChanged, OutError)
				|| !TrySetSoftObjectProperty(EntryPtr, StructProperty->Struct, TEXT("InitializationData"), Spec.InitializationData, true, bObjectChanged, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool EnsureSoftObjectArrayValue(
		UObject* ActionObject,
		void* OwnerStructPtr,
		UStruct* OwnerStruct,
		const TCHAR* ArrayPropertyName,
		const FString& ObjectPath,
		const TCHAR* ExpectedClassPath,
		bool bDryRun,
		bool& bOutAdded,
		FString& OutError)
	{
		bOutAdded = false;

		FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(OwnerStruct, ArrayPropertyName);
		if (!ArrayProperty)
		{
			OutError = FString::Printf(TEXT("Struct '%s' must expose array '%s'"), *OwnerStruct->GetName(), ArrayPropertyName);
			return false;
		}
		FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(ArrayProperty->Inner);
		if (!SoftObjectProperty)
		{
			OutError = FString::Printf(TEXT("Array '%s' on '%s' is not a soft object array"), ArrayPropertyName, *OwnerStruct->GetName());
			return false;
		}

		const FString DesiredObjectPath = NormalizeObjectPath(ObjectPath);
		if (DesiredObjectPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Object path for '%s' must not be empty"), ArrayPropertyName);
			return false;
		}
		UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *DesiredObjectPath);
		if (!LoadedObject)
		{
			OutError = FString::Printf(TEXT("Could not load object '%s'"), *DesiredObjectPath);
			return false;
		}
		if (ExpectedClassPath && *ExpectedClassPath)
		{
			if (UClass* ExpectedClass = StaticLoadClass(UObject::StaticClass(), nullptr, ExpectedClassPath))
			{
				if (!LoadedObject->IsA(ExpectedClass))
				{
					OutError = FString::Printf(TEXT("Object '%s' is not a '%s'"), *LoadedObject->GetPathName(), *ExpectedClass->GetPathName());
					return false;
				}
			}
		}

		void* ArrayPtr = ArrayProperty->ContainerPtrToValuePtr<void>(OwnerStructPtr);
		FScriptArrayHelper Helper(ArrayProperty, ArrayPtr);
		const FString DesiredComparePath = NormalizeSoftObjectPathForCompare(DesiredObjectPath);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			const void* ValuePtr = Helper.GetRawPtr(Index);
			const FSoftObjectPtr ExistingPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			if (NormalizeSoftObjectPathForCompare(ExistingPtr.ToSoftObjectPath().ToString()).Equals(DesiredComparePath, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		bOutAdded = true;
		if (!bDryRun)
		{
			ActionObject->Modify();
			const int32 NewIndex = Helper.AddValue();
			void* ValuePtr = Helper.GetRawPtr(NewIndex);
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(DesiredObjectPath)));
		}
		return true;
	}

	static bool EnsureAbilitiesListEntry(
		UObject* ActionObject,
		const FString& ActorClassPath,
		const TArray<FString>& AbilityClasses,
		const TArray<FAttributeSetGrantSpec>& AttributeSets,
		const TArray<FString>& AbilitySets,
		bool bDryRun,
		bool& bOutCreatedEntry,
		int32& OutAbilitiesAdded,
		int32& OutAttributesAdded,
		int32& OutAttributesUpdated,
		int32& OutAbilitySetsAdded,
		int32& OutCountBefore,
		int32& OutCountAfter,
		FString& OutError)
	{
		bOutCreatedEntry = false;
		OutAbilitiesAdded = 0;
		OutAttributesAdded = 0;
		OutAttributesUpdated = 0;
		OutAbilitySetsAdded = 0;
		OutCountBefore = 0;
		OutCountAfter = 0;

		FArrayProperty* AbilitiesListProperty = FindFProperty<FArrayProperty>(ActionObject->GetClass(), TEXT("AbilitiesList"));
		if (!AbilitiesListProperty)
		{
			OutError = FString::Printf(TEXT("Action class '%s' does not expose an AbilitiesList array"), *ActionObject->GetClass()->GetPathName());
			return false;
		}
		FStructProperty* EntryStructProperty = CastField<FStructProperty>(AbilitiesListProperty->Inner);
		if (!EntryStructProperty)
		{
			OutError = FString::Printf(TEXT("AbilitiesList on '%s' is not a struct array"), *ActionObject->GetPathName());
			return false;
		}

		const FString DesiredActorClassPath = NormalizeSoftClassPath(ActorClassPath);
		if (!LoadClassForParam(DesiredActorClassPath, TEXT("actor_class"), TEXT("/Script/Engine.Actor"), OutError))
		{
			return false;
		}

		void* ArrayPtr = AbilitiesListProperty->ContainerPtrToValuePtr<void>(ActionObject);
		FScriptArrayHelper Helper(AbilitiesListProperty, ArrayPtr);
		OutCountBefore = Helper.Num();
		OutCountAfter = Helper.Num();

		void* TargetEntryPtr = nullptr;
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			void* EntryPtr = Helper.GetRawPtr(Index);
			FString ExistingActorClassPath;
			if (!TryGetSoftClassPropertyPath(EntryPtr, EntryStructProperty->Struct, TEXT("ActorClass"), ExistingActorClassPath, OutError))
			{
				return false;
			}
			if (NormalizeSoftObjectPathForCompare(ExistingActorClassPath).Equals(
				NormalizeSoftObjectPathForCompare(DesiredActorClassPath),
				ESearchCase::IgnoreCase))
			{
				TargetEntryPtr = EntryPtr;
				break;
			}
		}

		if (!TargetEntryPtr)
		{
			bOutCreatedEntry = true;
			OutCountAfter = Helper.Num() + 1;
			if (!bDryRun)
			{
				ActionObject->Modify();
				const int32 NewIndex = Helper.AddValue();
				TargetEntryPtr = Helper.GetRawPtr(NewIndex);
				EntryStructProperty->InitializeValue(TargetEntryPtr);
				bool bActorChanged = false;
				if (!TrySetSoftClassProperty(TargetEntryPtr, EntryStructProperty->Struct, TEXT("ActorClass"), DesiredActorClassPath, bActorChanged, OutError))
				{
					return false;
				}
			}
		}

		if (!TargetEntryPtr && bDryRun)
		{
			for (const FString& AbilityClass : AbilityClasses)
			{
				if (!LoadClassForParam(AbilityClass, TEXT("ability_class"), TEXT("/Script/GameplayAbilities.GameplayAbility"), OutError))
				{
					return false;
				}
			}
			for (const FAttributeSetGrantSpec& AttributeSpec : AttributeSets)
			{
				if (!LoadClassForParam(AttributeSpec.AttributeSetClass, TEXT("attribute_set_class"), TEXT("/Script/GameplayAbilities.AttributeSet"), OutError))
				{
					return false;
				}
				if (!AttributeSpec.InitializationData.IsEmpty())
				{
					const FString ObjectPath = NormalizeObjectPath(AttributeSpec.InitializationData);
					UObject* InitializationData = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
					if (!InitializationData)
					{
						OutError = FString::Printf(TEXT("Could not load initialization_data '%s'"), *ObjectPath);
						return false;
					}
				}
			}
			for (const FString& AbilitySet : AbilitySets)
			{
				const FString ObjectPath = NormalizeObjectPath(AbilitySet);
				if (!StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
				{
					OutError = FString::Printf(TEXT("Could not load ability set '%s'"), *ObjectPath);
					return false;
				}
			}
			OutAbilitiesAdded = AbilityClasses.Num();
			OutAttributesAdded = AttributeSets.Num();
			OutAbilitySetsAdded = AbilitySets.Num();
			return true;
		}

		for (const FString& AbilityClass : AbilityClasses)
		{
			bool bAdded = false;
			if (!EnsureSoftClassStructArrayValue(
				ActionObject,
				TargetEntryPtr,
				EntryStructProperty->Struct,
				TEXT("GrantedAbilities"),
				TEXT("AbilityType"),
				AbilityClass,
				TEXT("/Script/GameplayAbilities.GameplayAbility"),
				bDryRun,
				bAdded,
				OutError))
			{
				return false;
			}
			OutAbilitiesAdded += bAdded ? 1 : 0;
		}

		for (const FAttributeSetGrantSpec& AttributeSpec : AttributeSets)
		{
			bool bAdded = false;
			bool bUpdated = false;
			if (!EnsureAttributeSetGrantValue(ActionObject, TargetEntryPtr, EntryStructProperty->Struct, AttributeSpec, bDryRun, bAdded, bUpdated, OutError))
			{
				return false;
			}
			OutAttributesAdded += bAdded ? 1 : 0;
			OutAttributesUpdated += bUpdated ? 1 : 0;
		}

		for (const FString& AbilitySet : AbilitySets)
		{
			bool bAdded = false;
			if (!EnsureSoftObjectArrayValue(
				ActionObject,
				TargetEntryPtr,
				EntryStructProperty->Struct,
				TEXT("GrantedAbilitySets"),
				AbilitySet,
				TEXT("/Script/LyraGame.LyraAbilitySet"),
				bDryRun,
				bAdded,
				OutError))
			{
				return false;
			}
			OutAbilitySetsAdded += bAdded ? 1 : 0;
		}

		if (!bDryRun)
		{
			OutCountAfter = Helper.Num();
		}
		return true;
	}

	static TArray<FString> DirectoryPathsToStrings(const TArray<FDirectoryPath>& Directories)
	{
		TArray<FString> Values;
		for (const FDirectoryPath& Directory : Directories)
		{
			Values.Add(Directory.Path);
		}
		return Values;
	}

	static TArray<FString> SoftObjectPathsToStrings(const TArray<FSoftObjectPath>& Paths)
	{
		TArray<FString> Values;
		for (const FSoftObjectPath& Path : Paths)
		{
			Values.Add(Path.ToString());
		}
		return Values;
	}

	static TArray<FDirectoryPath> StringsToDirectoryPaths(const TArray<FString>& Values)
	{
		TArray<FDirectoryPath> Directories;
		for (const FString& Value : Values)
		{
			FDirectoryPath Directory;
			Directory.Path = Value;
			Directories.Add(Directory);
		}
		return Directories;
	}

	static TArray<FSoftObjectPath> StringsToSoftObjectPaths(const TArray<FString>& Values)
	{
		TArray<FSoftObjectPath> Paths;
		for (const FString& Value : Values)
		{
			Paths.Add(FSoftObjectPath(NormalizeObjectPath(Value)));
		}
		return Paths;
	}

	static bool StringArraysEqual(const TArray<FString>& A, const TArray<FString>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Equals(B[Index], ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	static TSharedPtr<FJsonObject> PrimaryAssetTypeInfoToJson(const FPrimaryAssetTypeInfo& Info)
	{
		auto MakeStringValues = [](const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		};

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("primary_asset_type"), Info.PrimaryAssetType.ToString());
		Row->SetStringField(TEXT("asset_base_class"), Info.GetAssetBaseClass().ToSoftObjectPath().ToString());
		Row->SetBoolField(TEXT("has_blueprint_classes"), Info.bHasBlueprintClasses);
		Row->SetBoolField(TEXT("is_editor_only"), Info.bIsEditorOnly);
		Row->SetArrayField(TEXT("directories"), MakeStringValues(DirectoryPathsToStrings(Info.GetDirectories())));
		Row->SetArrayField(TEXT("specific_assets"), MakeStringValues(SoftObjectPathsToStrings(Info.GetSpecificAssets())));
		return Row;
	}

	static FString GetPackagePathForPlugin(const FGameFeaturePluginInfo& Plugin)
	{
		if (!Plugin.MountedAssetPath.IsEmpty())
		{
			FString Mounted = Plugin.MountedAssetPath;
			Mounted.RemoveFromEnd(TEXT("/"));
			return Mounted;
		}
		return FString::Printf(TEXT("/%s"), *Plugin.Name);
	}

	static TArray<FAssetData> FindGameFeatureDataForPlugin(const FGameFeaturePluginInfo& Plugin)
	{
		return GetGameFeatureDataAssets(GetPackagePathForPlugin(Plugin));
	}

	static TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& AssetData)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.GetAssetName().ToString());
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		Row->SetBoolField(TEXT("loaded"), AssetData.IsAssetLoaded());
		return Row;
	}

	static TSharedPtr<FJsonObject> PluginToJson(const FGameFeaturePluginInfo& Plugin, const TArray<FAssetData>* DataAssets = nullptr)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Plugin.Name);
		Row->SetStringField(TEXT("friendly_name"), Plugin.FriendlyName.IsEmpty() ? Plugin.Name : Plugin.FriendlyName);
		Row->SetStringField(TEXT("descriptor_path"), RedactPath(Plugin.DescriptorPath));
		Row->SetStringField(TEXT("base_dir"), RedactPath(Plugin.BaseDir));
		Row->SetStringField(TEXT("content_dir"), RedactPath(Plugin.ContentDir));
		Row->SetStringField(TEXT("content_root"), GetPackagePathForPlugin(Plugin));
		Row->SetBoolField(TEXT("enabled"), Plugin.bEnabled);
		Row->SetBoolField(TEXT("engine_plugin"), Plugin.bFromEngine);
		Row->SetBoolField(TEXT("can_contain_content"), Plugin.bCanContainContent);
		Row->SetBoolField(TEXT("declares_gamefeatures_dependency"), Plugin.bDeclaresGameFeaturesDependency);
		Row->SetBoolField(TEXT("descriptor_has_game_feature_state"), Plugin.bDescriptorHasGameFeatureState);

		if (DataAssets)
		{
			TArray<TSharedPtr<FJsonValue>> Assets;
			for (const FAssetData& AssetData : *DataAssets)
			{
				Assets.Add(MakeShared<FJsonValueObject>(AssetDataToJson(AssetData)));
			}
			Row->SetArrayField(TEXT("game_feature_data_assets"), Assets);
			Row->SetNumberField(TEXT("game_feature_data_count"), Assets.Num());
			if (DataAssets->Num() > 0)
			{
				Row->SetStringField(TEXT("game_feature_data"), (*DataAssets)[0].PackageName.ToString());
			}
		}
		return Row;
	}

	static bool TryGetPluginName(const TSharedPtr<FJsonObject>& Params, FString& OutPluginName, FMonolithActionResult& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("plugin_name"), OutPluginName) || OutPluginName.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(TEXT("Missing required param 'plugin_name'"), -32602);
			return false;
		}
		return true;
	}

	static bool TryFindPluginByName(const FString& PluginName, FGameFeaturePluginInfo& OutPlugin)
	{
		for (const FGameFeaturePluginInfo& Plugin : DiscoverPlugins(true))
		{
			if (Plugin.Name.Equals(PluginName, ESearchCase::IgnoreCase))
			{
				OutPlugin = Plugin;
				return true;
			}
		}
		return false;
	}

	static bool TryResolveGameFeatureData(const TSharedPtr<FJsonObject>& Params, FAssetData& OutAsset, FString& OutPluginName, FString& OutError)
	{
		FString AssetPath;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("asset_path"), AssetPath);
			Params->TryGetStringField(TEXT("plugin_name"), OutPluginName);
		}

		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!NormalizedAssetPath.IsEmpty())
		{
			for (const FAssetData& Asset : GetGameFeatureDataAssets())
			{
				if (Asset.PackageName.ToString().Equals(NormalizedAssetPath, ESearchCase::IgnoreCase)
					|| Asset.GetObjectPathString().Equals(AssetPath, ESearchCase::IgnoreCase))
				{
					OutAsset = Asset;
					return true;
				}
			}
			OutError = FString::Printf(TEXT("No GameFeatureData asset found at '%s'"), *AssetPath);
			return false;
		}

		if (!OutPluginName.IsEmpty())
		{
			FGameFeaturePluginInfo Plugin;
			if (!TryFindPluginByName(OutPluginName, Plugin))
			{
				OutError = FString::Printf(TEXT("No GameFeature plugin named '%s' was found"), *OutPluginName);
				return false;
			}
			TArray<FAssetData> Assets = FindGameFeatureDataForPlugin(Plugin);
			if (Assets.Num() == 0)
			{
				OutError = FString::Printf(TEXT("Plugin '%s' has no indexed GameFeatureData asset under %s"), *OutPluginName, *GetPackagePathForPlugin(Plugin));
				return false;
			}
			// Prefer the descriptor-declared GameFeatureData asset before any
			// heuristic candidate; a multi-data plugin must not silently
			// resolve to an arbitrary Assets[0] that the .uplugin did not name.
			if (Plugin.DescriptorJson.IsValid())
			{
				FString DeclaredPath;
				if (Plugin.DescriptorJson->TryGetStringField(TEXT("GameFeatureData"), DeclaredPath)
					&& !DeclaredPath.IsEmpty())
				{
					const FString DeclaredPackage = NormalizeAssetPath(DeclaredPath);
					for (const FAssetData& Asset : Assets)
					{
						if (Asset.PackageName.ToString().Equals(DeclaredPackage, ESearchCase::IgnoreCase)
							|| Asset.GetObjectPathString().Equals(DeclaredPath, ESearchCase::IgnoreCase))
						{
							OutAsset = Asset;
							return true;
						}
					}
				}
			}
			OutAsset = Assets[0];
			return true;
		}

		OutError = TEXT("Provide either 'plugin_name' or 'asset_path'");
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static FString GetClassModuleName(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}

		FString PackageName = Class->GetOutermost() ? Class->GetOutermost()->GetName() : FString();
		PackageName.RemoveFromStart(TEXT("/Script/"));
		return PackageName;
	}

	static FString TruncateValue(FString Value, int32 MaxChars)
	{
		const int32 EffectiveMax = FMath::Clamp(MaxChars, 64, 4096);
		if (Value.Len() <= EffectiveMax)
		{
			return Value;
		}
		Value.LeftInline(EffectiveMax);
		Value += TEXT("...");
		return Value;
	}

	static TSharedPtr<FJsonObject> PropertyToJson(
		const FProperty* Property,
		const void* Container,
		UObject* Owner,
		bool bIncludeValue,
		int32 MaxValueChars)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Property)
		{
			Row->SetStringField(TEXT("name"), TEXT("null"));
			return Row;
		}

		Row->SetStringField(TEXT("name"), Property->GetName());
		Row->SetStringField(TEXT("type"), Property->GetClass()->GetName());
		Row->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Row->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
		Row->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
		Row->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));
		Row->SetBoolField(TEXT("instanced_reference"), Property->HasAnyPropertyFlags(CPF_InstancedReference));
		Row->SetBoolField(TEXT("contains_instanced_reference"), Property->ContainsInstancedObjectProperty());

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			Row->SetStringField(TEXT("inner_type"), ArrayProperty->Inner ? ArrayProperty->Inner->GetClass()->GetName() : FString());
			Row->SetStringField(TEXT("inner_cpp_type"), ArrayProperty->Inner ? ArrayProperty->Inner->GetCPPType() : FString());
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			Row->SetStringField(TEXT("struct"), StructProperty->Struct ? StructProperty->Struct->GetPathName() : FString());
		}
		else if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			Row->SetStringField(TEXT("meta_class"), ClassProperty->MetaClass ? ClassProperty->MetaClass->GetPathName() : FString());
		}
		else if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			Row->SetStringField(TEXT("property_class"), SoftObjectProperty->PropertyClass ? SoftObjectProperty->PropertyClass->GetPathName() : FString());
		}
		else if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			Row->SetStringField(TEXT("property_class"), ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : FString());
		}

		if (bIncludeValue && Container && !Property->HasAnyPropertyFlags(CPF_Transient))
		{
			FString Value;
			const void* ValuePtr = Property->ContainerPtrToValuePtr<const void>(Container);
			Property->ExportTextItem_Direct(Value, ValuePtr, nullptr, Owner, PPF_None);
			Row->SetStringField(TEXT("value"), TruncateValue(Value, MaxValueChars));
		}
		return Row;
	}

	static TArray<TSharedPtr<FJsonValue>> PropertyListToJson(
		UClass* Class,
		const void* Container,
		UObject* Owner,
		bool bEditableOnly,
		bool bIncludeValue,
		int32 PropertyLimit,
		int32 MaxValueChars,
		int32& OutTotalPropertyCount)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		OutTotalPropertyCount = 0;
		const int32 Limit = FMath::Clamp(PropertyLimit, 0, 200);
		if (!Class)
		{
			return Rows;
		}

		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (bEditableOnly && !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			++OutTotalPropertyCount;
			if (Rows.Num() >= Limit)
			{
				continue;
			}
			Rows.Add(MakeShared<FJsonValueObject>(PropertyToJson(Property, Container, Owner, bIncludeValue, MaxValueChars)));
		}
		return Rows;
	}

	static TSharedPtr<FJsonObject> ActionObjectToJson(
		UObject* ActionObject,
		int32 Index,
		bool bIncludeProperties,
		bool bEditableOnly,
		bool bIncludeValues,
		int32 PropertyLimit,
		int32 MaxValueChars)
	{
		TSharedPtr<FJsonObject> ActionJson = MakeShared<FJsonObject>();
		ActionJson->SetNumberField(TEXT("index"), Index);

		if (!ActionObject)
		{
			ActionJson->SetStringField(TEXT("class"), TEXT("null"));
			ActionJson->SetBoolField(TEXT("is_null"), true);
			return ActionJson;
		}

		UClass* ActionClass = ActionObject->GetClass();
		ActionJson->SetBoolField(TEXT("is_null"), false);
		ActionJson->SetStringField(TEXT("name"), ActionObject->GetName());
		ActionJson->SetStringField(TEXT("object_path"), ActionObject->GetPathName());
		ActionJson->SetStringField(TEXT("class"), ActionClass ? ActionClass->GetName() : FString());
		ActionJson->SetStringField(TEXT("class_path"), ActionClass ? ActionClass->GetClassPathName().ToString() : FString());
		ActionJson->SetStringField(TEXT("module"), GetClassModuleName(ActionClass));

		int32 TotalPropertyCount = 0;
		TArray<TSharedPtr<FJsonValue>> Properties = PropertyListToJson(
			ActionClass,
			ActionObject,
			ActionObject,
			bEditableOnly,
			bIncludeValues,
			PropertyLimit,
			MaxValueChars,
			TotalPropertyCount);
		ActionJson->SetNumberField(TEXT("property_count"), TotalPropertyCount);
		ActionJson->SetBoolField(TEXT("properties_truncated"), TotalPropertyCount > Properties.Num());
		if (bIncludeProperties)
		{
			ActionJson->SetArrayField(TEXT("properties"), Properties);
		}
		return ActionJson;
	}

	static UClass* GetGameFeatureActionBaseClass()
	{
		return StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Script/GameFeatures.GameFeatureAction"));
	}

	static TSharedPtr<FJsonObject> MakeCheck(const FString& Name, bool bOk, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetBoolField(TEXT("ok"), bOk);
		Check->SetStringField(TEXT("detail"), Detail);
		return Check;
	}

	static TSharedPtr<FJsonObject> MakeStatusJson()
	{
		const UMonolithSettings* Settings = UMonolithSettings::Get();
		const bool bEnabled = Settings && Settings->bEnableGameFeatureActions;
		const bool bCreationAllowed = bEnabled && Settings && Settings->bAllowGameFeaturePluginCreation;
		const TArray<FGameFeaturePluginInfo> Plugins = DiscoverPlugins(false);
		const TArray<FString> AlwaysActions = {
			TEXT("get_status"),
			TEXT("add_action_set_input_mapping"),
			TEXT("set_primary_asset_scan"),
			TEXT("add_game_feature_data_input_mapping"),
			TEXT("add_game_feature_data_widgets"),
			TEXT("add_game_feature_data_components"),
			TEXT("add_game_feature_data_gameplay_cue_paths"),
			TEXT("add_game_feature_data_abilities"),
			TEXT("remove_game_feature_data_action")
		};
		const TArray<FString> InspectionActions = {
			TEXT("list_plugins"),
			TEXT("find_game_feature_data"),
			TEXT("describe_game_feature_data"),
			TEXT("list_action_classes"),
			TEXT("describe_action_set"),
			TEXT("validate_plugin")
		};
		const TArray<FString> WriteActions = {
			TEXT("add_action_set_input_mapping"),
			TEXT("set_primary_asset_scan"),
			TEXT("add_game_feature_data_input_mapping"),
			TEXT("add_game_feature_data_widgets"),
			TEXT("add_game_feature_data_components"),
			TEXT("add_game_feature_data_gameplay_cue_paths"),
			TEXT("add_game_feature_data_abilities"),
			TEXT("remove_game_feature_data_action")
		};
		TArray<FString> RegisteredActions = AlwaysActions;
		if (bEnabled)
		{
			RegisteredActions.Append(InspectionActions);
		}
		TArray<FString> ImplementedActions = AlwaysActions;
		ImplementedActions.Append(InspectionActions);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("gamefeatures"));
		Result->SetStringField(TEXT("mode"), bEnabled ? TEXT("inspection_and_instanced_action_writes") : TEXT("instanced_action_writes"));
		Result->SetBoolField(TEXT("enabled"), bEnabled);
		Result->SetBoolField(TEXT("inspection_enabled"), bEnabled);
		Result->SetBoolField(TEXT("write_actions_registered"), true);
		Result->SetBoolField(TEXT("creation_allowed"), bCreationAllowed);
		Result->SetBoolField(TEXT("hard_toolsetregistry_dependency"), false);
		Result->SetBoolField(TEXT("gamefeatures_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("GameFeatures")));
		Result->SetBoolField(TEXT("gamefeatures_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("GameFeaturesEditor")));
		Result->SetNumberField(TEXT("plugin_count"), Plugins.Num());
		Result->SetArrayField(TEXT("scan_roots"), StringArrayToJson({
			RedactPath(FPaths::ProjectPluginsDir() / TEXT("GameFeatures"))
		}));
		Result->SetArrayField(TEXT("actions"), StringArrayToJson(RegisteredActions));
		Result->SetArrayField(TEXT("registered_actions"), StringArrayToJson(RegisteredActions));
		Result->SetArrayField(TEXT("write_actions"), StringArrayToJson(WriteActions));
		Result->SetArrayField(TEXT("available_when_enabled"), StringArrayToJson(bEnabled ? TArray<FString>() : InspectionActions));
		Result->SetArrayField(TEXT("implemented_actions"), StringArrayToJson(ImplementedActions));
		Result->SetArrayField(TEXT("future_reserved_actions"), StringArrayToJson({
			TEXT("plan_plugin_creation"),
			TEXT("create_plugin")
		}));
		return Result;
	}
}

void FMonolithGameFeatureActions::Register(FMonolithToolRegistry& Registry, bool bEnableInspectionActions)
{
	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("get_status"),
		TEXT("Report read-only GameFeatures inspection availability, flags, module status, and discovered plugin count."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::GetStatus),
		EmptySchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_action_set_input_mapping"),
		TEXT("Add or update an instanced GameFeatureAction_AddInputContextMapping-style action on an ActionSet asset with one InputMappingContext entry."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddActionSetInputMapping),
		AddActionSetInputMappingSchema(),
		TEXT("Action Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("set_primary_asset_scan"),
		TEXT("Create or update one UGameFeatureData PrimaryAssetTypesToScan entry with idempotent class, directory, asset, blueprint, and editor-only comparison."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::SetPrimaryAssetScan),
		SetPrimaryAssetScanSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_game_feature_data_input_mapping"),
		TEXT("Add or update a GameFeatureAction_AddInputContextMapping-style instanced action directly on UGameFeatureData."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddGameFeatureDataInputMapping),
		AddGameFeatureDataInputMappingSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_game_feature_data_widgets"),
		TEXT("Add or update a GameFeatureAction_AddWidgets-style instanced action on UGameFeatureData with layout/widget class plus GameplayTag entries."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddGameFeatureDataWidgets),
		AddGameFeatureDataWidgetsSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_game_feature_data_components"),
		TEXT("Add or update a GameFeatureAction_AddComponents instanced action on UGameFeatureData with one actor/component request entry."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddGameFeatureDataComponents),
		AddGameFeatureDataComponentsSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_game_feature_data_gameplay_cue_paths"),
		TEXT("Add or update a GameFeatureAction_AddGameplayCuePath-style instanced action on UGameFeatureData with GameplayCue directory paths."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePaths),
		AddGameFeatureDataGameplayCuePathsSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("add_game_feature_data_abilities"),
		TEXT("Add or update a GameFeatureAction_AddAbilities-style instanced action on UGameFeatureData with actor ability, attribute, and ability-set grants."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::AddGameFeatureDataAbilities),
		AddGameFeatureDataAbilitiesSchema(),
		TEXT("GameFeatureData Authoring"));

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("remove_game_feature_data_action"),
		TEXT("Remove one or more instanced GameFeatureAction entries from a UGameFeatureData Actions array by index, object name, and/or action class, with dry-run support."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::RemoveGameFeatureDataAction),
		RemoveGameFeatureDataActionSchema(),
		TEXT("GameFeatureData Authoring"));

	if (!bEnableInspectionActions)
	{
		return;
	}

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("list_plugins"),
		TEXT("List GameFeature-style plugins using plugin descriptors and AssetRegistry metadata. Read-only; no plugin activation or file writes."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::ListPlugins),
		ListPluginsSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("find_game_feature_data"),
		TEXT("Resolve a GameFeature plugin name or asset path to bounded GameFeatureData AssetRegistry metadata without loading arbitrary paths."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::FindGameFeatureData),
		FindGameFeatureDataSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("describe_game_feature_data"),
		TEXT("Load and summarize one GameFeatureData asset by plugin name or asset path, including bounded reflected action summaries."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::DescribeGameFeatureData),
		DescribeGameFeatureDataSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("list_action_classes"),
		TEXT("List loaded UGameFeatureAction subclasses and their editable reflected properties for authoring discovery."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::ListActionClasses),
		ListActionClassesSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("describe_action_set"),
		TEXT("Load an existing Lyra-style ActionSet asset and summarize its instanced Actions array with bounded reflected property values."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::DescribeActionSet),
		DescribeActionSetSchema());

	Registry.RegisterAction(TEXT("gamefeatures"), TEXT("validate_plugin"),
		TEXT("Validate a GameFeature plugin descriptor, content root, GameFeatureData asset, and creation gate state. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::ValidatePlugin),
		ValidatePluginSchema());
}

FMonolithActionResult FMonolithGameFeatureActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	return FMonolithActionResult::Success(MonolithGameFeatures::MakeStatusJson());
}

FMonolithActionResult FMonolithGameFeatureActions::ListPlugins(const TSharedPtr<FJsonObject>& Params)
{
	double LimitValue = 50.0;
	bool bIncludeEngine = false;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		Params->TryGetBoolField(TEXT("include_engine"), bIncludeEngine);
	}
	const int32 Limit = MonolithGameFeatures::ClampLimit(LimitValue);
	const TArray<MonolithGameFeatures::FGameFeaturePluginInfo> Plugins = MonolithGameFeatures::DiscoverPlugins(bIncludeEngine);

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 MatchedCount = 0;
	for (const MonolithGameFeatures::FGameFeaturePluginInfo& Plugin : Plugins)
	{
		++MatchedCount;
		if (Rows.Num() >= Limit)
		{
			continue;
		}
		const TArray<FAssetData> DataAssets = MonolithGameFeatures::FindGameFeatureDataForPlugin(Plugin);
		Rows.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::PluginToJson(Plugin, &DataAssets)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("include_engine"), bIncludeEngine);
	Result->SetBoolField(TEXT("truncated"), MatchedCount > Rows.Num());
	Result->SetArrayField(TEXT("plugins"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::FindGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	FAssetData ResolvedAsset;
	FString PluginName;
	FString Error;
	const bool bFound = MonolithGameFeatures::TryResolveGameFeatureData(Params, ResolvedAsset, PluginName, Error);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("found"), bFound);
	if (!PluginName.IsEmpty())
	{
		Result->SetStringField(TEXT("plugin_name"), PluginName);
	}
	if (bFound)
	{
		Result->SetObjectField(TEXT("game_feature_data"), MonolithGameFeatures::AssetDataToJson(ResolvedAsset));
	}
	else
	{
		Result->SetStringField(TEXT("reason"), Error);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::DescribeGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	FAssetData ResolvedAsset;
	FString PluginName;
	FString Error;
	if (!MonolithGameFeatures::TryResolveGameFeatureData(Params, ResolvedAsset, PluginName, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* Asset = ResolvedAsset.GetAsset();
	if (!Asset)
	{
		return FMonolithActionResult::Error(TEXT("Failed to load GameFeatureData asset"), -32603);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!PluginName.IsEmpty())
	{
		Result->SetStringField(TEXT("plugin_name"), PluginName);
	}
	Result->SetObjectField(TEXT("asset"), MonolithGameFeatures::AssetDataToJson(ResolvedAsset));
	Result->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), Asset->GetClass()->GetClassPathName().ToString());

	bool bIncludeProperties = true;
	bool bEditableOnly = true;
	bool bIncludeValues = true;
	int32 PropertyLimit = 40;
	int32 MaxValueChars = 512;
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_action_properties"), bIncludeProperties, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("editable_only"), bEditableOnly, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_values"), bIncludeValues, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("property_limit"), PropertyLimit, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("max_value_chars"), MaxValueChars, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 TotalActionCount = 0;
	if (const FArrayProperty* ActionsProperty = FindFProperty<FArrayProperty>(Asset->GetClass(), TEXT("Actions")))
	{
		const void* ArrayPtr = ActionsProperty->ContainerPtrToValuePtr<void>(Asset);
		FScriptArrayHelper Helper(ActionsProperty, ArrayPtr);
		TotalActionCount = Helper.Num();
		const int32 Limit = FMath::Min(TotalActionCount, 50);
		for (int32 Index = 0; Index < Limit; ++Index)
		{
			UObject* ActionObject = nullptr;
			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(ActionsProperty->Inner))
			{
				ActionObject = ObjectProperty->GetObjectPropertyValue(Helper.GetRawPtr(Index));
			}

			Actions.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::ActionObjectToJson(
				ActionObject,
				Index,
				bIncludeProperties,
				bEditableOnly,
				bIncludeValues,
				PropertyLimit,
				MaxValueChars)));
		}
	}

	Result->SetNumberField(TEXT("action_count"), TotalActionCount);
	Result->SetBoolField(TEXT("actions_truncated"), TotalActionCount > Actions.Num());
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetBoolField(TEXT("raw_object_graph_dumped"), false);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::ListActionClasses(const TSharedPtr<FJsonObject>& Params)
{
	bool bIncludeAbstract = false;
	bool bEditableOnly = true;
	bool bIncludeDefaultValues = false;
	int32 Limit = 100;
	int32 PropertyLimit = 40;
	int32 MaxValueChars = 512;
	FString ModuleFilter;
	FString NameContains;
	FString Error;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("module"), ModuleFilter);
		Params->TryGetStringField(TEXT("name_contains"), NameContains);
		ModuleFilter.TrimStartAndEndInline();
		NameContains.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_abstract"), bIncludeAbstract, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("editable_only"), bEditableOnly, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_default_values"), bIncludeDefaultValues, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("limit"), Limit, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("property_limit"), PropertyLimit, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("max_value_chars"), MaxValueChars, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	Limit = FMath::Clamp(Limit, 1, 200);
	UClass* GameFeatureActionBase = MonolithGameFeatures::GetGameFeatureActionBaseClass();
	if (!GameFeatureActionBase)
	{
		return FMonolithActionResult::Error(TEXT("Could not load /Script/GameFeatures.GameFeatureAction"), -32603);
	}

	TArray<UClass*> Classes;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(GameFeatureActionBase) || Class == GameFeatureActionBase)
		{
			continue;
		}
		if (!bIncludeAbstract && Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		const FString ModuleName = MonolithGameFeatures::GetClassModuleName(Class);
		if (!ModuleFilter.IsEmpty() && !ModuleName.Contains(ModuleFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!NameContains.IsEmpty()
			&& !Class->GetName().Contains(NameContains, ESearchCase::IgnoreCase)
			&& !Class->GetClassPathName().ToString().Contains(NameContains, ESearchCase::IgnoreCase))
		{
			continue;
		}
		Classes.Add(Class);
	}
	Classes.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetClassPathName().ToString() < B.GetClassPathName().ToString();
	});

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 Index = 0; Index < Classes.Num() && Rows.Num() < Limit; ++Index)
	{
		UClass* Class = Classes[Index];
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("name"), Class->GetName());
		Row->SetStringField(TEXT("class_path"), Class->GetClassPathName().ToString());
		Row->SetStringField(TEXT("module"), MonolithGameFeatures::GetClassModuleName(Class));
		Row->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
		Row->SetBoolField(TEXT("native"), Class->HasAnyClassFlags(CLASS_Native));

		int32 TotalPropertyCount = 0;
		UObject* Defaults = Class->GetDefaultObject(false);
		const TArray<TSharedPtr<FJsonValue>> Properties = MonolithGameFeatures::PropertyListToJson(
			Class,
			Defaults,
			Defaults,
			bEditableOnly,
			bIncludeDefaultValues,
			PropertyLimit,
			MaxValueChars,
			TotalPropertyCount);
		Row->SetArrayField(TEXT("properties"), Properties);
		Row->SetNumberField(TEXT("property_count"), TotalPropertyCount);
		Row->SetBoolField(TEXT("properties_truncated"), TotalPropertyCount > Properties.Num());
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Classes.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), Classes.Num() > Rows.Num());
	Result->SetArrayField(TEXT("classes"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::DescribeActionSet(const TSharedPtr<FJsonObject>& Params)
{
	FString ActionSetPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("action_set_path"), ActionSetPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bIncludeProperties = true;
	bool bEditableOnly = true;
	bool bIncludeValues = true;
	int32 ActionLimit = 50;
	int32 PropertyLimit = 40;
	int32 MaxValueChars = 512;
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_action_properties"), bIncludeProperties, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("editable_only"), bEditableOnly, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("include_values"), bIncludeValues, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("action_limit"), ActionLimit, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("property_limit"), PropertyLimit, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("max_value_chars"), MaxValueChars, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	ActionLimit = FMath::Clamp(ActionLimit, 1, 200);

	UObject* ActionSet = MonolithGameFeatures::LoadAssetObject(ActionSetPath, Error);
	if (!ActionSet)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FArrayProperty* ActionsArrayProperty = nullptr;
	FObjectPropertyBase* ActionsObjectProperty = nullptr;
	if (!MonolithGameFeatures::TryGetActionsArray(ActionSet, ActionsArrayProperty, ActionsObjectProperty, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	void* ActionsArrayPtr = ActionsArrayProperty->ContainerPtrToValuePtr<void>(ActionSet);
	FScriptArrayHelper ActionsHelper(ActionsArrayProperty, ActionsArrayPtr);

	TArray<TSharedPtr<FJsonValue>> Actions;
	for (int32 Index = 0; Index < ActionsHelper.Num() && Actions.Num() < ActionLimit; ++Index)
	{
		UObject* ActionObject = ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index));
		Actions.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::ActionObjectToJson(
			ActionObject,
			Index,
			bIncludeProperties,
			bEditableOnly,
			bIncludeValues,
			PropertyLimit,
			MaxValueChars)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ActionSet->GetPathName());
	Result->SetStringField(TEXT("class"), ActionSet->GetClass()->GetName());
	Result->SetStringField(TEXT("class_path"), ActionSet->GetClass()->GetClassPathName().ToString());
	Result->SetStringField(TEXT("actions_property_class"), ActionsObjectProperty->PropertyClass ? ActionsObjectProperty->PropertyClass->GetPathName() : FString());
	Result->SetNumberField(TEXT("action_count"), ActionsHelper.Num());
	Result->SetBoolField(TEXT("actions_truncated"), ActionsHelper.Num() > Actions.Num());
	Result->SetArrayField(TEXT("actions"), Actions);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddActionSetInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString ActionSetPath;
	FString MappingContextPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("action_set_path"), ActionSetPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("mapping_context_path"), MappingContextPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FString ActionClassPath = TEXT("/Script/LyraGame.GameFeatureAction_AddInputContextMapping");
	FString ActionName;
	int32 Priority = 0;
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("priority"), Priority, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (ActionClassPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Param 'action_class_path' must not be empty"), -32602);
	}

	UObject* ActionSet = MonolithGameFeatures::LoadAssetObject(ActionSetPath, Error);
	if (!ActionSet)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FArrayProperty* ActionsArrayProperty = nullptr;
	FObjectPropertyBase* ActionsObjectProperty = nullptr;
	if (!MonolithGameFeatures::TryGetActionsArray(ActionSet, ActionsArrayProperty, ActionsObjectProperty, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	if (ActionsObjectProperty->PropertyClass && !ActionClass->IsChildOf(ActionsObjectProperty->PropertyClass))
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Action class '%s' is not compatible with Actions element class '%s'"),
				*ActionClass->GetPathName(),
				*ActionsObjectProperty->PropertyClass->GetPathName()),
			-32602);
	}

	if (!FindFProperty<FArrayProperty>(ActionClass, TEXT("InputMappings")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action class '%s' does not expose an InputMappings array"), *ActionClass->GetPathName()),
			-32602);
	}

	const FString NormalizedMappingContextPath = MonolithGameFeatures::NormalizeObjectPath(MappingContextPath);
	UObject* MappingContext = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizedMappingContextPath);
	if (!MappingContext)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Could not load InputMappingContext '%s'"), *MappingContextPath),
			-32602);
	}
	if (UClass* InputMappingContextClass = StaticLoadClass(UObject::StaticClass(), nullptr, TEXT("/Script/EnhancedInput.InputMappingContext")))
	{
		if (!MappingContext->IsA(InputMappingContextClass))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Asset '%s' is not an InputMappingContext"), *MappingContext->GetPathName()),
				-32602);
		}
	}

	void* ActionsArrayPtr = ActionsArrayProperty->ContainerPtrToValuePtr<void>(ActionSet);
	FScriptArrayHelper ActionsHelper(ActionsArrayProperty, ActionsArrayPtr);
	const int32 ActionCountBefore = ActionsHelper.Num();

	bool bCreatedAction = false;
	bool bAddedMapping = false;
	bool bUpdatedPriority = false;
	bool bSaved = false;
	int32 ExistingActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 MappingCountBefore = 0;
	int32 MappingCountAfter = 0;

	UObject* ActionObject = MonolithGameFeatures::FindExistingAction(
		ActionsHelper,
		ActionsObjectProperty,
		ActionClass,
		ActionName,
		ExistingActionIndex);

	if (!ActionObject)
	{
		bCreatedAction = true;
		if (!bDryRun)
		{
			ActionSet->Modify();
			const FName BaseName = ActionName.IsEmpty() ? FName(TEXT("AddInputMapping")) : FName(*ActionName);
			const FName UniqueName = MakeUniqueObjectName(ActionSet, ActionClass, BaseName);
			ActionObject = NewObject<UObject>(ActionSet, ActionClass, UniqueName, RF_Transactional);
			if (!ActionObject)
			{
				return FMonolithActionResult::Error(TEXT("Failed to create instanced GameFeatureAction object"), -32603);
			}
			ActionObject->Modify();
			const int32 NewIndex = ActionsHelper.AddValue();
			ActionsObjectProperty->SetObjectPropertyValue(ActionsHelper.GetRawPtr(NewIndex), ActionObject);
			ExistingActionIndex = NewIndex;
		}
	}

	if (bRemoveNullActions && !bDryRun)
	{
		ActionSet->Modify();
		RemovedNullActionCount = MonolithGameFeatures::RemoveNullActions(ActionsHelper, ActionsObjectProperty);
		if (RemovedNullActionCount > 0 && ExistingActionIndex != INDEX_NONE)
		{
			ExistingActionIndex = INDEX_NONE;
			for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
			{
				if (ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index)) == ActionObject)
				{
					ExistingActionIndex = Index;
					break;
				}
			}
		}
	}
	else if (bRemoveNullActions)
	{
		for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
		{
			if (!ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index)))
			{
				++RemovedNullActionCount;
			}
		}
	}

	if (ActionObject)
	{
		if (!MonolithGameFeatures::EnsureInputMappingEntry(
			ActionObject,
			MappingContextPath,
			Priority,
			bDryRun,
			bAddedMapping,
			bUpdatedPriority,
			MappingCountBefore,
			MappingCountAfter,
			Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
	}
	else
	{
		// Dry-run path for a missing action: validate by constructing a transient
		// class default object view without modifying the asset package.
		UObject* TransientAction = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!TransientAction)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
		if (!MonolithGameFeatures::EnsureInputMappingEntry(
			TransientAction,
			MappingContextPath,
			Priority,
			true,
			bAddedMapping,
			bUpdatedPriority,
			MappingCountBefore,
			MappingCountAfter,
			Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
	}

	if (!bDryRun && (bCreatedAction || bAddedMapping || bUpdatedPriority || RemovedNullActionCount > 0))
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(ActionSet, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("action_set_path"), ActionSet->GetPathName());
	Result->SetStringField(TEXT("action_set_class"), ActionSet->GetClass()->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetStringField(TEXT("mapping_context_path"), MonolithGameFeatures::NormalizeObjectPath(MappingContextPath));
	Result->SetNumberField(TEXT("priority"), Priority);
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetBoolField(TEXT("added_mapping"), bAddedMapping);
	Result->SetBoolField(TEXT("updated_priority"), bUpdatedPriority);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), bDryRun ? ActionCountBefore + (bCreatedAction ? 1 : 0) - RemovedNullActionCount : ActionsHelper.Num());
	Result->SetNumberField(TEXT("action_index"), ExistingActionIndex);
	Result->SetNumberField(TEXT("input_mappings_before"), MappingCountBefore);
	Result->SetNumberField(TEXT("input_mappings_after"), MappingCountAfter);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bCreatedAction || bAddedMapping || bUpdatedPriority || RemovedNullActionCount > 0);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::SetPrimaryAssetScan(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString PrimaryAssetType;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error)
		|| !MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("primary_asset_type"), PrimaryAssetType, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	const bool bHasAssetBaseClass = Params.IsValid() && Params->HasField(TEXT("asset_base_class"));
	const bool bHasBlueprintClasses = Params.IsValid() && Params->HasField(TEXT("has_blueprint_classes"));
	const bool bHasEditorOnly = Params.IsValid() && Params->HasField(TEXT("is_editor_only"));
	const bool bHasDirectories = Params.IsValid() && Params->HasField(TEXT("directories"));
	const bool bHasSpecificAssets = Params.IsValid() && Params->HasField(TEXT("specific_assets"));

	FString AssetBaseClassPath = TEXT("/Script/CoreUObject.Object");
	bool bBlueprintClasses = false;
	bool bEditorOnly = false;
	bool bSave = true;
	bool bDryRun = false;
	TArray<FString> Directories;
	TArray<FString> SpecificAssets;

	if (Params.IsValid() && bHasAssetBaseClass)
	{
		Params->TryGetStringField(TEXT("asset_base_class"), AssetBaseClassPath);
		AssetBaseClassPath.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("has_blueprint_classes"), bBlueprintClasses, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("is_editor_only"), bEditorOnly, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadStringArrayParam(Params, TEXT("directories"), Directories, Error)
		|| !MonolithGameFeatures::TryReadStringArrayParam(Params, TEXT("specific_assets"), SpecificAssets, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	const FString NormalizedAssetBaseClassPath = MonolithGameFeatures::NormalizeSoftClassPath(AssetBaseClassPath);
	UClass* AssetBaseClass = StaticLoadClass(UObject::StaticClass(), nullptr, *NormalizedAssetBaseClassPath);
	if (!AssetBaseClass)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Could not load asset_base_class '%s'"), *AssetBaseClassPath),
			-32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<FPrimaryAssetTypeInfo>& TypesToScan = GameFeatureData->GetPrimaryAssetTypesToScan();
	const FName DesiredPrimaryAssetType(*PrimaryAssetType);
	const int32 ExistingIndex = TypesToScan.IndexOfByPredicate([&DesiredPrimaryAssetType](const FPrimaryAssetTypeInfo& Info)
	{
		return Info.PrimaryAssetType == DesiredPrimaryAssetType;
	});
	const bool bFoundExisting = ExistingIndex != INDEX_NONE;

	FPrimaryAssetTypeInfo BeforeInfo;
	if (bFoundExisting)
	{
		BeforeInfo = TypesToScan[ExistingIndex];
	}

	FPrimaryAssetTypeInfo DesiredInfo = bFoundExisting ? TypesToScan[ExistingIndex] : FPrimaryAssetTypeInfo();
	DesiredInfo.PrimaryAssetType = DesiredPrimaryAssetType;
	if (bHasAssetBaseClass || !bFoundExisting)
	{
		if (!DesiredInfo.CanModifyConfigData())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("PrimaryAssetTypesToScan entry '%s' cannot modify config data after runtime scan data has been filled"), *PrimaryAssetType),
				-32603);
		}
		DesiredInfo.SetAssetBaseClass(TSoftClassPtr<UObject>(AssetBaseClass));
	}
	if (bHasBlueprintClasses || !bFoundExisting)
	{
		DesiredInfo.bHasBlueprintClasses = bBlueprintClasses;
	}
	if (bHasEditorOnly || !bFoundExisting)
	{
		DesiredInfo.bIsEditorOnly = bEditorOnly;
	}
	if (bHasDirectories)
	{
		if (!DesiredInfo.CanModifyConfigData())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("PrimaryAssetTypesToScan entry '%s' cannot update directories after runtime scan data has been filled"), *PrimaryAssetType),
				-32603);
		}
		DesiredInfo.GetDirectories() = MonolithGameFeatures::StringsToDirectoryPaths(Directories);
	}
	if (bHasSpecificAssets)
	{
		if (!DesiredInfo.CanModifyConfigData())
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("PrimaryAssetTypesToScan entry '%s' cannot update specific assets after runtime scan data has been filled"), *PrimaryAssetType),
				-32603);
		}
		DesiredInfo.GetSpecificAssets() = MonolithGameFeatures::StringsToSoftObjectPaths(SpecificAssets);
	}

	const bool bAssetBaseClassChanged = !bFoundExisting
		|| !BeforeInfo.GetAssetBaseClass().ToSoftObjectPath().ToString().Equals(DesiredInfo.GetAssetBaseClass().ToSoftObjectPath().ToString(), ESearchCase::IgnoreCase);
	const bool bBlueprintClassesChanged = !bFoundExisting || BeforeInfo.bHasBlueprintClasses != DesiredInfo.bHasBlueprintClasses;
	const bool bEditorOnlyChanged = !bFoundExisting || BeforeInfo.bIsEditorOnly != DesiredInfo.bIsEditorOnly;
	const bool bDirectoriesChanged = !bFoundExisting
		|| !MonolithGameFeatures::StringArraysEqual(
			MonolithGameFeatures::DirectoryPathsToStrings(BeforeInfo.GetDirectories()),
			MonolithGameFeatures::DirectoryPathsToStrings(DesiredInfo.GetDirectories()));
	const bool bSpecificAssetsChanged = !bFoundExisting
		|| !MonolithGameFeatures::StringArraysEqual(
			MonolithGameFeatures::SoftObjectPathsToStrings(BeforeInfo.GetSpecificAssets()),
			MonolithGameFeatures::SoftObjectPathsToStrings(DesiredInfo.GetSpecificAssets()));
	const bool bChanged = !bFoundExisting
		|| bAssetBaseClassChanged
		|| bBlueprintClassesChanged
		|| bEditorOnlyChanged
		|| bDirectoriesChanged
		|| bSpecificAssetsChanged;

	bool bSaved = false;
	if (bChanged && !bDryRun)
	{
		GameFeatureData->Modify();
		if (bFoundExisting)
		{
			TypesToScan[ExistingIndex] = DesiredInfo;
		}
		else
		{
			TypesToScan.Add(DesiredInfo);
		}
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("primary_asset_type"), PrimaryAssetType);
	Result->SetNumberField(TEXT("entry_index"), bFoundExisting ? ExistingIndex : (bDryRun ? TypesToScan.Num() : TypesToScan.Num() - 1));
	Result->SetBoolField(TEXT("created_entry"), !bFoundExisting);
	Result->SetBoolField(TEXT("updated_entry"), bFoundExisting && bChanged);
	Result->SetBoolField(TEXT("asset_base_class_changed"), bAssetBaseClassChanged);
	Result->SetBoolField(TEXT("has_blueprint_classes_changed"), bBlueprintClassesChanged);
	Result->SetBoolField(TEXT("is_editor_only_changed"), bEditorOnlyChanged);
	Result->SetBoolField(TEXT("directories_changed"), bDirectoriesChanged);
	Result->SetBoolField(TEXT("specific_assets_changed"), bSpecificAssetsChanged);
	Result->SetNumberField(TEXT("entries_before"), bFoundExisting ? TypesToScan.Num() : (bDryRun ? TypesToScan.Num() : TypesToScan.Num() - 1));
	Result->SetNumberField(TEXT("entries_after"), bDryRun ? TypesToScan.Num() + (!bFoundExisting ? 1 : 0) : TypesToScan.Num());
	Result->SetObjectField(TEXT("entry_after"), MonolithGameFeatures::PrimaryAssetTypeInfoToJson(DesiredInfo));
	if (bFoundExisting)
	{
		Result->SetObjectField(TEXT("entry_before"), MonolithGameFeatures::PrimaryAssetTypeInfoToJson(BeforeInfo));
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString MappingContextPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error)
		|| !MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("mapping_context_path"), MappingContextPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FString ActionClassPath = TEXT("/Script/LyraGame.GameFeatureAction_AddInputContextMapping");
	FString ActionName;
	int32 Priority = 0;
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("priority"), Priority, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (!FindFProperty<FArrayProperty>(ActionClass, TEXT("InputMappings")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action class '%s' does not expose an InputMappings array"), *ActionClass->GetPathName()),
			-32602);
	}

	UObject* ActionObject = nullptr;
	bool bCreatedAction = false;
	int32 ActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 ActionCountBefore = 0;
	int32 ActionCountAfter = 0;
	if (!MonolithGameFeatures::EnsureInstancedActionObject(
		GameFeatureData,
		ActionClass,
		ActionName,
		bRemoveNullActions,
		bDryRun,
		ActionObject,
		bCreatedAction,
		ActionIndex,
		RemovedNullActionCount,
		ActionCountBefore,
		ActionCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bAddedMapping = false;
	bool bUpdatedPriority = false;
	int32 MappingCountBefore = 0;
	int32 MappingCountAfter = 0;
	UObject* MappingActionObject = ActionObject;
	if (!MappingActionObject)
	{
		MappingActionObject = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!MappingActionObject)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
	}
	if (!MonolithGameFeatures::EnsureInputMappingEntry(
		MappingActionObject,
		MappingContextPath,
		Priority,
		bDryRun,
		bAddedMapping,
		bUpdatedPriority,
		MappingCountBefore,
		MappingCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bSaved = false;
	const bool bChanged = bCreatedAction || bAddedMapping || bUpdatedPriority || RemovedNullActionCount > 0;
	if (bChanged && !bDryRun)
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetStringField(TEXT("mapping_context_path"), MonolithGameFeatures::NormalizeObjectPath(MappingContextPath));
	Result->SetNumberField(TEXT("priority"), Priority);
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetBoolField(TEXT("added_mapping"), bAddedMapping);
	Result->SetBoolField(TEXT("updated_priority"), bUpdatedPriority);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionCountAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex);
	Result->SetNumberField(TEXT("input_mappings_before"), MappingCountBefore);
	Result->SetNumberField(TEXT("input_mappings_after"), MappingCountAfter);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataWidgets(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<TSharedPtr<FJsonObject>> LayoutEntries;
	TArray<TSharedPtr<FJsonObject>> WidgetEntries;
	if (!MonolithGameFeatures::ReadWidgetEntryArray(Params, TEXT("layouts"), LayoutEntries, Error)
		|| !MonolithGameFeatures::ReadWidgetEntryArray(Params, TEXT("widgets"), WidgetEntries, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (LayoutEntries.Num() == 0 && WidgetEntries.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Provide at least one layout or widget entry"), -32602);
	}

	FString ActionClassPath = TEXT("/Script/LyraGame.GameFeatureAction_AddWidgets");
	FString ActionName;
	FString LayoutArrayPropertyName = TEXT("Layout");
	FString LayoutClassPropertyName = TEXT("LayoutClass");
	FString LayoutTagPropertyName = TEXT("LayerID");
	FString WidgetArrayPropertyName = TEXT("Widgets");
	FString WidgetClassPropertyName = TEXT("WidgetClass");
	FString WidgetTagPropertyName = TEXT("SlotID");
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		Params->TryGetStringField(TEXT("layout_array_property"), LayoutArrayPropertyName);
		Params->TryGetStringField(TEXT("layout_class_property"), LayoutClassPropertyName);
		Params->TryGetStringField(TEXT("layout_tag_property"), LayoutTagPropertyName);
		Params->TryGetStringField(TEXT("widget_array_property"), WidgetArrayPropertyName);
		Params->TryGetStringField(TEXT("widget_class_property"), WidgetClassPropertyName);
		Params->TryGetStringField(TEXT("widget_tag_property"), WidgetTagPropertyName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
		LayoutArrayPropertyName.TrimStartAndEndInline();
		LayoutClassPropertyName.TrimStartAndEndInline();
		LayoutTagPropertyName.TrimStartAndEndInline();
		WidgetArrayPropertyName.TrimStartAndEndInline();
		WidgetClassPropertyName.TrimStartAndEndInline();
		WidgetTagPropertyName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* ActionObject = nullptr;
	bool bCreatedAction = false;
	int32 ActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 ActionCountBefore = 0;
	int32 ActionCountAfter = 0;
	if (!MonolithGameFeatures::EnsureInstancedActionObject(
		GameFeatureData,
		ActionClass,
		ActionName,
		bRemoveNullActions,
		bDryRun,
		ActionObject,
		bCreatedAction,
		ActionIndex,
		RemovedNullActionCount,
		ActionCountBefore,
		ActionCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* WidgetsActionObject = ActionObject;
	const bool bUseTransientAction = !WidgetsActionObject;
	if (!WidgetsActionObject)
	{
		WidgetsActionObject = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!WidgetsActionObject)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
	}

	int32 LayoutsAdded = 0;
	int32 LayoutsUpdated = 0;
	int32 WidgetsAdded = 0;
	int32 WidgetsUpdated = 0;
	TArray<TSharedPtr<FJsonValue>> EntryResults;

	auto EnsureEntry = [&EntryResults, &Error](
		UObject* TargetAction,
		const TSharedPtr<FJsonObject>& Entry,
		const TCHAR* EntryKind,
		const FString& ClassField,
		const FString& TagField,
		const FString& ArrayPropertyName,
		const FString& ClassPropertyName,
		const FString& TagPropertyName,
		bool bEntryDryRun,
		bool& bOutAdded,
		bool& bOutUpdated,
		int32& OutCountBefore,
		int32& OutCountAfter) -> bool
	{
		FString ClassPath;
		FString TagName;
		if (!MonolithGameFeatures::TryGetRequiredStringParam(Entry, *ClassField, ClassPath, Error)
			|| !MonolithGameFeatures::TryGetRequiredStringParam(Entry, *TagField, TagName, Error))
		{
			return false;
		}
		if (!MonolithGameFeatures::EnsureSoftClassTagStructEntry(
			TargetAction,
			*ArrayPropertyName,
			*ClassPropertyName,
			*TagPropertyName,
			ClassPath,
			TagName,
			bEntryDryRun,
			bOutAdded,
			bOutUpdated,
			OutCountBefore,
			OutCountAfter,
			Error))
		{
			return false;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("kind"), EntryKind);
		Row->SetStringField(TEXT("class_path"), MonolithGameFeatures::NormalizeSoftClassPath(ClassPath));
		Row->SetStringField(TEXT("tag"), TagName);
		Row->SetBoolField(TEXT("added"), bOutAdded);
		Row->SetBoolField(TEXT("updated"), bOutUpdated);
		Row->SetNumberField(TEXT("entries_before"), OutCountBefore);
		Row->SetNumberField(TEXT("entries_after"), OutCountAfter);
		EntryResults.Add(MakeShared<FJsonValueObject>(Row));
		return true;
	};

	for (const TSharedPtr<FJsonObject>& Entry : LayoutEntries)
	{
		bool bAdded = false;
		bool bUpdated = false;
		int32 CountBefore = 0;
		int32 CountAfter = 0;
		if (!EnsureEntry(
			WidgetsActionObject,
			Entry,
			TEXT("layout"),
			TEXT("layout_class"),
			TEXT("layer_id"),
			LayoutArrayPropertyName,
			LayoutClassPropertyName,
			LayoutTagPropertyName,
			bDryRun && !bUseTransientAction,
			bAdded,
			bUpdated,
			CountBefore,
			CountAfter))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
		LayoutsAdded += bAdded ? 1 : 0;
		LayoutsUpdated += bUpdated ? 1 : 0;
	}

	for (const TSharedPtr<FJsonObject>& Entry : WidgetEntries)
	{
		bool bAdded = false;
		bool bUpdated = false;
		int32 CountBefore = 0;
		int32 CountAfter = 0;
		if (!EnsureEntry(
			WidgetsActionObject,
			Entry,
			TEXT("widget"),
			TEXT("widget_class"),
			TEXT("slot_id"),
			WidgetArrayPropertyName,
			WidgetClassPropertyName,
			WidgetTagPropertyName,
			bDryRun && !bUseTransientAction,
			bAdded,
			bUpdated,
			CountBefore,
			CountAfter))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
		WidgetsAdded += bAdded ? 1 : 0;
		WidgetsUpdated += bUpdated ? 1 : 0;
	}

	bool bSaved = false;
	const bool bChanged = bCreatedAction || RemovedNullActionCount > 0 || LayoutsAdded > 0 || LayoutsUpdated > 0 || WidgetsAdded > 0 || WidgetsUpdated > 0;
	if (bChanged && !bDryRun)
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionCountAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex);
	Result->SetNumberField(TEXT("layouts_added"), LayoutsAdded);
	Result->SetNumberField(TEXT("layouts_updated"), LayoutsUpdated);
	Result->SetNumberField(TEXT("widgets_added"), WidgetsAdded);
	Result->SetNumberField(TEXT("widgets_updated"), WidgetsUpdated);
	Result->SetArrayField(TEXT("entries"), EntryResults);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataComponents(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString ActorClassPath;
	FString ComponentClassPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error)
		|| !MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("actor_class"), ActorClassPath, Error)
		|| !MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("component_class"), ComponentClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FString ActionClassPath = TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents");
	FString ActionName;
	bool bClientComponent = true;
	bool bServerComponent = true;
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;
	int32 AdditionFlags = 0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("client_component"), bClientComponent, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("server_component"), bServerComponent, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error)
		|| !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("addition_flags"), AdditionFlags, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (AdditionFlags < 0 || AdditionFlags > 255)
	{
		return FMonolithActionResult::Error(TEXT("Param 'addition_flags' must be between 0 and 255"), -32602);
	}
	if (!bClientComponent && !bServerComponent)
	{
		return FMonolithActionResult::Error(TEXT("At least one of client_component or server_component must be true"), -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (!FindFProperty<FArrayProperty>(ActionClass, TEXT("ComponentList")))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Action class '%s' does not expose a ComponentList array"), *ActionClass->GetPathName()),
			-32602);
	}

	UObject* ActionObject = nullptr;
	bool bCreatedAction = false;
	int32 ActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 ActionCountBefore = 0;
	int32 ActionCountAfter = 0;
	if (!MonolithGameFeatures::EnsureInstancedActionObject(
		GameFeatureData,
		ActionClass,
		ActionName,
		bRemoveNullActions,
		bDryRun,
		ActionObject,
		bCreatedAction,
		ActionIndex,
		RemovedNullActionCount,
		ActionCountBefore,
		ActionCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* ComponentsActionObject = ActionObject;
	const bool bUseTransientAction = !ComponentsActionObject;
	if (!ComponentsActionObject)
	{
		ComponentsActionObject = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!ComponentsActionObject)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
	}

	bool bAddedComponent = false;
	bool bUpdatedComponent = false;
	int32 ComponentCountBefore = 0;
	int32 ComponentCountAfter = 0;
	if (!MonolithGameFeatures::EnsureComponentListEntry(
		ComponentsActionObject,
		ActorClassPath,
		ComponentClassPath,
		bClientComponent,
		bServerComponent,
		AdditionFlags,
		bDryRun && !bUseTransientAction,
		bAddedComponent,
		bUpdatedComponent,
		ComponentCountBefore,
		ComponentCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bSaved = false;
	const bool bChanged = bCreatedAction || RemovedNullActionCount > 0 || bAddedComponent || bUpdatedComponent;
	if (bChanged && !bDryRun)
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetStringField(TEXT("actor_class"), MonolithGameFeatures::NormalizeSoftClassPath(ActorClassPath));
	Result->SetStringField(TEXT("component_class"), MonolithGameFeatures::NormalizeSoftClassPath(ComponentClassPath));
	Result->SetBoolField(TEXT("client_component"), bClientComponent);
	Result->SetBoolField(TEXT("server_component"), bServerComponent);
	Result->SetNumberField(TEXT("addition_flags"), AdditionFlags);
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetBoolField(TEXT("added_component"), bAddedComponent);
	Result->SetBoolField(TEXT("updated_component"), bUpdatedComponent);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionCountAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex);
	Result->SetNumberField(TEXT("components_before"), ComponentCountBefore);
	Result->SetNumberField(TEXT("components_after"), ComponentCountAfter);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePaths(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<FString> DirectoryPaths;
	if (!MonolithGameFeatures::TryReadStringArrayParam(Params, TEXT("directory_paths"), DirectoryPaths, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (Params.IsValid() && Params->HasField(TEXT("directory_path")))
	{
		FString DirectoryPath;
		if (!Params->TryGetStringField(TEXT("directory_path"), DirectoryPath))
		{
			return FMonolithActionResult::Error(TEXT("Param 'directory_path' must be a string"), -32602);
		}
		DirectoryPath.TrimStartAndEndInline();
		if (!DirectoryPath.IsEmpty())
		{
			DirectoryPaths.AddUnique(DirectoryPath);
		}
	}
	if (DirectoryPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Provide at least one directory_path or directory_paths entry"), -32602);
	}

	FString ActionClassPath = TEXT("/Script/LyraGame.GameFeatureAction_AddGameplayCuePath");
	FString ActionName;
	FString DirectoryArrayPropertyName = TEXT("DirectoryPathsToAdd");
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		Params->TryGetStringField(TEXT("directory_array_property"), DirectoryArrayPropertyName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
		DirectoryArrayPropertyName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* ActionObject = nullptr;
	bool bCreatedAction = false;
	int32 ActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 ActionCountBefore = 0;
	int32 ActionCountAfter = 0;
	if (!MonolithGameFeatures::EnsureInstancedActionObject(
		GameFeatureData,
		ActionClass,
		ActionName,
		bRemoveNullActions,
		bDryRun,
		ActionObject,
		bCreatedAction,
		ActionIndex,
		RemovedNullActionCount,
		ActionCountBefore,
		ActionCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* GameplayCueActionObject = ActionObject;
	const bool bUseTransientAction = !GameplayCueActionObject;
	if (!GameplayCueActionObject)
	{
		GameplayCueActionObject = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!GameplayCueActionObject)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
	}

	int32 PathsAdded = 0;
	int32 PathsBefore = 0;
	int32 PathsAfter = 0;
	TArray<TSharedPtr<FJsonValue>> PathEntries;
	if (!MonolithGameFeatures::EnsureDirectoryPathArrayValues(
		GameplayCueActionObject,
		DirectoryArrayPropertyName,
		DirectoryPaths,
		bDryRun && !bUseTransientAction,
		PathsAdded,
		PathsBefore,
		PathsAfter,
		PathEntries,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bSaved = false;
	const bool bChanged = bCreatedAction || RemovedNullActionCount > 0 || PathsAdded > 0;
	if (bChanged && !bDryRun)
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionCountAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex);
	Result->SetNumberField(TEXT("paths_added"), PathsAdded);
	Result->SetNumberField(TEXT("paths_before"), PathsBefore);
	Result->SetNumberField(TEXT("paths_after"), PathsAfter);
	Result->SetArrayField(TEXT("paths"), PathEntries);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataAbilities(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString ActorClassPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error)
		|| !MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("actor_class"), ActorClassPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	TArray<FString> AbilityClasses;
	TArray<FString> AbilitySets;
	TArray<MonolithGameFeatures::FAttributeSetGrantSpec> AttributeSets;
	if (!MonolithGameFeatures::TryReadStringArrayParam(Params, TEXT("ability_classes"), AbilityClasses, Error)
		|| !MonolithGameFeatures::TryReadStringArrayParam(Params, TEXT("ability_sets"), AbilitySets, Error)
		|| !MonolithGameFeatures::TryReadAttributeSetGrantArrayParam(Params, TEXT("attribute_sets"), AttributeSets, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (AbilityClasses.Num() == 0 && AbilitySets.Num() == 0 && AttributeSets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Provide at least one ability_classes, attribute_sets, or ability_sets entry"), -32602);
	}

	FString ActionClassPath = TEXT("/Script/LyraGame.GameFeatureAction_AddAbilities");
	FString ActionName;
	bool bSave = true;
	bool bDryRun = false;
	bool bRemoveNullActions = true;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		ActionClassPath.TrimStartAndEndInline();
		ActionName.TrimStartAndEndInline();
	}
	if (!MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_null_actions"), bRemoveNullActions, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = MonolithGameFeatures::LoadActionClass(ActionClassPath, Error);
	if (!ActionClass)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* ActionObject = nullptr;
	bool bCreatedAction = false;
	int32 ActionIndex = INDEX_NONE;
	int32 RemovedNullActionCount = 0;
	int32 ActionCountBefore = 0;
	int32 ActionCountAfter = 0;
	if (!MonolithGameFeatures::EnsureInstancedActionObject(
		GameFeatureData,
		ActionClass,
		ActionName,
		bRemoveNullActions,
		bDryRun,
		ActionObject,
		bCreatedAction,
		ActionIndex,
		RemovedNullActionCount,
		ActionCountBefore,
		ActionCountAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UObject* AbilitiesActionObject = ActionObject;
	const bool bUseTransientAction = !AbilitiesActionObject;
	if (!AbilitiesActionObject)
	{
		AbilitiesActionObject = NewObject<UObject>(GetTransientPackage(), ActionClass, NAME_None, RF_Transient);
		if (!AbilitiesActionObject)
		{
			return FMonolithActionResult::Error(TEXT("Failed to create transient GameFeatureAction object for dry-run validation"), -32603);
		}
	}

	bool bCreatedEntry = false;
	int32 AbilitiesAdded = 0;
	int32 AttributesAdded = 0;
	int32 AttributesUpdated = 0;
	int32 AbilitySetsAdded = 0;
	int32 AbilityEntriesBefore = 0;
	int32 AbilityEntriesAfter = 0;
	if (!MonolithGameFeatures::EnsureAbilitiesListEntry(
		AbilitiesActionObject,
		ActorClassPath,
		AbilityClasses,
		AttributeSets,
		AbilitySets,
		bDryRun && !bUseTransientAction,
		bCreatedEntry,
		AbilitiesAdded,
		AttributesAdded,
		AttributesUpdated,
		AbilitySetsAdded,
		AbilityEntriesBefore,
		AbilityEntriesAfter,
		Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bSaved = false;
	const bool bChanged = bCreatedAction
		|| RemovedNullActionCount > 0
		|| bCreatedEntry
		|| AbilitiesAdded > 0
		|| AttributesAdded > 0
		|| AttributesUpdated > 0
		|| AbilitySetsAdded > 0;
	if (bChanged && !bDryRun)
	{
		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	Result->SetStringField(TEXT("actor_class"), MonolithGameFeatures::NormalizeSoftClassPath(ActorClassPath));
	Result->SetBoolField(TEXT("created_action"), bCreatedAction);
	Result->SetBoolField(TEXT("created_entry"), bCreatedEntry);
	Result->SetNumberField(TEXT("abilities_added"), AbilitiesAdded);
	Result->SetNumberField(TEXT("attributes_added"), AttributesAdded);
	Result->SetNumberField(TEXT("attributes_updated"), AttributesUpdated);
	Result->SetNumberField(TEXT("ability_sets_added"), AbilitySetsAdded);
	Result->SetNumberField(TEXT("removed_null_actions"), RemovedNullActionCount);
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), ActionCountAfter);
	Result->SetNumberField(TEXT("action_index"), ActionIndex);
	Result->SetNumberField(TEXT("ability_entries_before"), AbilityEntriesBefore);
	Result->SetNumberField(TEXT("ability_entries_after"), AbilityEntriesAfter);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), bChanged);
	if (ActionObject)
	{
		Result->SetStringField(TEXT("action_object_path"), ActionObject->GetPathName());
		Result->SetStringField(TEXT("action_object_name"), ActionObject->GetName());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::RemoveGameFeatureDataAction(const TSharedPtr<FJsonObject>& Params)
{
	FString GameFeatureDataPath;
	FString Error;
	if (!MonolithGameFeatures::TryGetRequiredStringParam(Params, TEXT("game_feature_data_path"), GameFeatureDataPath, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	const bool bHasActionIndex = Params.IsValid() && Params->HasField(TEXT("action_index"));
	const bool bHasActionName = Params.IsValid() && Params->HasField(TEXT("action_name"));
	const bool bHasActionClassPath = Params.IsValid() && Params->HasField(TEXT("action_class_path"));
	if (!bHasActionIndex && !bHasActionName && !bHasActionClassPath)
	{
		return FMonolithActionResult::Error(TEXT("Provide at least one selector: action_index, action_name, or action_class_path"), -32602);
	}

	int32 ActionIndex = INDEX_NONE;
	FString ActionName;
	FString ActionClassPath;
	bool bRemoveAll = false;
	bool bSave = true;
	bool bDryRun = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("action_name"), ActionName);
		Params->TryGetStringField(TEXT("action_class_path"), ActionClassPath);
		ActionName.TrimStartAndEndInline();
		ActionClassPath.TrimStartAndEndInline();
	}
	if ((bHasActionIndex && !MonolithGameFeatures::TryReadOptionalIntParam(Params, TEXT("action_index"), ActionIndex, Error))
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("remove_all"), bRemoveAll, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("save"), bSave, Error)
		|| !MonolithGameFeatures::TryReadOptionalBoolParam(Params, TEXT("dry_run"), bDryRun, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (bHasActionName && ActionName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Param 'action_name' must not be empty when provided"), -32602);
	}
	if (bHasActionClassPath && ActionClassPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Param 'action_class_path' must not be empty when provided"), -32602);
	}

	UGameFeatureData* GameFeatureData = MonolithGameFeatures::LoadGameFeatureDataAsset(GameFeatureDataPath, Error);
	if (!GameFeatureData)
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	FArrayProperty* ActionsArrayProperty = nullptr;
	FObjectPropertyBase* ActionsObjectProperty = nullptr;
	if (!MonolithGameFeatures::TryGetActionsArray(GameFeatureData, ActionsArrayProperty, ActionsObjectProperty, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}

	UClass* ActionClass = nullptr;
	FString NormalizedActionClassPath;
	if (bHasActionClassPath)
	{
		NormalizedActionClassPath = MonolithGameFeatures::NormalizeSoftClassPath(ActionClassPath);
		ActionClass = StaticLoadClass(UObject::StaticClass(), nullptr, *NormalizedActionClassPath);
		if (!ActionClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Could not load action_class_path '%s'"), *ActionClassPath),
				-32602);
		}
		if (ActionsObjectProperty->PropertyClass && !ActionClass->IsChildOf(ActionsObjectProperty->PropertyClass))
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Action class '%s' is not compatible with Actions element class '%s'"),
					*ActionClass->GetPathName(),
					*ActionsObjectProperty->PropertyClass->GetPathName()),
				-32602);
		}
	}

	void* ActionsArrayPtr = ActionsArrayProperty->ContainerPtrToValuePtr<void>(GameFeatureData);
	FScriptArrayHelper ActionsHelper(ActionsArrayProperty, ActionsArrayPtr);
	const int32 ActionCountBefore = ActionsHelper.Num();
	if (bHasActionIndex && (ActionIndex < 0 || ActionIndex >= ActionCountBefore))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("action_index %d is outside Actions range 0..%d"), ActionIndex, ActionCountBefore - 1),
			-32602);
	}

	TArray<int32> MatchedIndices;
	TArray<TSharedPtr<FJsonValue>> RemovedActions;
	for (int32 Index = 0; Index < ActionsHelper.Num(); ++Index)
	{
		UObject* ActionObject = ActionsObjectProperty->GetObjectPropertyValue(ActionsHelper.GetRawPtr(Index));
		if (bHasActionIndex && Index != ActionIndex)
		{
			continue;
		}
		if (bHasActionName && (!ActionObject || !ActionObject->GetName().Equals(ActionName, ESearchCase::CaseSensitive)))
		{
			continue;
		}
		if (ActionClass && (!ActionObject || !ActionObject->IsA(ActionClass)))
		{
			continue;
		}

		MatchedIndices.Add(Index);
		RemovedActions.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::ActionObjectToJson(
			ActionObject,
			Index,
			true,
			true,
			true,
			40,
			512)));
		if (!bRemoveAll)
		{
			break;
		}
	}

	bool bSaved = false;
	if (MatchedIndices.Num() > 0 && !bDryRun)
	{
		GameFeatureData->Modify();
		MatchedIndices.Sort([](const int32 A, const int32 B)
		{
			return A > B;
		});
		for (const int32 IndexToRemove : MatchedIndices)
		{
			ActionsHelper.RemoveValues(IndexToRemove, 1);
		}

		if (!MonolithGameFeatures::SaveAssetIfRequested(GameFeatureData, bSave, bSaved, Error))
		{
			return FMonolithActionResult::Error(Error, -32603);
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetStringField(TEXT("game_feature_data_path"), GameFeatureData->GetPathName());
	Result->SetNumberField(TEXT("actions_before"), ActionCountBefore);
	Result->SetNumberField(TEXT("actions_after"), bDryRun ? ActionCountBefore - MatchedIndices.Num() : ActionsHelper.Num());
	Result->SetNumberField(TEXT("removed_count"), MatchedIndices.Num());
	Result->SetBoolField(TEXT("removed"), MatchedIndices.Num() > 0);
	Result->SetBoolField(TEXT("remove_all"), bRemoveAll);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("changed"), MatchedIndices.Num() > 0);
	Result->SetArrayField(TEXT("removed_actions"), RemovedActions);
	if (bHasActionIndex)
	{
		Result->SetNumberField(TEXT("action_index"), ActionIndex);
	}
	if (bHasActionName)
	{
		Result->SetStringField(TEXT("action_name"), ActionName);
	}
	if (ActionClass)
	{
		Result->SetStringField(TEXT("action_class_path"), ActionClass->GetPathName());
	}
	else if (bHasActionClassPath)
	{
		Result->SetStringField(TEXT("action_class_path"), NormalizedActionClassPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::ValidatePlugin(const TSharedPtr<FJsonObject>& Params)
{
	FString PluginName;
	FMonolithActionResult Error = FMonolithActionResult::Error(TEXT("Missing required param 'plugin_name'"), -32602);
	if (!MonolithGameFeatures::TryGetPluginName(Params, PluginName, Error))
	{
		return Error;
	}

	MonolithGameFeatures::FGameFeaturePluginInfo Plugin;
	if (!MonolithGameFeatures::TryFindPluginByName(PluginName, Plugin))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No GameFeature plugin named '%s' was found"), *PluginName), -32602);
	}

	const TArray<FAssetData> DataAssets = MonolithGameFeatures::FindGameFeatureDataForPlugin(Plugin);
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	const bool bCreationAllowed = Settings && Settings->bEnableGameFeatureActions && Settings->bAllowGameFeaturePluginCreation;

	TArray<TSharedPtr<FJsonValue>> Checks;
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("descriptor"),
		FPaths::FileExists(Plugin.DescriptorPath),
		MonolithGameFeatures::RedactPath(Plugin.DescriptorPath))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("gamefeatures_dependency"),
		Plugin.bDeclaresGameFeaturesDependency,
		Plugin.bDeclaresGameFeaturesDependency ? TEXT("Descriptor declares enabled GameFeatures plugin dependency") : TEXT("Descriptor does not declare enabled GameFeatures plugin dependency"))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("content_root"),
		Plugin.bCanContainContent && !Plugin.MountedAssetPath.IsEmpty(),
		Plugin.MountedAssetPath.IsEmpty() ? TEXT("Plugin has no mounted asset path") : Plugin.MountedAssetPath)));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("game_feature_data"),
		DataAssets.Num() > 0,
		DataAssets.Num() > 0 ? DataAssets[0].PackageName.ToString() : TEXT("No GameFeatureData asset found under plugin content root"))));
	Checks.Add(MakeShared<FJsonValueObject>(MonolithGameFeatures::MakeCheck(
		TEXT("creation_gate"),
		!bCreationAllowed,
		bCreationAllowed ? TEXT("Creation flags are enabled, but no creation action is registered in this first slice") : TEXT("Creation disabled in this read-only slice"))));

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (!Plugin.bDeclaresGameFeaturesDependency)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Descriptor was identified by path or metadata hint; it does not declare an enabled GameFeatures plugin dependency.")));
	}
	if (!Plugin.bDescriptorHasGameFeatureState)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Descriptor does not expose BuiltInInitialFeatureState/GameFeatureData metadata.")));
	}
	if (DataAssets.Num() > 1)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT("Multiple GameFeatureData assets found; callers should pass asset_path to inspect a specific asset.")));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("plugin_name"), Plugin.Name);
	Result->SetBoolField(TEXT("ok"), FPaths::FileExists(Plugin.DescriptorPath) && Plugin.bDeclaresGameFeaturesDependency && Plugin.bCanContainContent && DataAssets.Num() > 0);
	Result->SetObjectField(TEXT("plugin"), MonolithGameFeatures::PluginToJson(Plugin, &DataAssets));
	Result->SetArrayField(TEXT("checks"), Checks);
	Result->SetArrayField(TEXT("warnings"), Warnings);
	Result->SetArrayField(TEXT("next_actions"), MonolithGameFeatures::StringArrayToJson({
		TEXT("gamefeatures.describe_game_feature_data"),
		TEXT("gamefeatures.list_plugins")
	}));
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::EmptySchema()
{
	return FParamSchemaBuilder().Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ListPluginsSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum plugins to return, clamped to 1..200"), TEXT("50"))
		.Optional(TEXT("include_engine"), TEXT("bool"), TEXT("Include engine GameFeature plugins; default only reports project/external plugins"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::FindGameFeatureDataSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to inspect"))
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("GameFeatureData package or object path"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::DescribeGameFeatureDataSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to inspect"))
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("GameFeatureData package or object path"))
		.Optional(TEXT("include_action_properties"), TEXT("bool"), TEXT("Include bounded reflected properties for each action"), TEXT("true"))
		.Optional(TEXT("editable_only"), TEXT("bool"), TEXT("Only report editable properties"), TEXT("true"))
		.Optional(TEXT("include_values"), TEXT("bool"), TEXT("Include exported action property values"), TEXT("true"))
		.Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per action"), TEXT("40"))
		.Optional(TEXT("max_value_chars"), TEXT("integer"), TEXT("Maximum characters per exported property value"), TEXT("512"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ListActionClassesSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum classes to return, clamped to 1..200"), TEXT("100"))
		.Optional(TEXT("module"), TEXT("string"), TEXT("Optional module substring filter, e.g. LyraGame or GameFeatures"))
		.Optional(TEXT("name_contains"), TEXT("string"), TEXT("Optional class name or class path substring filter"))
		.Optional(TEXT("include_abstract"), TEXT("bool"), TEXT("Include abstract GameFeatureAction classes"), TEXT("false"))
		.Optional(TEXT("editable_only"), TEXT("bool"), TEXT("Only report editable properties"), TEXT("true"))
		.Optional(TEXT("include_default_values"), TEXT("bool"), TEXT("Include class default object property values"), TEXT("false"))
		.Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per class"), TEXT("40"))
		.Optional(TEXT("max_value_chars"), TEXT("integer"), TEXT("Maximum characters per exported default value"), TEXT("512"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::DescribeActionSetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("action_set_path"), TEXT("string"), TEXT("ActionSet asset path exposing an instanced Actions object array"))
		.Optional(TEXT("action_limit"), TEXT("integer"), TEXT("Maximum action entries to return"), TEXT("50"))
		.Optional(TEXT("include_action_properties"), TEXT("bool"), TEXT("Include bounded reflected properties for each action"), TEXT("true"))
		.Optional(TEXT("editable_only"), TEXT("bool"), TEXT("Only report editable properties"), TEXT("true"))
		.Optional(TEXT("include_values"), TEXT("bool"), TEXT("Include exported property values"), TEXT("true"))
		.Optional(TEXT("property_limit"), TEXT("integer"), TEXT("Maximum properties per action"), TEXT("40"))
		.Optional(TEXT("max_value_chars"), TEXT("integer"), TEXT("Maximum characters per exported property value"), TEXT("512"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddActionSetInputMappingSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("action_set_path"), TEXT("string"), TEXT("ActionSet asset path whose instanced Actions array should contain the input mapping action"))
		.Required(TEXT("mapping_context_path"), TEXT("string"), TEXT("InputMappingContext package or object path to add to the action"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/LyraGame.GameFeatureAction_AddInputContextMapping"), TEXT("/Script/LyraGame.GameFeatureAction_AddInputContextMapping"))
		.Optional(TEXT("priority"), TEXT("integer"), TEXT("Enhanced Input mapping priority"), TEXT("0"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the ActionSet package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::SetPrimaryAssetScanSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path to edit"))
		.Required(TEXT("primary_asset_type"), TEXT("string"), TEXT("PrimaryAssetType name to ensure in PrimaryAssetTypesToScan"))
		.Optional(TEXT("asset_base_class"), TEXT("string"), TEXT("UClass path for assets of this type. Defaults to /Script/CoreUObject.Object for new entries"), TEXT("/Script/CoreUObject.Object"))
		.Optional(TEXT("has_blueprint_classes"), TEXT("bool"), TEXT("Whether scanned assets are Blueprint-generated classes"))
		.Optional(TEXT("is_editor_only"), TEXT("bool"), TEXT("Whether this type is editor-only for cook management"))
		.Optional(TEXT("directories"), TEXT("array"), TEXT("Long package paths to scan, such as /Game/Experiences or /PluginName/Experiences"))
		.Optional(TEXT("specific_assets"), TEXT("array"), TEXT("Specific package or object paths to scan"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataInputMappingSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should contain the input mapping action"))
		.Required(TEXT("mapping_context_path"), TEXT("string"), TEXT("InputMappingContext package or object path to add to the action"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/LyraGame.GameFeatureAction_AddInputContextMapping"), TEXT("/Script/LyraGame.GameFeatureAction_AddInputContextMapping"))
		.Optional(TEXT("priority"), TEXT("integer"), TEXT("Enhanced Input mapping priority"), TEXT("0"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataWidgetsSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should contain the AddWidgets action"))
		.Optional(TEXT("layouts"), TEXT("array"), TEXT("Objects with layout_class and layer_id fields"))
		.Optional(TEXT("widgets"), TEXT("array"), TEXT("Objects with widget_class and slot_id fields"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/LyraGame.GameFeatureAction_AddWidgets"), TEXT("/Script/LyraGame.GameFeatureAction_AddWidgets"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("layout_array_property"), TEXT("string"), TEXT("Layout entry array property name on the action"), TEXT("Layout"))
		.Optional(TEXT("layout_class_property"), TEXT("string"), TEXT("Layout entry soft class property name"), TEXT("LayoutClass"))
		.Optional(TEXT("layout_tag_property"), TEXT("string"), TEXT("Layout entry GameplayTag property name"), TEXT("LayerID"))
		.Optional(TEXT("widget_array_property"), TEXT("string"), TEXT("Widget entry array property name on the action"), TEXT("Widgets"))
		.Optional(TEXT("widget_class_property"), TEXT("string"), TEXT("Widget entry soft class property name"), TEXT("WidgetClass"))
		.Optional(TEXT("widget_tag_property"), TEXT("string"), TEXT("Widget entry GameplayTag property name"), TEXT("SlotID"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataComponentsSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should contain the AddComponents action"))
		.Required(TEXT("actor_class"), TEXT("string"), TEXT("Actor class to receive the component request"))
		.Required(TEXT("component_class"), TEXT("string"), TEXT("ActorComponent class to add through ModularGameplay"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/GameFeatures.GameFeatureAction_AddComponents"), TEXT("/Script/GameFeatures.GameFeatureAction_AddComponents"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("client_component"), TEXT("bool"), TEXT("Whether to request the component on clients"), TEXT("true"))
		.Optional(TEXT("server_component"), TEXT("bool"), TEXT("Whether to request the component on servers"), TEXT("true"))
		.Optional(TEXT("addition_flags"), TEXT("integer"), TEXT("Bitmask for EGameFrameworkAddComponentFlags"), TEXT("0"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePathsSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should contain the AddGameplayCuePath action"))
		.Optional(TEXT("directory_path"), TEXT("string"), TEXT("Single slash-prefixed GameplayCue directory path to add, such as /GameplayCues"))
		.Optional(TEXT("directory_paths"), TEXT("array"), TEXT("Slash-prefixed GameplayCue directory paths to add"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/LyraGame.GameFeatureAction_AddGameplayCuePath"), TEXT("/Script/LyraGame.GameFeatureAction_AddGameplayCuePath"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("directory_array_property"), TEXT("string"), TEXT("Directory path array property name on the action"), TEXT("DirectoryPathsToAdd"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataAbilitiesSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should contain the AddAbilities action"))
		.Required(TEXT("actor_class"), TEXT("string"), TEXT("Actor class whose AbilitySystemComponent should receive grants"))
		.Optional(TEXT("ability_classes"), TEXT("array"), TEXT("GameplayAbility class paths to add to GrantedAbilities"))
		.Optional(TEXT("attribute_sets"), TEXT("array"), TEXT("Objects with attribute_set_class and optional initialization_data fields"))
		.Optional(TEXT("ability_sets"), TEXT("array"), TEXT("LyraAbilitySet asset paths to add to GrantedAbilitySets"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path. Defaults to /Script/LyraGame.GameFeatureAction_AddAbilities"), TEXT("/Script/LyraGame.GameFeatureAction_AddAbilities"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Optional exact instanced action object name to reuse or create"))
		.Optional(TEXT("remove_null_actions"), TEXT("bool"), TEXT("Remove null entries from the Actions array while editing"), TEXT("true"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::RemoveGameFeatureDataActionSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("game_feature_data_path"), TEXT("string"), TEXT("UGameFeatureData package or object path whose Actions array should be edited"))
		.Optional(TEXT("action_index"), TEXT("integer"), TEXT("Exact Actions array index to remove"))
		.Optional(TEXT("action_name"), TEXT("string"), TEXT("Exact instanced action object name to remove"))
		.Optional(TEXT("action_class_path"), TEXT("string"), TEXT("GameFeatureAction class path to match"))
		.Optional(TEXT("remove_all"), TEXT("bool"), TEXT("Remove every matching entry instead of the first match"), TEXT("false"))
		.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the GameFeatureData package after changes"), TEXT("true"))
		.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the edit without modifying the asset"), TEXT("false"))
		.Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ValidatePluginSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("plugin_name"), TEXT("string"), TEXT("GameFeature plugin name to validate"))
		.Build();
}

#else

namespace
{
	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(std::initializer_list<const TCHAR*> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TCHAR* Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	FMonolithActionResult GameFeaturesUnavailable()
	{
		return FMonolithActionResult::Error(
			TEXT("GameFeatures optional dependency is not compiled into this Monolith build."),
			-32011);
	}
}

void FMonolithGameFeatureActions::Register(FMonolithToolRegistry& Registry, bool bEnableInspectionActions)
{
	const FString Namespace = TEXT("gamefeatures");
	const FString Action = TEXT("get_status");
	Registry.RegisterAction(Namespace, Action,
		TEXT("Report GameFeatures namespace availability and optional dependency state."),
		FMonolithActionHandler::CreateStatic(&FMonolithGameFeatureActions::GetStatus),
		EmptySchema());
}

FMonolithActionResult FMonolithGameFeatureActions::GetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("gamefeatures"));
	Result->SetStringField(TEXT("mode"), TEXT("unavailable"));
	Result->SetBoolField(TEXT("enabled"), false);
	Result->SetBoolField(TEXT("inspection_enabled"), false);
	Result->SetBoolField(TEXT("write_actions_registered"), false);
	Result->SetBoolField(TEXT("creation_allowed"), false);
	Result->SetBoolField(TEXT("hard_toolsetregistry_dependency"), false);
	Result->SetBoolField(TEXT("with_gamefeatures"), false);
	Result->SetBoolField(TEXT("gamefeatures_module_exists"), FModuleManager::Get().ModuleExists(TEXT("GameFeatures")));
	Result->SetBoolField(TEXT("gamefeatures_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("GameFeatures")));
	Result->SetStringField(TEXT("dependency_state"), TEXT("unavailable"));
	Result->SetStringField(TEXT("reason"), TEXT("GameFeatures optional dependency is not compiled into this Monolith build."));
	Result->SetArrayField(TEXT("actions"), StringArrayToJson({ TEXT("get_status") }));
	Result->SetArrayField(TEXT("registered_actions"), StringArrayToJson({ TEXT("get_status") }));
	Result->SetArrayField(TEXT("write_actions"), StringArrayToJson({}));
	Result->SetArrayField(TEXT("available_when_compiled"), StringArrayToJson({
		TEXT("add_action_set_input_mapping"),
		TEXT("set_primary_asset_scan"),
		TEXT("add_game_feature_data_input_mapping"),
		TEXT("add_game_feature_data_widgets"),
		TEXT("add_game_feature_data_components"),
		TEXT("add_game_feature_data_gameplay_cue_paths"),
		TEXT("add_game_feature_data_abilities"),
		TEXT("remove_game_feature_data_action"),
		TEXT("list_plugins"),
		TEXT("find_game_feature_data"),
		TEXT("describe_game_feature_data"),
		TEXT("list_action_classes"),
		TEXT("describe_action_set"),
		TEXT("validate_plugin")
	}));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithGameFeatureActions::ListPlugins(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::FindGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::DescribeGameFeatureData(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::ListActionClasses(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::DescribeActionSet(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddActionSetInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::SetPrimaryAssetScan(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataInputMapping(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataWidgets(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataComponents(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePaths(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::AddGameFeatureDataAbilities(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::RemoveGameFeatureDataAction(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

FMonolithActionResult FMonolithGameFeatureActions::ValidatePlugin(const TSharedPtr<FJsonObject>& Params)
{
	return GameFeaturesUnavailable();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::EmptySchema()
{
	return FParamSchemaBuilder().Build();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ListPluginsSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::FindGameFeatureDataSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::DescribeGameFeatureDataSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ListActionClassesSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::DescribeActionSetSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddActionSetInputMappingSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::SetPrimaryAssetScanSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataInputMappingSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataWidgetsSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataComponentsSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataGameplayCuePathsSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::AddGameFeatureDataAbilitiesSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::RemoveGameFeatureDataActionSchema()
{
	return EmptySchema();
}

TSharedPtr<FJsonObject> FMonolithGameFeatureActions::ValidatePluginSchema()
{
	return EmptySchema();
}

#endif
