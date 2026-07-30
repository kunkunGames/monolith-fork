#include "MonolithAssetLifecycleActions.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Runtime/Launch/Resources/Version.h"

#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithParamSchema.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "HAL/PlatformFileManager.h"
#include "IAssetTools.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/PackagePath.h"
#include "Misc/PackageSegment.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "PixelFormat.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "SourceControlOperations.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	bool ReadStringAlias(const TSharedPtr<FJsonObject>& Params, const TCHAR* Primary, const TCHAR* Alternate, FString& OutValue)
	{
		return Params->TryGetStringField(Primary, OutValue)
			|| (Alternate && Params->TryGetStringField(Alternate, OutValue));
	}

	bool ParseSettingsObject(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutSettings,
		FString& OutError)
	{
		OutSettings.Reset();
		OutError.Reset();
		if (!Params->HasField(TEXT("settings")))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
		{
			OutSettings = *SettingsObj;
			return true;
		}

		OutError = TEXT("settings must be a JSON object");
		return false;
	}

	TSharedPtr<FJsonValue> FindSettingValue(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Settings,
		const TCHAR* Name,
		FString& OutError)
	{
		OutError.Reset();
		const bool bNestedPresent = Settings.IsValid() && Settings->HasField(Name);
		const bool bTopLevelPresent = Params->HasField(Name);
		if (bNestedPresent && bTopLevelPresent)
		{
			OutError = FString::Printf(
				TEXT("Setting '%s' was provided both at the top level and inside settings; provide exactly one"),
				Name);
			return nullptr;
		}

		return bNestedPresent ? Settings->TryGetField(Name) : Params->TryGetField(Name);
	}

	bool ReadBoolSetting(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Settings,
		const TCHAR* Name,
		bool& InOutValue,
		FString& OutError)
	{
		TSharedPtr<FJsonValue> Value = FindSettingValue(Params, Settings, Name, OutError);
		if (!OutError.IsEmpty() || !Value.IsValid())
		{
			return OutError.IsEmpty();
		}
		if (Value->Type != EJson::Boolean || !Value->TryGetBool(InOutValue))
		{
			OutError = FString::Printf(TEXT("Setting '%s' must be a boolean"), Name);
			return false;
		}
		return true;
	}

	bool ReadStringSetting(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Settings,
		const TCHAR* Name,
		FString& OutValue,
		FString& OutError)
	{
		TSharedPtr<FJsonValue> Value = FindSettingValue(Params, Settings, Name, OutError);
		if (!OutError.IsEmpty() || !Value.IsValid())
		{
			return OutError.IsEmpty();
		}
		if (!Value->TryGetString(OutValue) || OutValue.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Setting '%s' must be a non-empty string"), Name);
			return false;
		}
		return true;
	}

	bool ReadIntSetting(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Settings,
		const TCHAR* Name,
		int32& OutValue,
		FString& OutError)
	{
		TSharedPtr<FJsonValue> Value = FindSettingValue(Params, Settings, Name, OutError);
		if (!OutError.IsEmpty() || !Value.IsValid())
		{
			return OutError.IsEmpty();
		}

		double Number = 0.0;
		if (!Value->TryGetNumber(Number)
			|| Number < 0.0
			|| Number > static_cast<double>(MAX_int32)
			|| !FMath::IsNearlyEqual(Number, static_cast<double>(FMath::RoundToInt(Number))))
		{
			OutError = FString::Printf(TEXT("Setting '%s' must be a non-negative integer"), Name);
			return false;
		}

		OutValue = FMath::RoundToInt(Number);
		return true;
	}

	bool ParseTextureCompression(const FString& Str, TextureCompressionSettings& OutSetting)
	{
		static const TMap<FString, TextureCompressionSettings> Mappings = {
			{ TEXT("default"), TC_Default },
			{ TEXT("dxt5"), TC_Default },
			{ TEXT("dxt1"), TC_Default },
			{ TEXT("normalmap"), TC_Normalmap },
			{ TEXT("normalmapbc5"), TC_Normalmap },
			{ TEXT("normalmapla"), TC_Normalmap },
			{ TEXT("grayscale"), TC_Grayscale },
			{ TEXT("alpha"), TC_Alpha },
			{ TEXT("masks"), TC_Masks },
			{ TEXT("ui"), TC_EditorIcon },
			{ TEXT("userinterface2d"), TC_EditorIcon },
			{ TEXT("userinterface2drgba"), TC_EditorIcon },
			{ TEXT("tc_editoricon"), TC_EditorIcon },
			{ TEXT("hdr"), TC_HDR },
			{ TEXT("bc7"), TC_BC7 },
			{ TEXT("halfhdr"), TC_HalfFloat },
			{ TEXT("halffloat"), TC_HalfFloat },
			{ TEXT("displacementmap"), TC_Displacementmap },
			{ TEXT("vectordisplacementmap"), TC_VectorDisplacementmap },
		};

		if (const TextureCompressionSettings* Found = Mappings.Find(Str.ToLower()))
		{
			OutSetting = *Found;
			return true;
		}

		if (const UEnum* Enum = StaticEnum<TextureCompressionSettings>())
		{
			const int64 Value = Enum->GetValueByNameString(Str);
			if (Value != INDEX_NONE)
			{
				OutSetting = static_cast<TextureCompressionSettings>(Value);
				return true;
			}
		}

		return false;
	}

	bool ParseTextureLODGroup(const FString& Str, TextureGroup& OutGroup)
	{
		static const TMap<FString, TextureGroup> Mappings = {
			{ TEXT("world"), TEXTUREGROUP_World },
			{ TEXT("worldnormalmap"), TEXTUREGROUP_WorldNormalMap },
			{ TEXT("worldspecular"), TEXTUREGROUP_WorldSpecular },
			{ TEXT("character"), TEXTUREGROUP_Character },
			{ TEXT("characternormalmap"), TEXTUREGROUP_CharacterNormalMap },
			{ TEXT("characterspecular"), TEXTUREGROUP_CharacterSpecular },
			{ TEXT("weapon"), TEXTUREGROUP_Weapon },
			{ TEXT("weaponnormalmap"), TEXTUREGROUP_WeaponNormalMap },
			{ TEXT("weaponspecular"), TEXTUREGROUP_WeaponSpecular },
			{ TEXT("vehicle"), TEXTUREGROUP_Vehicle },
			{ TEXT("vehiclenormalmap"), TEXTUREGROUP_VehicleNormalMap },
			{ TEXT("vehiclespecular"), TEXTUREGROUP_VehicleSpecular },
			{ TEXT("effects"), TEXTUREGROUP_Effects },
			{ TEXT("ui"), TEXTUREGROUP_UI },
			{ TEXT("skybox"), TEXTUREGROUP_Skybox },
		};

		if (const TextureGroup* Found = Mappings.Find(Str.ToLower()))
		{
			OutGroup = *Found;
			return true;
		}

		if (const UEnum* Enum = StaticEnum<TextureGroup>())
		{
			const int64 Value = Enum->GetValueByNameString(Str);
			if (Value != INDEX_NONE)
			{
				OutGroup = static_cast<TextureGroup>(Value);
				return true;
			}
		}

		return false;
	}

	FString TextureSourceFormatName(const ETextureSourceFormat Format)
	{
		switch (Format)
		{
		case TSF_G8:
			return TEXT("TSF_G8");
		case TSF_BGRA8:
			return TEXT("TSF_BGRA8");
		case TSF_BGRE8:
			return TEXT("TSF_BGRE8");
		case TSF_RGBA16:
			return TEXT("TSF_RGBA16");
		case TSF_RGBA16F:
			return TEXT("TSF_RGBA16F");
		case TSF_G16:
			return TEXT("TSF_G16");
		case TSF_RGBA32F:
			return TEXT("TSF_RGBA32F");
		case TSF_R16F:
			return TEXT("TSF_R16F");
		case TSF_R32F:
			return TEXT("TSF_R32F");
		default:
			return TEXT("TSF_Invalid");
		}
	}

	bool SplitAssetPath(const FString& AssetPath, FString& OutPackagePath, FString& OutAssetName, FString& OutError)
	{
		FString LongPackageName = AssetPath;
		if (LongPackageName.Contains(TEXT(".")))
		{
			LongPackageName = FPackageName::ObjectPathToPackageName(LongPackageName);
		}

		if (const FString ValidationError = MonolithCore::ValidatePackagePath(LongPackageName); !ValidationError.IsEmpty())
		{
			OutError = ValidationError;
			return false;
		}

		int32 SlashIndex = INDEX_NONE;
		if (!LongPackageName.FindLastChar(TEXT('/'), SlashIndex) || SlashIndex <= 0 || SlashIndex == LongPackageName.Len() - 1)
		{
			OutError = FString::Printf(TEXT("Invalid asset path '%s': expected /Game/Folder/AssetName"), *AssetPath);
			return false;
		}

		OutPackagePath = LongPackageName.Left(SlashIndex);
		OutAssetName = LongPackageName.Mid(SlashIndex + 1);
		return true;
	}

	bool ReadStrictStringArray(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		OutValues.Reserve(Values.Num());
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = Values[Index];
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(
					TEXT("%s[%d] must be a non-empty string"),
					FieldName,
					Index);
				return false;
			}

			StringValue.TrimStartAndEndInline();
			if (StringValue.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("%s[%d] must be a non-empty string"),
					FieldName,
					Index);
				return false;
			}
			OutValues.Add(StringValue);
		}
		return true;
	}

	bool TryNormalizeAssetPackageName(const FString& AssetPath, FString& OutPackageName, FString& OutError)
	{
		OutPackageName = AssetPath.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(AssetPath)
			: AssetPath;

		if (const FString ValidationError = MonolithCore::ValidatePackagePath(OutPackageName); !ValidationError.IsEmpty())
		{
			OutError = ValidationError;
			return false;
		}
		return true;
	}

	FString MakeCanonicalAssetObjectPath(const FString& RequestedPath, const FString& PackageName)
	{
		FString ObjectPath = FMonolithAssetUtils::ResolveAssetPath(RequestedPath);
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = PackageName
				+ TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}
		return ObjectPath;
	}

	bool TryNormalizeAllowedPackagePrefix(const FString& Prefix, FString& OutPrefix, FString& OutError)
	{
		OutPrefix = Prefix;
		OutPrefix.TrimStartAndEndInline();
		while (OutPrefix.Len() > 1 && OutPrefix.EndsWith(TEXT("/")))
		{
			OutPrefix.LeftChopInline(1);
		}

		const FString PrefixToNormalize = OutPrefix;
		return TryNormalizeAssetPackageName(PrefixToNormalize, OutPrefix, OutError);
	}

	bool IsPackageWithinAllowedPrefix(const FString& PackageName, const FString& AllowedPrefix)
	{
		if (PackageName.Equals(AllowedPrefix, ESearchCase::IgnoreCase))
		{
			return true;
		}

		return PackageName.Len() > AllowedPrefix.Len()
			&& PackageName.StartsWith(AllowedPrefix, ESearchCase::IgnoreCase)
			&& PackageName[AllowedPrefix.Len()] == TEXT('/');
	}

	void AddPackageFilenameCandidate(
		const FString& PackageName,
		const FString& Extension,
		TArray<FString>& OutFilenames)
	{
		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Extension))
		{
			return;
		}

		Filename = FPaths::ConvertRelativePathToFull(Filename);
		FPaths::NormalizeFilename(Filename);
		OutFilenames.AddUnique(Filename);
	}

	TArray<FString> GetPackageHeaderFilenameCandidates(const FString& PackageName)
	{
		TArray<FString> Result;

		FString ExistingFilename;
		if (FPackageName::DoesPackageExist(PackageName, &ExistingFilename))
		{
			ExistingFilename = FPaths::ConvertRelativePathToFull(ExistingFilename);
			FPaths::NormalizeFilename(ExistingFilename);
			Result.AddUnique(ExistingFilename);
		}

		AddPackageFilenameCandidate(PackageName, FPackageName::GetAssetPackageExtension(), Result);
		AddPackageFilenameCandidate(PackageName, FPackageName::GetMapPackageExtension(), Result);
		AddPackageFilenameCandidate(PackageName, FPackageName::GetTextAssetPackageExtension(), Result);
		AddPackageFilenameCandidate(PackageName, FPackageName::GetTextMapPackageExtension(), Result);
		return Result;
	}

	TArray<FString> GetPackageSidecarFilenameCandidates(const FString& PackageName)
	{
		TArray<FString> Result;
		FPackagePath PackagePath;
		if (!FPackagePath::TryFromPackageName(PackageName, PackagePath))
		{
			return Result;
		}

		const EPackageSegment SidecarSegments[] = {
			EPackageSegment::Exports,
			EPackageSegment::BulkDataDefault,
			EPackageSegment::BulkDataOptional,
			EPackageSegment::BulkDataMemoryMapped,
			EPackageSegment::PayloadSidecar,
		};
		for (const EPackageSegment Segment : SidecarSegments)
		{
			FString Filename = PackagePath.GetLocalFullPath(Segment);
			if (Filename.IsEmpty())
			{
				continue;
			}
			Filename = FPaths::ConvertRelativePathToFull(Filename);
			FPaths::NormalizeFilename(Filename);
			Result.AddUnique(Filename);
		}
		return Result;
	}

	TArray<FString> GetPackageFilenameCandidates(const FString& PackageName)
	{
		TArray<FString> Result = GetPackageHeaderFilenameCandidates(PackageName);
		Result.Append(GetPackageSidecarFilenameCandidates(PackageName));
		return Result;
	}

	TArray<FString> FindExistingPackageFiles(const FString& PackageName)
	{
		TArray<FString> Result;
		for (const FString& Filename : GetPackageFilenameCandidates(PackageName))
		{
			if (IFileManager::Get().FileExists(*Filename))
			{
				Result.Add(Filename);
			}
		}
		return Result;
	}

	bool HasRegisteredAssetsForPackage(IAssetRegistry& AssetRegistry, const FString& PackageName)
	{
		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(
			FName(*PackageName),
			PackageAssets,
			/*bIncludeOnlyOnDiskAssets=*/false);
		return PackageAssets.Num() > 0;
	}

	struct FDeleteAssetTarget
	{
		enum class ESourceControlPostcondition : uint8
		{
			MarkedForDelete,
			PendingAddRemoved,
			UntrackedAbsent,
		};

		struct FSourceControlExpectation
		{
			FString Filename;
			ESourceControlPostcondition ExpectedPostcondition = ESourceControlPostcondition::UntrackedAbsent;
			bool bSourceControlledBefore = false;
			bool bAddedBefore = false;
			bool bDeletedBefore = false;
			bool bCheckedOutBefore = false;
			bool bStateValidAfter = false;
			bool bSourceControlledAfter = false;
			bool bAddedAfter = false;
			bool bDeletedAfter = false;
			bool bUnknownAfter = false;
			bool bPostconditionMet = false;
			FString FailureReason;
		};

		FString RequestedPath;
		FString PackageName;
		TArray<FString> NotFoundRequestedPaths;
		TArray<FString> InitialPackageFiles;
		TArray<FString> FinalPackageFiles;
		TArray<FSourceControlExpectation> SourceControlExpectations;
		bool bHadRegisteredAsset = false;
		bool bHadLoadedPackage = false;
		bool bFoundObject = false;
		// True when the package holds only assets this request asked to delete.
		// Residual package files are only removed under that proof.
		bool bPackageExclusivelyRequested = false;
		bool bResidualRemoved = false;
		bool bSourceControlFailure = false;
		bool bFinalRegisteredAsset = false;
		bool bFinalLoadedPackage = false;
		bool bSucceeded = false;
		FString Status;
		FString FailureReason;
	};

	FString SourceControlPostconditionToString(
		const FDeleteAssetTarget::ESourceControlPostcondition Postcondition)
	{
		switch (Postcondition)
		{
		case FDeleteAssetTarget::ESourceControlPostcondition::MarkedForDelete:
			return TEXT("marked_for_delete");
		case FDeleteAssetTarget::ESourceControlPostcondition::PendingAddRemoved:
			return TEXT("pending_add_removed");
		case FDeleteAssetTarget::ESourceControlPostcondition::UntrackedAbsent:
			return TEXT("untracked_absent");
		default:
			return TEXT("unknown");
		}
	}

	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	bool EvictLoadedPackageForDelete(const FString& PackageName, TArray<FString>& OutEvictedPackages, TArray<FString>& OutStalePackages)
	{
		UPackage* ExistingPackage = FindPackage(nullptr, *PackageName);
		if (!ExistingPackage)
		{
			return true;
		}

		ExistingPackage->SetDirtyFlag(false);
		ResetLoaders(ExistingPackage);

		TArray<UPackage*> PackagesToUnload;
		PackagesToUnload.Add(ExistingPackage);
		UPackageTools::UnloadPackages(PackagesToUnload);
		// Preserve unrelated editor assets carrying RF_Standalone. RF_NoFlags can collect a dirty,
		// unsaved asset from another Monolith workflow while this delete evicts only PackageName.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		ExistingPackage = FindPackage(nullptr, *PackageName);
		if (!ExistingPackage)
		{
			return true;
		}

		ResetLoaders(ExistingPackage);
		ExistingPackage->SetDirtyFlag(false);

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		const FString TrashPackageName = FString::Printf(
			TEXT("/Temp/__monolith_deleted_%s_%s"),
			*AssetName,
			*FGuid::NewGuid().ToString(EGuidFormats::Short));

		if (ExistingPackage->Rename(
			*TrashPackageName,
			nullptr,
			REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty))
		{
			ExistingPackage->MarkAsGarbage();
			OutEvictedPackages.Add(FString::Printf(TEXT("%s -> %s"), *PackageName, *TrashPackageName));
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			if (FindPackage(nullptr, *PackageName) == nullptr)
			{
				return true;
			}
		}

		OutStalePackages.Add(PackageName);
		return false;
	}

	FString SourceControlCommandResultToString(const ECommandResult::Type Result)
	{
		switch (Result)
		{
		case ECommandResult::Succeeded:
			return TEXT("Succeeded");
		case ECommandResult::Failed:
			return TEXT("Failed");
		case ECommandResult::Cancelled:
			return TEXT("Cancelled");
		default:
			return TEXT("Unknown");
		}
	}

	bool ExecuteResidualSourceControlOperation(
		ISourceControlProvider& Provider,
		const FString& Filename,
		const FString& OperationName,
		const FSourceControlOperationRef& Operation,
		TArray<FString>& OutSourceControlOperations,
		TArray<FString>& OutSourceControlFailures)
	{
		TArray<FString> Files;
		Files.Add(Filename);
		const ECommandResult::Type CommandResult = Provider.Execute(Operation, Files, EConcurrency::Synchronous);
		const FString CommandResultString = SourceControlCommandResultToString(CommandResult);
		if (CommandResult == ECommandResult::Succeeded)
		{
			OutSourceControlOperations.Add(FString::Printf(
				TEXT("%s:%s:%s"),
				*OperationName,
				*CommandResultString,
				*Filename));
			return true;
		}

		OutSourceControlFailures.Add(FString::Printf(
			TEXT("%s:%s:%s"),
			*OperationName,
			*CommandResultString,
			*Filename));
		return false;
	}

	void RemoveResidualPackageFilesForDelete(
		const FString& PackageName,
		TArray<FString>& OutDeletedFiles,
		TArray<FString>& OutResidualFiles,
		TArray<FString>& OutSourceControlOperations,
		TArray<FString>& OutSourceControlFailures,
		TArray<FString>& OutFilesToRescan,
		TArray<FString>& OutPathsToRescan)
	{
		OutPathsToRescan.AddUnique(FPackageName::GetLongPackagePath(PackageName));

		for (const FString& HeaderFilename : GetPackageHeaderFilenameCandidates(PackageName))
		{
			OutFilesToRescan.AddUnique(HeaderFilename);
		}

		for (const FString& PackageFilename : GetPackageFilenameCandidates(PackageName))
		{
			if (!IFileManager::Get().FileExists(*PackageFilename))
			{
				continue;
			}

			bool bCanDirectDelete = false;
			bool bCanDeleteReadOnly = false;
			bool bSourceControlBlocked = false;

			ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
			if (SourceControlModule.IsEnabled())
			{
				ISourceControlProvider& Provider = SourceControlModule.GetProvider();
				if (!Provider.IsEnabled() || !Provider.IsAvailable())
				{
					OutSourceControlFailures.AddUnique(FString::Printf(
						TEXT("provider_unavailable:%s"),
						*PackageFilename));
					bSourceControlBlocked = true;
				}
				else
				{
					FSourceControlStatePtr State = Provider.GetState(PackageFilename, EStateCacheUsage::ForceUpdate);
					if (!State.IsValid())
					{
						OutSourceControlFailures.AddUnique(FString::Printf(
							TEXT("state_unavailable:%s"),
							*PackageFilename));
						bSourceControlBlocked = true;
					}
					else if (State->IsAdded())
					{
						if (State->CanRevert()
							&& ExecuteResidualSourceControlOperation(
								Provider,
								PackageFilename,
								TEXT("revert_added"),
								ISourceControlOperation::Create<FRevert>(),
								OutSourceControlOperations,
								OutSourceControlFailures))
						{
							bCanDirectDelete = true;
							bCanDeleteReadOnly = true;
						}
						else
						{
							if (!State->CanRevert())
							{
								OutSourceControlFailures.AddUnique(FString::Printf(
									TEXT("cannot_revert_added:%s"),
									*PackageFilename));
							}
							bSourceControlBlocked = true;
						}
					}
					else
					{
						if (State->IsCheckedOut())
						{
							if (!State->CanRevert()
								|| !ExecuteResidualSourceControlOperation(
									Provider,
									PackageFilename,
									TEXT("revert_checked_out"),
									ISourceControlOperation::Create<FRevert>(),
									OutSourceControlOperations,
									OutSourceControlFailures))
							{
								if (!State->CanRevert())
								{
									OutSourceControlFailures.AddUnique(FString::Printf(
										TEXT("cannot_revert_checked_out:%s"),
										*PackageFilename));
								}
								bSourceControlBlocked = true;
							}
							else
							{
								State = Provider.GetState(PackageFilename, EStateCacheUsage::ForceUpdate);
								if (!State.IsValid())
								{
									OutSourceControlFailures.AddUnique(FString::Printf(
										TEXT("state_unavailable_after_revert:%s"),
										*PackageFilename));
									bSourceControlBlocked = true;
								}
							}
						}

						if (!bSourceControlBlocked && State.IsValid())
						{
							if (State->IsDeleted())
							{
								OutSourceControlOperations.AddUnique(FString::Printf(
									TEXT("already_deleted:%s"),
									*PackageFilename));
								bCanDirectDelete = true;
								bCanDeleteReadOnly = true;
							}
							else if (State->IsSourceControlled())
							{
								if (State->CanDelete()
									&& ExecuteResidualSourceControlOperation(
										Provider,
										PackageFilename,
										TEXT("delete"),
										ISourceControlOperation::Create<FDelete>(),
										OutSourceControlOperations,
										OutSourceControlFailures))
								{
									bCanDirectDelete = true;
									bCanDeleteReadOnly = true;
								}
								else
								{
									if (!State->CanDelete())
									{
										OutSourceControlFailures.AddUnique(FString::Printf(
											TEXT("cannot_mark_for_delete:%s"),
											*PackageFilename));
									}
									bSourceControlBlocked = true;
								}
							}
							else
							{
								// A valid provider state proved that the file is untracked.
								bCanDirectDelete = !IFileManager::Get().IsReadOnly(*PackageFilename);
							}
						}
					}
				}
			}
			else
			{
				bCanDirectDelete = !IFileManager::Get().IsReadOnly(*PackageFilename);
			}

			if (!IFileManager::Get().FileExists(*PackageFilename))
			{
				OutDeletedFiles.AddUnique(PackageFilename);
				continue;
			}

			if (!bSourceControlBlocked
				&& bCanDirectDelete
				&& IFileManager::Get().Delete(
					*PackageFilename,
					/*RequireExists=*/false,
					/*EvenReadOnly=*/bCanDeleteReadOnly,
					/*Quiet=*/true))
			{
				OutDeletedFiles.AddUnique(PackageFilename);
				continue;
			}

			OutResidualFiles.AddUnique(PackageFilename);
		}
	}
}

void FMonolithAssetLifecycleActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("import_texture_from_file"),
		TEXT("Import an external image file as a UTexture2D asset with optional texture settings."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetLifecycleActions::ImportTextureFromFile),
		FParamSchemaBuilder()
			.Required(TEXT("source_path"), TEXT("string"), TEXT("Absolute source image path. Aliases: source_file, file_path, path."),
				{ TEXT("source_file"), TEXT("file_path"), TEXT("path") })
			.Required(TEXT("destination"), TEXT("string"), TEXT("Destination asset path, e.g. /Game/Textures/T_Example. Aliases: dest_path, destination_path. If asset_name is also supplied, destination/destination_path is treated as the output folder."),
				{ TEXT("dest_path"), TEXT("destination_path") })
			.Optional(TEXT("asset_name"), TEXT("string"), TEXT("Optional asset name used with destination_path/destination as an output folder."))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("{compression, srgb, tiling, max_size, lod_group}. compression accepts TC_* names plus UI/UserInterface2D aliases."))
			.Optional(TEXT("replace_existing"), TEXT("bool"), TEXT("Overwrite existing destination asset"), TEXT("false"))
			.Optional(TEXT("overwrite_policy"), TEXT("string"), TEXT("Compatibility alias for replace_existing: overwrite/replace -> true; fail/unique -> false."))
			.StrictComplexTypes()
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("save_asset"),
		TEXT("Save a loaded asset package to disk, with optional non-interactive package reload verification."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetLifecycleActions::SaveAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to save"))
			.Optional(TEXT("verify_reload"), TEXT("bool"), TEXT("Reload the clean package non-interactively and resolve the asset again to prove persistence"), TEXT("false"))
			.StrictComplexTypes()
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("delete_assets"),
		TEXT("Delete UE assets by path. Optional safety: restrict to allowed path prefixes."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetLifecycleActions::DeleteAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of UE asset paths to delete"))
			.Optional(TEXT("allowed_prefixes"), TEXT("array"), TEXT("Only packages equal to or under these package-path prefixes may be deleted"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report targets without deleting"), TEXT("false"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("Force-delete referenced assets after closing open editors. Default false"), TEXT("false"))
			.Optional(TEXT("require_source_control"), TEXT("bool"), TEXT("Require an available provider plus per-file state preflight and verified delete/revert-add postconditions"), TEXT("false"))
			.StrictComplexTypes()
			.Build());
}

FMonolithActionResult FMonolithAssetLifecycleActions::ImportTextureFromFile(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(
			TEXT("params must be an object"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FString SourcePath;
	FString Destination;
	if (!ReadStringAlias(Params, TEXT("source_path"), TEXT("source_file"), SourcePath) || SourcePath.IsEmpty())
	{
		if (!ReadStringAlias(Params, TEXT("file_path"), TEXT("path"), SourcePath))
		{
			SourcePath.Reset();
		}
	}
	if (!ReadStringAlias(Params, TEXT("destination"), TEXT("dest_path"), Destination) || Destination.IsEmpty())
	{
		Params->TryGetStringField(TEXT("destination_path"), Destination);
	}
	FString AssetName;
	if (Params->HasField(TEXT("asset_name"))
		&& (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty()))
	{
		return FMonolithActionResult::Error(
			TEXT("asset_name must be a non-empty string"),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!AssetName.IsEmpty() && !Destination.IsEmpty())
	{
		Destination = Destination / AssetName;
	}

	if (SourcePath.IsEmpty() || Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path and destination are required. Aliases: file_path/source_file/path and destination_path/dest_path; destination_path may be paired with asset_name."));
	}

	if (FPaths::IsRelative(SourcePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("source_path must be absolute: %s"), *SourcePath),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*SourcePath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Source file not found: %s"), *SourcePath));
	}

	FString DestinationPackagePath;
	FString DestinationName;
	FString PathError;
	if (!SplitAssetPath(Destination, DestinationPackagePath, DestinationName, PathError))
	{
		return FMonolithActionResult::Error(PathError);
	}

	TSharedPtr<FJsonObject> Settings;
	FString SettingError;
	if (!ParseSettingsObject(Params, Settings, SettingError))
	{
		return FMonolithActionResult::Error(
			SettingError,
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Settings.IsValid())
	{
		static const TSet<FString> AllowedSettingNames = {
			TEXT("compression"),
			TEXT("srgb"),
			TEXT("tiling"),
			TEXT("max_size"),
			TEXT("lod_group")
		};
		for (const auto& Pair : Settings->Values)
		{
			const FString Key = MonolithKeyToString(Pair.Key);
			if (!AllowedSettingNames.Contains(Key))
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("Unknown texture setting '%s'"), *Key),
					FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	bool bReplaceExisting = false;
	if (Params->HasField(TEXT("replace_existing"))
		&& (!Params->HasTypedField<EJson::Boolean>(TEXT("replace_existing"))
			|| !Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting)))
	{
		return FMonolithActionResult::Error(
			TEXT("replace_existing must be a boolean"),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	FString OverwritePolicy;
	if (Params->HasField(TEXT("overwrite_policy")))
	{
		if (!Params->TryGetStringField(TEXT("overwrite_policy"), OverwritePolicy)
			|| OverwritePolicy.IsEmpty())
		{
			return FMonolithActionResult::Error(
				TEXT("overwrite_policy must be a non-empty string"),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		const FString Policy = OverwritePolicy.ToLower();
		if (Policy == TEXT("overwrite") || Policy == TEXT("replace") || Policy == TEXT("replace_existing"))
		{
			bReplaceExisting = true;
		}
		else if (Policy != TEXT("fail") && Policy != TEXT("unique"))
		{
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Unsupported overwrite_policy '%s'; expected fail, unique, overwrite, replace, or replace_existing"),
					*OverwritePolicy),
				FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	TextureCompressionSettings Compression = TC_Default;
	FString CompressionStr;
	if (!ReadStringSetting(Params, Settings, TEXT("compression"), CompressionStr, SettingError))
	{
		return FMonolithActionResult::Error(SettingError, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!CompressionStr.IsEmpty() && !ParseTextureCompression(CompressionStr, Compression))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid compression setting: '%s'"), *CompressionStr),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bSRGB = true;
	if (!ReadBoolSetting(Params, Settings, TEXT("srgb"), bSRGB, SettingError))
	{
		return FMonolithActionResult::Error(SettingError, FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bTiling = false;
	if (!ReadBoolSetting(Params, Settings, TEXT("tiling"), bTiling, SettingError))
	{
		return FMonolithActionResult::Error(SettingError, FMonolithJsonUtils::ErrInvalidParams);
	}

	TextureGroup LODGroup = TEXTUREGROUP_World;
	FString LODGroupStr;
	if (!ReadStringSetting(Params, Settings, TEXT("lod_group"), LODGroupStr, SettingError))
	{
		return FMonolithActionResult::Error(SettingError, FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!LODGroupStr.IsEmpty() && !ParseTextureLODGroup(LODGroupStr, LODGroup))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid lod_group: '%s'"), *LODGroupStr),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 MaxSize = 0;
	if (!ReadIntSetting(Params, Settings, TEXT("max_size"), MaxSize, SettingError))
	{
		return FMonolithActionResult::Error(SettingError, FMonolithJsonUtils::ErrInvalidParams);
	}

	const FString FinalAssetPath = DestinationPackagePath / DestinationName;
	const bool bExpectedAssetExists = UEditorAssetLibrary::DoesAssetExist(FinalAssetPath);
	if (!bReplaceExisting && bExpectedAssetExists)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Set replace_existing=true to overwrite."),
			*FinalAssetPath));
	}

	TSet<FString> PreexistingDestinationPackages;
	{
		TArray<FAssetData> ExistingAssets;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
			.Get()
			.GetAssetsByPath(
				FName(*DestinationPackagePath),
				ExistingAssets,
				/*bRecursive=*/false,
				/*bIncludeOnlyOnDiskAssets=*/false);
		for (const FAssetData& ExistingAsset : ExistingAssets)
		{
			PreexistingDestinationPackages.Add(ExistingAsset.PackageName.ToString());
		}
	}

	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->Filename = SourcePath;
	ImportTask->DestinationPath = DestinationPackagePath;
	ImportTask->DestinationName = DestinationName;
	ImportTask->bAutomated = true;
	ImportTask->bReplaceExisting = bReplaceExisting && bExpectedAssetExists;
	ImportTask->bSave = true;

	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(ImportTask);
	FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks(Tasks);

	TArray<FString> UnexpectedImportedPackages;
	for (const FString& ImportedObjectPath : ImportTask->ImportedObjectPaths)
	{
		const FString ImportedPackagePath = FPackageName::ObjectPathToPackageName(ImportedObjectPath);
		if (!ImportedPackagePath.IsEmpty() && ImportedPackagePath != FinalAssetPath)
		{
			UnexpectedImportedPackages.AddUnique(ImportedPackagePath);
		}
	}
	if (UnexpectedImportedPackages.Num() > 0)
	{
		TArray<FString> ResidualPackages;
		for (const FString& UnexpectedPackage : UnexpectedImportedPackages)
		{
			if (PreexistingDestinationPackages.Contains(UnexpectedPackage))
			{
				ResidualPackages.Add(UnexpectedPackage);
			}
			else if (UEditorAssetLibrary::DoesAssetExist(UnexpectedPackage)
				&& !UEditorAssetLibrary::DeleteAsset(UnexpectedPackage))
			{
				ResidualPackages.Add(UnexpectedPackage);
			}
		}

		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Import did not honor the exact destination '%s'. Unexpected packages: [%s]. Residual packages after cleanup: [%s]."),
				*FinalAssetPath,
				*FString::Join(UnexpectedImportedPackages, TEXT(", ")),
				*FString::Join(ResidualPackages, TEXT(", "))),
			FMonolithJsonUtils::ErrInternalError);
	}

	UObject* ImportedObject = FMonolithAssetUtils::LoadAssetByPath(FinalAssetPath);
	UTexture2D* Texture = ImportedObject ? Cast<UTexture2D>(ImportedObject) : nullptr;

	if (!Texture)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Import appeared to run, but no UTexture2D was found at '%s'. Check source image format."),
			*FinalAssetPath));
	}

	Texture->Modify();
	Texture->CompressionSettings = Compression;
	Texture->SRGB = bSRGB;
	Texture->LODGroup = LODGroup;
	if (bTiling)
	{
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
	}
	if (MaxSize > 0)
	{
		Texture->MaxTextureSize = MaxSize;
	}
	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	if (!UEditorAssetLibrary::SaveLoadedAsset(Texture, false))
	{
		// The import already replaced or created the destination object and this
		// code already mutated its texture settings, so a bare error would let the
		// caller assume nothing happened.
		if (!bExpectedAssetExists)
		{
			// Nothing was there before, so the new asset can be removed outright
			// and the failed action really is a no-op.
			const bool bRemoved = UEditorAssetLibrary::DeleteAsset(FinalAssetPath);
			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("asset_path"), FinalAssetPath);
			ErrorData->SetBoolField(TEXT("destination_pre_existed"), false);
			ErrorData->SetBoolField(TEXT("created_asset_removed"), bRemoved);
			ErrorData->SetBoolField(TEXT("partial_mutation"), !bRemoved);
			return FMonolithActionResult::Error(
				FString::Printf(
					bRemoved
						? TEXT("Failed to save imported texture asset '%s'; the newly created asset was removed.")
						: TEXT("Failed to save imported texture asset '%s', and the newly created asset could not be removed."),
					*FinalAssetPath),
				-32603)
				.WithErrorData(ErrorData);
		}

		// replace_existing overwrote a pre-existing asset in memory. Its previous
		// content is not recoverable here, so report the committed mutation
		// explicitly instead of implying the action did nothing.
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("asset_path"), FinalAssetPath);
		ErrorData->SetBoolField(TEXT("destination_pre_existed"), true);
		ErrorData->SetBoolField(TEXT("mutation_committed"), true);
		ErrorData->SetBoolField(TEXT("partial_mutation"), true);
		ErrorData->SetBoolField(TEXT("saved"), false);
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Failed to save imported texture asset '%s'. The existing asset was already replaced in memory and is left dirty and unsaved; do not retry blindly."),
				*FinalAssetPath),
			-32603)
			.WithErrorData(ErrorData);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), FinalAssetPath);
	int32 ResultSizeX = Texture->GetSizeX();
	int32 ResultSizeY = Texture->GetSizeY();
	FString FormatName = GPixelFormats[Texture->GetPixelFormat()].Name;
#if WITH_EDITOR
	const int32 SourceSizeX = Texture->Source.GetSizeX();
	const int32 SourceSizeY = Texture->Source.GetSizeY();
	const FString SourceFormatName = TextureSourceFormatName(Texture->Source.GetFormat());
	if (SourceSizeX > 0 && SourceSizeY > 0)
	{
		ResultSizeX = SourceSizeX;
		ResultSizeY = SourceSizeY;
		Result->SetNumberField(TEXT("source_size_x"), SourceSizeX);
		Result->SetNumberField(TEXT("source_size_y"), SourceSizeY);
	}
	Result->SetStringField(TEXT("source_format"), SourceFormatName);
	if (FormatName.Equals(TEXT("unknown"), ESearchCase::IgnoreCase) && SourceFormatName != TEXT("TSF_Invalid"))
	{
		FormatName = SourceFormatName;
	}
#endif
	Result->SetNumberField(TEXT("size_x"), ResultSizeX);
	Result->SetNumberField(TEXT("size_y"), ResultSizeY);
	Result->SetStringField(TEXT("format"), FormatName);

	if (const UEnum* CompressionEnum = StaticEnum<TextureCompressionSettings>())
	{
		Result->SetStringField(TEXT("compression_settings"), CompressionEnum->GetNameStringByValue(Texture->CompressionSettings));
	}
	if (const UEnum* LODGroupEnum = StaticEnum<TextureGroup>())
	{
		Result->SetStringField(TEXT("lod_group"), LODGroupEnum->GetNameStringByValue(Texture->LODGroup));
	}
	Result->SetBoolField(TEXT("srgb"), Texture->SRGB);
	if (Texture->MaxTextureSize > 0)
	{
		Result->SetNumberField(TEXT("max_size"), Texture->MaxTextureSize);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetLifecycleActions::SaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: asset_path"));
	}
	bool bVerifyReload = false;
	if (Params->HasField(TEXT("verify_reload"))
		&& (!Params->HasTypedField<EJson::Boolean>(TEXT("verify_reload"))
			|| !Params->TryGetBoolField(TEXT("verify_reload"), bVerifyReload)))
	{
		return FMonolithActionResult::Error(TEXT("verify_reload must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
	}

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset has no package: %s"), *AssetPath));
	}

	const FString CanonicalAssetPath = Asset->GetPathName();
	const FString PackageName = Package->GetName();
	const FString ClassPath = Asset->GetClass()->GetClassPathName().ToString();
	if (bVerifyReload)
	{
		if (Asset->IsA<UWorld>() || Package->ContainsMap())
		{
			return FMonolithActionResult::Error(
				TEXT("verify_reload is not allowed for map packages; reloading the current or referenced map is unsafe"));
		}
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				if (AssetEditorSubsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
				{
					return FMonolithActionResult::Error(
						FString::Printf(TEXT("Close the asset editor before verify_reload: %s"), *CanonicalAssetPath));
				}
			}
		}
	}

	const bool bWasDirty = Package->IsDirty();
	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath));
	}

	const bool bDirtyAfterSave = Package->IsDirty();
	FString PackageFilename;
	const bool bExistsOnDisk = FPackageName::DoesPackageExist(PackageName, &PackageFilename);
	const int64 FileSize = bExistsOnDisk ? IFileManager::Get().FileSize(*PackageFilename) : INDEX_NONE;
	if (bDirtyAfterSave || !bExistsOnDisk || FileSize <= 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Save postcondition failed for '%s' (dirty=%s, exists_on_disk=%s, file_size=%lld)"),
			*CanonicalAssetPath,
			bDirtyAfterSave ? TEXT("true") : TEXT("false"),
			bExistsOnDisk ? TEXT("true") : TEXT("false"),
			FileSize));
	}

	bool bReloaded = false;
	FString ReloadedClassPath;
	if (bVerifyReload)
	{
		FText ReloadError;
		TArray<UPackage*> PackagesToReload = { Package };
		bReloaded = UPackageTools::ReloadPackages(
			PackagesToReload,
			ReloadError,
			EReloadPackagesInteractionMode::AssumeNegative);
		if (!bReloaded)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Saved '%s' but package reload verification failed: %s"),
				*CanonicalAssetPath,
				*ReloadError.ToString()));
		}

		UObject* ReloadedAsset = FMonolithAssetUtils::LoadAssetByPath(CanonicalAssetPath);
		if (!ReloadedAsset)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Package reloaded but asset could not be resolved again: %s"),
				*CanonicalAssetPath));
		}
		ReloadedClassPath = ReloadedAsset->GetClass()->GetClassPathName().ToString();
		if (ReloadedClassPath != ClassPath || ReloadedAsset->GetOutermost()->IsDirty())
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Reloaded asset postcondition failed for '%s' (class='%s', dirty=%s)"),
				*CanonicalAssetPath,
				*ReloadedClassPath,
				ReloadedAsset->GetOutermost()->IsDirty() ? TEXT("true") : TEXT("false")));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
	Result->SetStringField(TEXT("package_name"), PackageName);
	Result->SetStringField(TEXT("class"), ClassPath);
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetBoolField(TEXT("was_dirty"), bWasDirty);
	Result->SetBoolField(TEXT("dirty_after_save"), false);
	Result->SetBoolField(TEXT("exists_on_disk"), true);
	Result->SetStringField(TEXT("filename"), PackageFilename);
	Result->SetNumberField(TEXT("file_size"), FileSize);
	Result->SetBoolField(TEXT("verify_reload"), bVerifyReload);
	Result->SetBoolField(TEXT("reloaded"), bReloaded);
	if (bVerifyReload)
	{
		Result->SetStringField(TEXT("reloaded_class"), ReloadedClassPath);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetLifecycleActions::DeleteAssets(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	if (AssetPathsArray->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array exceeds maximum allowed size (200)"));
	}

	TArray<FString> AssetPaths;
	FString ArrayError;
	if (!ReadStrictStringArray(*AssetPathsArray, TEXT("asset_paths"), AssetPaths, ArrayError))
	{
		return FMonolithActionResult::Error(ArrayError, FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bDryRun = false;
	bool bForce = false;
	bool bRequireSourceControl = false;
	for (const TPair<const TCHAR*, bool*> BoolParam : {
		TPair<const TCHAR*, bool*>(TEXT("dry_run"), &bDryRun),
		TPair<const TCHAR*, bool*>(TEXT("force"), &bForce),
		TPair<const TCHAR*, bool*>(TEXT("require_source_control"), &bRequireSourceControl),
	})
	{
		if (Params->HasField(BoolParam.Key)
			&& (!Params->HasTypedField<EJson::Boolean>(BoolParam.Key)
				|| !Params->TryGetBoolField(BoolParam.Key, *BoolParam.Value)))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Param '%s' must be a boolean"), BoolParam.Key),
				FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	TArray<FDeleteAssetTarget> Targets;
	Targets.Reserve(AssetPaths.Num());
	TMap<FName, int32> TargetIndexByPackage;
	TArray<int32> AssetPathTargetIndices;
	AssetPathTargetIndices.Reserve(AssetPaths.Num());
	for (const FString& Path : AssetPaths)
	{
		FString PackageName;
		FString PathError;
		if (!TryNormalizeAssetPackageName(Path, PackageName, PathError))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid asset path '%s': %s"),
				*Path,
				*PathError));
		}

		const FName PackageKey(*PackageName);
		if (const int32* ExistingIndex = TargetIndexByPackage.Find(PackageKey))
		{
			AssetPathTargetIndices.Add(*ExistingIndex);
			continue;
		}

		FDeleteAssetTarget& Target = Targets.AddDefaulted_GetRef();
		Target.RequestedPath = Path;
		Target.PackageName = PackageName;
		const int32 TargetIndex = Targets.Num() - 1;
		TargetIndexByPackage.Add(PackageKey, TargetIndex);
		AssetPathTargetIndices.Add(TargetIndex);
	}

	TArray<FString> AllowedPrefixes;
	const TArray<TSharedPtr<FJsonValue>>* PrefixArray = nullptr;
	if (Params->HasField(TEXT("allowed_prefixes")))
	{
		if (!Params->TryGetArrayField(TEXT("allowed_prefixes"), PrefixArray) || !PrefixArray || PrefixArray->Num() == 0)
		{
			return FMonolithActionResult::Error(
				TEXT("allowed_prefixes must be a non-empty array when provided"),
				FMonolithJsonUtils::ErrInvalidParams);
		}
		if (!ReadStrictStringArray(*PrefixArray, TEXT("allowed_prefixes"), AllowedPrefixes, ArrayError))
		{
			return FMonolithActionResult::Error(ArrayError, FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	TArray<FString> NormalizedAllowedPrefixes;
	NormalizedAllowedPrefixes.Reserve(AllowedPrefixes.Num());
	for (const FString& Prefix : AllowedPrefixes)
	{
		FString NormalizedPrefix;
		FString PrefixError;
		if (!TryNormalizeAllowedPackagePrefix(Prefix, NormalizedPrefix, PrefixError))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Invalid allowed_prefixes entry '%s': %s"),
				*Prefix,
				*PrefixError));
		}
		NormalizedAllowedPrefixes.AddUnique(NormalizedPrefix);
	}

	if (NormalizedAllowedPrefixes.Num() > 0)
	{
		for (const FDeleteAssetTarget& Target : Targets)
		{
			bool bAllowed = false;
			for (const FString& Prefix : NormalizedAllowedPrefixes)
			{
				if (IsPackageWithinAllowedPrefix(Target.PackageName, Prefix))
				{
					bAllowed = true;
					break;
				}
			}
			if (!bAllowed)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Refusing to delete '%s' because it is not under any allowed_prefixes entry: %s"),
					*Target.RequestedPath,
					*FString::Join(AllowedPrefixes, TEXT(", "))));
			}
		}
	}

	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	for (FDeleteAssetTarget& Target : Targets)
	{
		Target.bHadRegisteredAsset = HasRegisteredAssetsForPackage(AssetRegistry, Target.PackageName);
		Target.bHadLoadedPackage = FindPackage(nullptr, *Target.PackageName) != nullptr;
		Target.InitialPackageFiles = FindExistingPackageFiles(Target.PackageName);
	}

	if (bRequireSourceControl)
	{
		ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
		const bool bProviderReady = SourceControlModule.IsEnabled()
			&& SourceControlModule.GetProvider().IsEnabled()
			&& SourceControlModule.GetProvider().IsAvailable();
		if (!bProviderReady)
		{
			TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
			ErrorResult->SetBoolField(TEXT("success"), false);
			ErrorResult->SetStringField(TEXT("status"), TEXT("source_control_preflight_failed"));
			ErrorResult->SetBoolField(TEXT("require_source_control"), true);
			ErrorResult->SetBoolField(TEXT("source_control_enabled"), SourceControlModule.IsEnabled());
			ErrorResult->SetBoolField(
				TEXT("source_control_available"),
				SourceControlModule.IsEnabled() && SourceControlModule.GetProvider().IsAvailable());
			return FMonolithActionResult::Error(
				TEXT("delete_assets requires an enabled and available source-control provider"),
				FMonolithJsonUtils::ErrInvalidParams)
				.WithErrorData(ErrorResult);
		}

		ISourceControlProvider& Provider = SourceControlModule.GetProvider();
		TArray<TSharedPtr<FJsonValue>> PreflightFailures;
		for (FDeleteAssetTarget& Target : Targets)
		{
			for (const FString& Filename : Target.InitialPackageFiles)
			{
				FSourceControlStatePtr State = Provider.GetState(Filename, EStateCacheUsage::ForceUpdate);
				FDeleteAssetTarget::FSourceControlExpectation Expectation;
				Expectation.Filename = Filename;
				if (!State.IsValid() || State->IsUnknown())
				{
					Expectation.FailureReason = State.IsValid()
						? TEXT("source_control_state_unknown")
						: TEXT("source_control_state_unavailable");
				}
				else
				{
					Expectation.bSourceControlledBefore = State->IsSourceControlled();
					Expectation.bAddedBefore = State->IsAdded();
					Expectation.bDeletedBefore = State->IsDeleted();
					Expectation.bCheckedOutBefore = State->IsCheckedOut();

					if (State->IsAdded())
					{
						Expectation.ExpectedPostcondition =
							FDeleteAssetTarget::ESourceControlPostcondition::PendingAddRemoved;
						if (!State->CanRevert())
						{
							Expectation.FailureReason = TEXT("source_control_added_file_cannot_revert");
						}
					}
					else if (State->IsSourceControlled())
					{
						Expectation.ExpectedPostcondition =
							FDeleteAssetTarget::ESourceControlPostcondition::MarkedForDelete;
						if ((State->IsCheckedOut() || State->IsDeleted()) && !State->CanRevert())
						{
							Expectation.FailureReason = TEXT("source_control_open_file_cannot_revert");
						}
						else if (!State->IsCheckedOut() && !State->IsDeleted() && !State->CanDelete())
						{
							Expectation.FailureReason = TEXT("source_control_file_cannot_delete");
						}
					}
					else
					{
						Expectation.ExpectedPostcondition =
							FDeleteAssetTarget::ESourceControlPostcondition::UntrackedAbsent;
						if (IFileManager::Get().IsReadOnly(*Filename))
						{
							Expectation.FailureReason = TEXT("untracked_file_is_read_only");
						}
					}
				}

				if (!Expectation.FailureReason.IsEmpty())
				{
					TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetStringField(TEXT("asset_path"), Target.RequestedPath);
					Failure->SetStringField(TEXT("package_name"), Target.PackageName);
					Failure->SetStringField(TEXT("filename"), Filename);
					Failure->SetStringField(TEXT("reason"), Expectation.FailureReason);
					PreflightFailures.Add(MakeShared<FJsonValueObject>(Failure));
				}
				Target.SourceControlExpectations.Add(MoveTemp(Expectation));
			}
		}

		if (!PreflightFailures.IsEmpty())
		{
			TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
			ErrorResult->SetBoolField(TEXT("success"), false);
			ErrorResult->SetStringField(TEXT("status"), TEXT("source_control_preflight_failed"));
			ErrorResult->SetBoolField(TEXT("require_source_control"), true);
			ErrorResult->SetArrayField(TEXT("source_control_failures"), PreflightFailures);
			return FMonolithActionResult::Error(
				TEXT("delete_assets source-control preflight failed before any deletion"),
				FMonolithJsonUtils::ErrInvalidParams)
				.WithErrorData(ErrorResult);
		}
	}

	TArray<UObject*> ObjectsToDelete;
	TArray<FString> NotFound;
	for (int32 PathIndex = 0; PathIndex < AssetPaths.Num(); ++PathIndex)
	{
		const FString& Path = AssetPaths[PathIndex];
		FDeleteAssetTarget& Target = Targets[AssetPathTargetIndices[PathIndex]];
		const FString ObjectPath = MakeCanonicalAssetObjectPath(Path, Target.PackageName);
		UObject* Asset = FindObject<UObject>(nullptr, *ObjectPath);
		const FAssetData RegisteredAsset = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(ObjectPath),
			/*bIncludeOnlyOnDiskAssets=*/false,
			/*bSkipARFilteredAssets=*/true);
		const bool bRegisteredExact = RegisteredAsset.IsValid()
			&& RegisteredAsset.GetSoftObjectPath().ToString().Equals(
				ObjectPath,
				ESearchCase::IgnoreCase);
		const bool bExactAssetExists = Asset != nullptr || bRegisteredExact;
		if (!bDryRun && !Asset && bRegisteredExact)
		{
			Asset = FMonolithAssetUtils::LoadAssetByPath(ObjectPath);
		}

		if (bExactAssetExists && (bDryRun || Asset))
		{
			if (Asset)
			{
				ObjectsToDelete.AddUnique(Asset);
			}
			Target.bFoundObject = true;
		}
		else
		{
			NotFound.Add(Path);
			Target.NotFoundRequestedPaths.Add(Path);
		}
	}

	// Establish, per package, whether this request covers every asset it holds.
	// Removing package files is only safe under that proof.
	{
		TMap<FString, TSet<FString>> RequestedObjectPathsByPackage;
		for (int32 PathIndex = 0; PathIndex < AssetPaths.Num(); ++PathIndex)
		{
			FDeleteAssetTarget& Target = Targets[AssetPathTargetIndices[PathIndex]];
			RequestedObjectPathsByPackage
				.FindOrAdd(Target.PackageName)
				.Add(MakeCanonicalAssetObjectPath(AssetPaths[PathIndex], Target.PackageName)
					.ToLower());
		}

		for (FDeleteAssetTarget& Target : Targets)
		{
			const TSet<FString>* RequestedForPackage =
				RequestedObjectPathsByPackage.Find(Target.PackageName);
			if (!RequestedForPackage)
			{
				Target.bPackageExclusivelyRequested = false;
				continue;
			}

			TArray<FAssetData> PackageAssets;
			AssetRegistry.GetAssetsByPackageName(
				FName(*Target.PackageName),
				PackageAssets,
				/*bIncludeOnlyOnDiskAssets=*/false);

			bool bAllRequested = PackageAssets.Num() > 0;
			for (const FAssetData& PackageAsset : PackageAssets)
			{
				if (!RequestedForPackage->Contains(
					PackageAsset.GetSoftObjectPath().ToString().ToLower()))
				{
					bAllRequested = false;
					break;
				}
			}
			Target.bPackageExclusivelyRequested = bAllRequested;
		}
	}

	TArray<FString> UnregisteredStringTables;
	// Package -> dirty flag as it was before this action cleared it, so a refused
	// deletion can restore the user's unsaved state.
	TMap<UPackage*, bool> PreDeleteDirtyPackages;
	for (UObject* Asset : ObjectsToDelete)
	{
		if (!Asset)
		{
			continue;
		}

		if (!bDryRun)
		{
			// Record the dirty flag before clearing it. DeleteObjects can refuse a
			// non-forced delete (for example while the asset is still referenced),
			// and the package then survives in a clean state with its editor
			// closed, so a user's unsaved edits could be discarded later without a
			// save prompt.
			if (UPackage* Package = Asset->GetOutermost())
			{
				PreDeleteDirtyPackages.Add(Package, Package->IsDirty());
				Package->SetDirtyFlag(false);
			}
			if (GEditor)
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
				}
			}
			if (UStringTable* StringTable = Cast<UStringTable>(Asset))
			{
				const FName TableId = StringTable->GetStringTableId();
				if (!TableId.IsNone() && FStringTableRegistry::Get().FindStringTable(TableId).IsValid())
				{
					if (FStringTableRegistry::Get().UnregisterStringTable(TableId))
					{
						UnregisteredStringTables.Add(TableId.ToString());
					}
				}
			}
		}
	}

	int32 NumObjectDeletesReported = 0;
	if (!bDryRun && ObjectsToDelete.Num() > 0)
	{
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		NumObjectDeletesReported = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

		// Restore the dirty flag on every package that survived the delete. A
		// surviving package still holds the user's unsaved edits, and leaving it
		// clean would let them be dropped without a save prompt.
		for (const TPair<UPackage*, bool>& DirtyPair : PreDeleteDirtyPackages)
		{
			UPackage* Package = DirtyPair.Key;
			if (DirtyPair.Value && IsValid(Package))
			{
				Package->SetDirtyFlag(true);
			}
		}
	}

	TArray<FString> EvictedPackages;
	TArray<FString> StalePackages;
	TArray<FString> DeletedResidualFiles;
	TArray<FString> ResidualFiles;
	TArray<FString> SourceControlOperations;
	TArray<FString> SourceControlFailures;
	TArray<FString> AssetRegistryFilesToRescan;
	TArray<FString> AssetRegistryPathsToRescan;
	if (!bDryRun)
	{
		if (ObjectsToDelete.Num() > 0)
		{
			// Match the editor's asset-delete GC policy: deleted targets have already lost their keep
			// flags, while unrelated RF_Standalone assets must survive concurrent workflow cleanup.
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
		for (FDeleteAssetTarget& Target : Targets)
		{
			const bool bShouldEvict = bForce || Target.bFoundObject;
			if (bShouldEvict)
			{
				EvictLoadedPackageForDelete(Target.PackageName, EvictedPackages, StalePackages);
			}

			// force=true previously removed the package file for every normalized
			// package, even when the exact object was never found. A typo such as
			// /Game/Hero.Hreo normalizes to package /Game/Hero and would destroy
			// the valid package; selecting one asset in a multi-asset package
			// would likewise take its unrequested siblings. Require both an exact
			// hit and proof that nothing unrequested lives in the package.
			if (bForce && Target.bFoundObject && Target.bPackageExclusivelyRequested)
			{
				const int32 DeletedResidualFilesBefore = DeletedResidualFiles.Num();
				const int32 SourceControlFailuresBefore = SourceControlFailures.Num();
				RemoveResidualPackageFilesForDelete(
					Target.PackageName,
					DeletedResidualFiles,
					ResidualFiles,
					SourceControlOperations,
					SourceControlFailures,
					AssetRegistryFilesToRescan,
					AssetRegistryPathsToRescan);
				Target.bResidualRemoved = DeletedResidualFiles.Num() > DeletedResidualFilesBefore;
				Target.bSourceControlFailure = SourceControlFailures.Num() > SourceControlFailuresBefore;
			}
		}

		if (AssetRegistryFilesToRescan.Num() > 0 || AssetRegistryPathsToRescan.Num() > 0)
		{
			if (AssetRegistryFilesToRescan.Num() > 0)
			{
				AssetRegistry.ScanModifiedAssetFiles(AssetRegistryFilesToRescan);
			}
			if (AssetRegistryPathsToRescan.Num() > 0)
			{
				AssetRegistry.ScanPathsSynchronous(AssetRegistryPathsToRescan, /*bForceRescan=*/true);
			}
		}
		if (ObjectsToDelete.Num() > 0 || bForce)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}
	}

	if (!bDryRun && bRequireSourceControl)
	{
		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		for (FDeleteAssetTarget& Target : Targets)
		{
			for (FDeleteAssetTarget::FSourceControlExpectation& Expectation : Target.SourceControlExpectations)
			{
				FSourceControlStatePtr State = Provider.GetState(
					Expectation.Filename,
					EStateCacheUsage::ForceUpdate);
				Expectation.bStateValidAfter = State.IsValid();
				if (State.IsValid())
				{
					Expectation.bSourceControlledAfter = State->IsSourceControlled();
					Expectation.bAddedAfter = State->IsAdded();
					Expectation.bDeletedAfter = State->IsDeleted();
					Expectation.bUnknownAfter = State->IsUnknown();
				}

				const bool bFileAbsent = !IFileManager::Get().FileExists(*Expectation.Filename);
				switch (Expectation.ExpectedPostcondition)
				{
				case FDeleteAssetTarget::ESourceControlPostcondition::MarkedForDelete:
					Expectation.bPostconditionMet = State.IsValid()
						&& State->IsDeleted()
						&& !State->IsAdded()
						&& bFileAbsent;
					break;
				case FDeleteAssetTarget::ESourceControlPostcondition::PendingAddRemoved:
				case FDeleteAssetTarget::ESourceControlPostcondition::UntrackedAbsent:
					Expectation.bPostconditionMet = State.IsValid()
						&& !State->IsSourceControlled()
						&& !State->IsAdded()
						&& !State->IsDeleted()
						&& !State->IsUnknown()
						&& bFileAbsent;
					break;
				default:
					Expectation.bPostconditionMet = false;
					break;
				}

				if (!Expectation.bPostconditionMet)
				{
					Expectation.FailureReason = TEXT("source_control_delete_postcondition_failed");
					Target.bSourceControlFailure = true;
					SourceControlFailures.AddUnique(FString::Printf(
						TEXT("%s:%s"),
						*Expectation.FailureReason,
						*Expectation.Filename));
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> NotFoundArray;
	for (const FString& Path : NotFound)
	{
		NotFoundArray.Add(MakeShared<FJsonValueString>(Path));
	}

	bool bSuccess = true;
	int32 NumDeletedTargets = 0;
	TArray<TSharedPtr<FJsonValue>> TargetResults;
	TArray<TSharedPtr<FJsonValue>> FailedArray;
	if (!bDryRun)
	{
		StalePackages.Reset();
		ResidualFiles.Reset();
	}
	TargetResults.Reserve(Targets.Num());
	for (FDeleteAssetTarget& Target : Targets)
	{
		if (bDryRun)
		{
			const bool bHasDeletableState = Target.bFoundObject
				|| Target.bHadRegisteredAsset
				|| Target.bHadLoadedPackage
				|| Target.InitialPackageFiles.Num() > 0;
			if (bForce)
			{
				Target.bSucceeded = true;
				if (Target.bFoundObject || Target.bHadRegisteredAsset || Target.bHadLoadedPackage)
				{
					Target.Status = TEXT("would_delete");
				}
				else if (Target.InitialPackageFiles.Num() > 0)
				{
					Target.Status = TEXT("would_remove_residual");
				}
				else
				{
					Target.Status = TEXT("already_absent");
				}
			}
			else if (bHasDeletableState && Target.bFoundObject && Target.NotFoundRequestedPaths.Num() == 0)
			{
				Target.bSucceeded = true;
				Target.Status = TEXT("would_delete");
			}
			else
			{
				Target.bSucceeded = false;
				Target.Status = TEXT("not_found");
				Target.FailureReason = TEXT("asset_not_found");
			}
		}
		else
		{
			Target.bFinalLoadedPackage = FindPackage(nullptr, *Target.PackageName) != nullptr;
			Target.bFinalRegisteredAsset = HasRegisteredAssetsForPackage(AssetRegistry, Target.PackageName);
			Target.FinalPackageFiles = FindExistingPackageFiles(Target.PackageName);
			const bool bFinalAbsent = !Target.bFinalLoadedPackage
				&& !Target.bFinalRegisteredAsset
				&& Target.FinalPackageFiles.Num() == 0;
			if (Target.bFinalLoadedPackage)
			{
				StalePackages.Add(Target.PackageName);
			}
			for (const FString& FinalPackageFile : Target.FinalPackageFiles)
			{
				ResidualFiles.AddUnique(FinalPackageFile);
			}

			if (bForce)
			{
				Target.bSucceeded = bFinalAbsent && !Target.bSourceControlFailure;
				if (!Target.bSucceeded)
				{
					Target.Status = TEXT("failed");
				}
				else if (Target.bFoundObject || Target.bHadRegisteredAsset || Target.bHadLoadedPackage)
				{
					Target.Status = TEXT("deleted");
				}
				else if (Target.InitialPackageFiles.Num() > 0)
				{
					Target.Status = TEXT("residual_removed");
				}
				else
				{
					Target.Status = TEXT("already_absent");
				}
			}
			else
			{
				Target.bSucceeded = bFinalAbsent
					&& Target.bFoundObject
					&& Target.NotFoundRequestedPaths.Num() == 0
					&& (!bRequireSourceControl || !Target.bSourceControlFailure);
				Target.Status = Target.bSucceeded ? TEXT("deleted") : TEXT("failed");
			}

			if (!Target.bSucceeded)
			{
				TArray<FString> FailureReasons;
				if (!bForce && (!Target.bFoundObject || Target.NotFoundRequestedPaths.Num() > 0))
				{
					FailureReasons.Add(TEXT("asset_not_found"));
				}
				if (Target.bFinalLoadedPackage)
				{
					FailureReasons.Add(TEXT("loaded_package_remaining"));
				}
				if (Target.bFinalRegisteredAsset)
				{
					FailureReasons.Add(TEXT("asset_registry_entry_remaining"));
				}
				if (Target.FinalPackageFiles.Num() > 0)
				{
					FailureReasons.Add(TEXT("package_file_remaining"));
				}
				if (Target.bSourceControlFailure)
				{
					FailureReasons.Add(TEXT("source_control_operation_failed"));
				}
				Target.FailureReason = FString::Join(FailureReasons, TEXT(","));
				FailedArray.Add(MakeShared<FJsonValueString>(Target.RequestedPath));
			}
		}

		bSuccess = bSuccess && Target.bSucceeded;
		if (Target.Status == TEXT("deleted") || Target.Status == TEXT("residual_removed"))
		{
			++NumDeletedTargets;
		}

		TSharedPtr<FJsonObject> TargetResult = MakeShared<FJsonObject>();
		TargetResult->SetStringField(TEXT("requested_path"), Target.RequestedPath);
		TargetResult->SetStringField(TEXT("package_name"), Target.PackageName);
		TargetResult->SetStringField(TEXT("status"), Target.Status);
		TargetResult->SetBoolField(TEXT("success"), Target.bSucceeded);
		TargetResult->SetBoolField(TEXT("asset_found"), Target.bFoundObject);
		TargetResult->SetBoolField(TEXT("asset_registry_found_before"), Target.bHadRegisteredAsset);
		TargetResult->SetBoolField(TEXT("loaded_package_found_before"), Target.bHadLoadedPackage);
		TargetResult->SetBoolField(TEXT("package_file_found_before"), Target.InitialPackageFiles.Num() > 0);
		TargetResult->SetBoolField(TEXT("residual_removed"), Target.bResidualRemoved);
		TargetResult->SetBoolField(TEXT("source_control_failure"), Target.bSourceControlFailure);
		if (bRequireSourceControl)
		{
			TArray<TSharedPtr<FJsonValue>> SourceControlRows;
			for (const FDeleteAssetTarget::FSourceControlExpectation& Expectation : Target.SourceControlExpectations)
			{
				TSharedPtr<FJsonObject> SourceControlRow = MakeShared<FJsonObject>();
				SourceControlRow->SetStringField(TEXT("filename"), Expectation.Filename);
				SourceControlRow->SetStringField(
					TEXT("expected_postcondition"),
					SourceControlPostconditionToString(Expectation.ExpectedPostcondition));
				SourceControlRow->SetBoolField(TEXT("source_controlled_before"), Expectation.bSourceControlledBefore);
				SourceControlRow->SetBoolField(TEXT("added_before"), Expectation.bAddedBefore);
				SourceControlRow->SetBoolField(TEXT("deleted_before"), Expectation.bDeletedBefore);
				SourceControlRow->SetBoolField(TEXT("checked_out_before"), Expectation.bCheckedOutBefore);
				if (!bDryRun)
				{
					SourceControlRow->SetBoolField(TEXT("state_valid_after"), Expectation.bStateValidAfter);
					SourceControlRow->SetBoolField(TEXT("source_controlled_after"), Expectation.bSourceControlledAfter);
					SourceControlRow->SetBoolField(TEXT("added_after"), Expectation.bAddedAfter);
					SourceControlRow->SetBoolField(TEXT("deleted_after"), Expectation.bDeletedAfter);
					SourceControlRow->SetBoolField(TEXT("unknown_after"), Expectation.bUnknownAfter);
					SourceControlRow->SetBoolField(TEXT("postcondition_met"), Expectation.bPostconditionMet);
				}
				if (!Expectation.FailureReason.IsEmpty())
				{
					SourceControlRow->SetStringField(TEXT("failure_reason"), Expectation.FailureReason);
				}
				SourceControlRows.Add(MakeShared<FJsonValueObject>(SourceControlRow));
			}
			TargetResult->SetArrayField(TEXT("source_control_expectations"), SourceControlRows);
		}
		if (Target.NotFoundRequestedPaths.Num() > 0)
		{
			TargetResult->SetArrayField(TEXT("not_found_paths"), ToJsonStringArray(Target.NotFoundRequestedPaths));
		}
		if (!bDryRun)
		{
			TargetResult->SetBoolField(TEXT("postcondition_met"), Target.bSucceeded);
			TargetResult->SetBoolField(TEXT("loaded_package_remaining"), Target.bFinalLoadedPackage);
			TargetResult->SetBoolField(TEXT("asset_registry_entry_remaining"), Target.bFinalRegisteredAsset);
			TargetResult->SetArrayField(TEXT("residual_files"), ToJsonStringArray(Target.FinalPackageFiles));
		}
		if (!Target.FailureReason.IsEmpty())
		{
			TargetResult->SetStringField(TEXT("failure_reason"), Target.FailureReason);
		}
		TargetResults.Add(MakeShared<FJsonValueObject>(TargetResult));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("force"), bForce);
	Result->SetBoolField(TEXT("require_source_control"), bRequireSourceControl);
	Result->SetNumberField(TEXT("deleted"), NumDeletedTargets);
	Result->SetNumberField(TEXT("object_delete_reported"), NumObjectDeletesReported);
	Result->SetNumberField(TEXT("requested"), AssetPaths.Num());
	int32 FoundTargetCount = 0;
	for (const FDeleteAssetTarget& Target : Targets)
	{
		FoundTargetCount += Target.bFoundObject ? 1 : 0;
	}
	Result->SetNumberField(TEXT("found"), FoundTargetCount);
	Result->SetArrayField(TEXT("not_found"), NotFoundArray);
	Result->SetArrayField(TEXT("targets"), TargetResults);
	if (!bDryRun && EvictedPackages.Num() > 0)
	{
		Result->SetArrayField(TEXT("evicted_packages"), ToJsonStringArray(EvictedPackages));
	}
	if (!bDryRun && StalePackages.Num() > 0)
	{
		Result->SetArrayField(TEXT("stale_packages"), ToJsonStringArray(StalePackages));
	}
	if (!bDryRun && DeletedResidualFiles.Num() > 0)
	{
		Result->SetArrayField(TEXT("deleted_residual_files"), ToJsonStringArray(DeletedResidualFiles));
	}
	if (!bDryRun && ResidualFiles.Num() > 0)
	{
		Result->SetArrayField(TEXT("residual_files"), ToJsonStringArray(ResidualFiles));
	}
	if (!bDryRun && SourceControlOperations.Num() > 0)
	{
		Result->SetArrayField(TEXT("source_control_operations"), ToJsonStringArray(SourceControlOperations));
	}
	if (!bDryRun && SourceControlFailures.Num() > 0)
	{
		Result->SetArrayField(TEXT("source_control_failures"), ToJsonStringArray(SourceControlFailures));
	}
	if (!bDryRun && UnregisteredStringTables.Num() > 0)
	{
		Result->SetArrayField(TEXT("unregistered_string_tables"), ToJsonStringArray(UnregisteredStringTables));
	}
	if (!bDryRun && FailedArray.Num() > 0)
	{
		Result->SetArrayField(TEXT("failed_to_delete"), FailedArray);
	}
	return FMonolithActionResult::Success(Result);
}
