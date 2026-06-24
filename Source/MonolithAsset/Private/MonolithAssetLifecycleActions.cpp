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
#include "HAL/PlatformFileManager.h"
#include "IAssetTools.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
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
#include "UObject/UObjectGlobals.h"

namespace
{
	bool ReadStringAlias(const TSharedPtr<FJsonObject>& Params, const TCHAR* Primary, const TCHAR* Alternate, FString& OutValue)
	{
		return Params->TryGetStringField(Primary, OutValue)
			|| (Alternate && Params->TryGetStringField(Alternate, OutValue));
	}

	bool ParseSettingsObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutSettings)
	{
		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && SettingsObj->IsValid())
		{
			OutSettings = *SettingsObj;
			return true;
		}

		FString SettingsJson;
		if (Params->TryGetStringField(TEXT("settings"), SettingsJson) && !SettingsJson.IsEmpty())
		{
			OutSettings = FMonolithJsonUtils::Parse(SettingsJson);
			return OutSettings.IsValid();
		}

		return false;
	}

	bool ReadBoolSetting(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Settings, const TCHAR* Name, bool& InOutValue)
	{
		if (Settings.IsValid() && Settings->TryGetBoolField(Name, InOutValue))
		{
			return true;
		}
		return Params->TryGetBoolField(Name, InOutValue);
	}

	bool ReadStringSetting(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Settings, const TCHAR* Name, FString& OutValue)
	{
		if (Settings.IsValid() && Settings->TryGetStringField(Name, OutValue))
		{
			return true;
		}
		return Params->TryGetStringField(Name, OutValue);
	}

	bool ReadIntSetting(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Settings, const TCHAR* Name, int32& OutValue)
	{
		double Value = 0.0;
		if (Settings.IsValid() && Settings->TryGetNumberField(Name, Value))
		{
			OutValue = static_cast<int32>(Value);
			return true;
		}
		if (Params->TryGetNumberField(Name, Value))
		{
			OutValue = static_cast<int32>(Value);
			return true;
		}
		return false;
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

	TArray<FString> ReadStringArray(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		TArray<FString> Result;
		Result.Reserve(Values.Num());
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				Result.Add(StringValue);
			}
		}
		return Result;
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
		CollectGarbage(RF_NoFlags);

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
			CollectGarbage(RF_NoFlags);
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

	bool RemoveResidualPackageFileForDelete(
		const FString& PackageName,
		TArray<FString>& OutDeletedFiles,
		TArray<FString>& OutResidualFiles,
		TArray<FString>& OutSourceControlOperations,
		TArray<FString>& OutSourceControlFailures,
		TArray<FString>& OutFilesToRescan,
		TArray<FString>& OutPathsToRescan)
	{
		FString PackageFilename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		PackageFilename = FPaths::ConvertRelativePathToFull(PackageFilename);
		FPaths::NormalizeFilename(PackageFilename);

		OutFilesToRescan.AddUnique(PackageFilename);
		OutPathsToRescan.AddUnique(FPackageName::GetLongPackagePath(PackageName));

		if (!IFileManager::Get().FileExists(*PackageFilename))
		{
			return true;
		}

		bool bSourceControlOperationSucceeded = false;
		bool bSourceControlledBlockingState = false;

		ISourceControlModule& SourceControlModule = ISourceControlModule::Get();
		if (SourceControlModule.IsEnabled())
		{
			ISourceControlProvider& Provider = SourceControlModule.GetProvider();
			if (Provider.IsEnabled() && Provider.IsAvailable())
			{
				const FSourceControlStatePtr State = Provider.GetState(PackageFilename, EStateCacheUsage::ForceUpdate);
				if (State.IsValid())
				{
					if (State->IsAdded() && State->CanRevert())
					{
						bSourceControlOperationSucceeded = ExecuteResidualSourceControlOperation(
							Provider,
							PackageFilename,
							TEXT("revert_added"),
							ISourceControlOperation::Create<FRevert>(),
							OutSourceControlOperations,
							OutSourceControlFailures);
					}
					else if (!State->IsDeleted() && State->CanDelete())
					{
						bSourceControlOperationSucceeded = ExecuteResidualSourceControlOperation(
							Provider,
							PackageFilename,
							TEXT("delete"),
							ISourceControlOperation::Create<FDelete>(),
							OutSourceControlOperations,
							OutSourceControlFailures);
					}
					else
					{
						bSourceControlledBlockingState = State->IsSourceControlled()
							&& !State->IsAdded()
							&& !State->IsDeleted();
					}
				}
			}
		}

		if (!IFileManager::Get().FileExists(*PackageFilename))
		{
			OutDeletedFiles.Add(PackageFilename);
			return true;
		}

		const bool bCanDirectDeleteResidual = bSourceControlOperationSucceeded
			|| (!bSourceControlledBlockingState && !IFileManager::Get().IsReadOnly(*PackageFilename));
		if (bCanDirectDeleteResidual
			&& IFileManager::Get().Delete(*PackageFilename, /*RequireExists=*/false, /*EvenReadOnly=*/bSourceControlOperationSucceeded, /*Quiet=*/true))
		{
			OutDeletedFiles.Add(PackageFilename);
			return true;
		}

		OutResidualFiles.Add(PackageFilename);
		return false;
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
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("save_asset"),
		TEXT("Save a loaded asset package to disk."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetLifecycleActions::SaveAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to save"))
			.Build());

	Registry.RegisterAction(TEXT("asset"), TEXT("delete_assets"),
		TEXT("Delete UE assets by path. Optional safety: restrict to allowed path prefixes."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetLifecycleActions::DeleteAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of UE asset paths to delete"))
			.Optional(TEXT("allowed_prefixes"), TEXT("array"), TEXT("Only paths starting with these prefixes may be deleted"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Validate and report targets without deleting"), TEXT("false"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("Force-delete referenced assets after closing open editors. Default false"), TEXT("false"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("asset"), TEXT("import_texture_from_file"),
		{ TEXT("import image"), TEXT("png to texture"), TEXT("load texture from disk"), TEXT("ingest external image"), TEXT("texture2d from file") },
		{ TEXT("import texture"), TEXT("add texture"), TEXT("create texture from image") },
		{ TEXT("import D:/art/rock.png as a Texture2D under /Game/Textures"), TEXT("bring this PNG into the project as a texture") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("asset"), TEXT("save_asset"),
		{ TEXT("write to disk"), TEXT("persist asset"), TEXT("flush package"), TEXT("commit changes"), TEXT("unsaved asset") },
		{ TEXT("save package"), TEXT("save"), TEXT("save changes") },
		{ TEXT("save /Game/Textures/T_Rock to disk"), TEXT("persist my edits to this asset") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("asset"), TEXT("delete_assets"),
		{ TEXT("remove asset"), TEXT("erase asset"), TEXT("force delete referenced"), TEXT("clean up assets"), TEXT("delete multiple") },
		{ TEXT("delete asset"), TEXT("remove"), TEXT("trash asset") },
		{ TEXT("delete /Game/Old/T_Unused"), TEXT("remove these unreferenced assets safely") });
}

FMonolithActionResult FMonolithAssetLifecycleActions::ImportTextureFromFile(const TSharedPtr<FJsonObject>& Params)
{
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
	Params->TryGetStringField(TEXT("asset_name"), AssetName);
	if (!AssetName.IsEmpty() && !Destination.IsEmpty())
	{
		Destination = Destination / AssetName;
	}

	if (SourcePath.IsEmpty() || Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path and destination are required. Aliases: file_path/source_file/path and destination_path/dest_path; destination_path may be paired with asset_name."));
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
	ParseSettingsObject(Params, Settings);

	bool bReplaceExisting = false;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
	FString OverwritePolicy;
	if (!bReplaceExisting && Params->TryGetStringField(TEXT("overwrite_policy"), OverwritePolicy))
	{
		const FString Policy = OverwritePolicy.ToLower();
		if (Policy == TEXT("overwrite") || Policy == TEXT("replace") || Policy == TEXT("replace_existing"))
		{
			bReplaceExisting = true;
		}
	}

	TextureCompressionSettings Compression = TC_Default;
	FString CompressionStr;
	if (ReadStringSetting(Params, Settings, TEXT("compression"), CompressionStr)
		&& !ParseTextureCompression(CompressionStr, Compression))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid compression setting: '%s'"), *CompressionStr));
	}

	bool bSRGB = true;
	ReadBoolSetting(Params, Settings, TEXT("srgb"), bSRGB);

	bool bTiling = false;
	ReadBoolSetting(Params, Settings, TEXT("tiling"), bTiling);

	TextureGroup LODGroup = TEXTUREGROUP_World;
	FString LODGroupStr;
	if (ReadStringSetting(Params, Settings, TEXT("lod_group"), LODGroupStr)
		&& !ParseTextureLODGroup(LODGroupStr, LODGroup))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Invalid lod_group: '%s'"), *LODGroupStr));
	}

	int32 MaxSize = 0;
	ReadIntSetting(Params, Settings, TEXT("max_size"), MaxSize);

	const FString FinalAssetPath = DestinationPackagePath / DestinationName;
	if (!bReplaceExisting && UEditorAssetLibrary::DoesAssetExist(FinalAssetPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Asset already exists at '%s'. Set replace_existing=true to overwrite."),
			*FinalAssetPath));
	}

	UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
	ImportTask->Filename = SourcePath;
	ImportTask->DestinationPath = DestinationPackagePath;
	ImportTask->DestinationName = DestinationName;
	ImportTask->bAutomated = true;
	ImportTask->bReplaceExisting = bReplaceExisting;
	ImportTask->bSave = true;

	TArray<UAssetImportTask*> Tasks;
	Tasks.Add(ImportTask);
	FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks(Tasks);

	UObject* ImportedObject = FMonolithAssetUtils::LoadAssetByPath(FinalAssetPath);
	UTexture2D* Texture = ImportedObject ? Cast<UTexture2D>(ImportedObject) : nullptr;
	FString ResolvedAssetPath = FinalAssetPath;

	if (!Texture)
	{
		const FString FallbackPath = DestinationPackagePath / FPaths::GetBaseFilename(SourcePath);
		ImportedObject = FMonolithAssetUtils::LoadAssetByPath(FallbackPath);
		Texture = ImportedObject ? Cast<UTexture2D>(ImportedObject) : nullptr;
		if (Texture)
		{
			ResolvedAssetPath = FallbackPath;
		}
	}

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
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save imported texture asset: %s"), *ResolvedAssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
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

	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
	}

	const bool bWasDirty = Asset->GetOutermost()->IsDirty();
	const bool bSaved = UEditorAssetLibrary::SaveLoadedAsset(Asset, false);
	if (!bSaved)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("saved"), true);
	Result->SetBoolField(TEXT("was_dirty"), bWasDirty);
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

	const TArray<FString> AssetPaths = ReadStringArray(*AssetPathsArray);
	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid paths in asset_paths"));
	}

	TArray<FString> RequestedPackageNames;
	RequestedPackageNames.Reserve(AssetPaths.Num());
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
		RequestedPackageNames.AddUnique(PackageName);
	}

	TArray<FString> AllowedPrefixes;
	const TArray<TSharedPtr<FJsonValue>>* PrefixArray = nullptr;
	if (Params->TryGetArrayField(TEXT("allowed_prefixes"), PrefixArray) && PrefixArray)
	{
		AllowedPrefixes = ReadStringArray(*PrefixArray);
	}

	if (AllowedPrefixes.Num() > 0)
	{
		for (const FString& Path : AssetPaths)
		{
			bool bAllowed = false;
			for (const FString& Prefix : AllowedPrefixes)
			{
				if (Path.StartsWith(Prefix))
				{
					bAllowed = true;
					break;
				}
			}
			if (!bAllowed)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Refusing to delete '%s' because it is not under any allowed_prefixes entry: %s"),
					*Path,
					*FString::Join(AllowedPrefixes, TEXT(", "))));
			}
		}
	}

	TArray<UObject*> ObjectsToDelete;
	TArray<FString> NotFound;
	TArray<FString> NotFoundPackageNames;
	for (const FString& Path : AssetPaths)
	{
		if (UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Path))
		{
			ObjectsToDelete.Add(Asset);
		}
		else
		{
			NotFound.Add(Path);
			FString PackageName;
			FString PathError;
			if (TryNormalizeAssetPackageName(Path, PackageName, PathError))
			{
				NotFoundPackageNames.AddUnique(PackageName);
			}
		}
	}

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	TArray<FString> AttemptedPaths;
	AttemptedPaths.Reserve(ObjectsToDelete.Num());
	TArray<FString> UnregisteredStringTables;
	for (UObject* Asset : ObjectsToDelete)
	{
		if (!Asset)
		{
			continue;
		}

		AttemptedPaths.Add(Asset->GetPathName());

		if (UPackage* Package = Asset->GetOutermost())
		{
			Package->SetDirtyFlag(false);
		}
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
			}
		}
		if (!bDryRun)
		{
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

	int32 NumDeleted = 0;
	if (!bDryRun && ObjectsToDelete.Num() > 0)
	{
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		NumDeleted = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
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
		const bool bUsePackageFileDelete = bForce && (NumDeleted < ObjectsToDelete.Num());
		const TArray<FString>& PackageNamesToEvict = bForce
			? RequestedPackageNames
			: NotFoundPackageNames;

		CollectGarbage(RF_NoFlags);
		for (const FString& PackageName : PackageNamesToEvict)
		{
			const int32 StaleBefore = StalePackages.Num();
			const bool bEvicted = EvictLoadedPackageForDelete(PackageName, EvictedPackages, StalePackages);
			bool bResidualRemoved = true;
			if (bUsePackageFileDelete)
			{
				bResidualRemoved = RemoveResidualPackageFileForDelete(
					PackageName,
					DeletedResidualFiles,
					ResidualFiles,
					SourceControlOperations,
					SourceControlFailures,
					AssetRegistryFilesToRescan,
					AssetRegistryPathsToRescan);
			}
			if (bUsePackageFileDelete && bEvicted && bResidualRemoved && StalePackages.Num() == StaleBefore)
			{
				++NumDeleted;
			}
		}

		if (AssetRegistryFilesToRescan.Num() > 0 || AssetRegistryPathsToRescan.Num() > 0)
		{
			FAssetRegistryModule& AssetRegistryModule =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
			if (AssetRegistryFilesToRescan.Num() > 0)
			{
				AssetRegistryModule.Get().ScanModifiedAssetFiles(AssetRegistryFilesToRescan);
			}
			if (AssetRegistryPathsToRescan.Num() > 0)
			{
				AssetRegistryModule.Get().ScanPathsSynchronous(AssetRegistryPathsToRescan, /*bForceRescan=*/true);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> NotFoundArray;
	for (const FString& Path : NotFound)
	{
		NotFoundArray.Add(MakeShared<FJsonValueString>(Path));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bSuccess = bDryRun
		? NotFound.Num() == 0
		: (bForce
			? (StalePackages.Num() == 0 && ResidualFiles.Num() == 0)
			: (NumDeleted == ObjectsToDelete.Num()
				&& NotFound.Num() == 0
				&& StalePackages.Num() == 0
				&& ResidualFiles.Num() == 0));
	Result->SetBoolField(TEXT("success"), bSuccess);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("force"), bForce);
	Result->SetNumberField(TEXT("deleted"), NumDeleted);
	Result->SetNumberField(TEXT("requested"), AssetPaths.Num());
	Result->SetNumberField(TEXT("found"), ObjectsToDelete.Num());
	Result->SetArrayField(TEXT("not_found"), NotFoundArray);
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
	if (!bDryRun && NumDeleted < ObjectsToDelete.Num())
	{
		TArray<TSharedPtr<FJsonValue>> FailedArray;
		for (const FString& Path : AttemptedPaths)
		{
			FailedArray.Add(MakeShared<FJsonValueString>(Path));
		}
		Result->SetArrayField(TEXT("failed_to_delete"), FailedArray);
	}
	return FMonolithActionResult::Success(Result);
}
