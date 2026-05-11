#include "MonolithMeshInterchangeActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

namespace
{
	struct FInterchangeFormatDef
	{
		const TCHAR* Extension;
		const TCHAR* Category;
		const TCHAR* Description;
		bool bSceneCapable;
		bool bCommonExport;
	};

	const TArray<FInterchangeFormatDef>& GetFormatDefs()
	{
		static const TArray<FInterchangeFormatDef> Defs = {
			{ TEXT("fbx"), TEXT("mesh"), TEXT("FBX mesh or scene file"), true, true },
			{ TEXT("obj"), TEXT("mesh"), TEXT("Wavefront OBJ static mesh"), false, true },
			{ TEXT("glb"), TEXT("scene"), TEXT("Binary glTF scene or mesh"), true, true },
			{ TEXT("gltf"), TEXT("scene"), TEXT("glTF scene or mesh"), true, true },
			{ TEXT("usd"), TEXT("scene"), TEXT("Universal Scene Description file"), true, true },
			{ TEXT("usda"), TEXT("scene"), TEXT("ASCII Universal Scene Description file"), true, true },
			{ TEXT("usdc"), TEXT("scene"), TEXT("Binary Universal Scene Description file"), true, true },
			{ TEXT("abc"), TEXT("scene"), TEXT("Alembic geometry cache or scene"), true, false },
			{ TEXT("png"), TEXT("texture"), TEXT("PNG texture image"), false, true },
			{ TEXT("jpg"), TEXT("texture"), TEXT("JPEG texture image"), false, true },
			{ TEXT("jpeg"), TEXT("texture"), TEXT("JPEG texture image"), false, true },
			{ TEXT("tga"), TEXT("texture"), TEXT("TGA texture image"), false, true },
			{ TEXT("bmp"), TEXT("texture"), TEXT("BMP texture image"), false, false },
			{ TEXT("exr"), TEXT("texture"), TEXT("OpenEXR texture image"), false, true },
			{ TEXT("wav"), TEXT("audio"), TEXT("Wave audio file"), false, false }
		};
		return Defs;
	}

	TArray<FString> GetInterchangeModuleNames()
	{
		return {
			TEXT("InterchangeCore"),
			TEXT("InterchangeEngine"),
			TEXT("InterchangeEditor"),
			TEXT("InterchangePipelines"),
			TEXT("InterchangeFactoryNodes"),
			TEXT("InterchangeImport")
		};
	}

	bool IsInterchangeAvailable()
	{
		for (const FString& ModuleName : GetInterchangeModuleNames())
		{
			if (FModuleManager::Get().ModuleExists(*ModuleName))
			{
				return true;
			}
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> GetModuleStatusRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FString& ModuleName : GetInterchangeModuleNames())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("module"), ModuleName);
			Row->SetBoolField(TEXT("exists"), FModuleManager::Get().ModuleExists(*ModuleName));
			Row->SetBoolField(TEXT("loaded"), FModuleManager::Get().IsModuleLoaded(*ModuleName));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	const FInterchangeFormatDef* FindFormatDef(const FString& Extension)
	{
		const FString Lower = Extension.ToLower();
		for (const FInterchangeFormatDef& Def : GetFormatDefs())
		{
			if (Lower == Def.Extension)
			{
				return &Def;
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> GetFormatRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FInterchangeFormatDef& Def : GetFormatDefs())
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("extension"), Def.Extension);
			Row->SetStringField(TEXT("category"), Def.Category);
			Row->SetStringField(TEXT("description"), Def.Description);
			Row->SetBoolField(TEXT("scene_capable"), Def.bSceneCapable);
			Row->SetBoolField(TEXT("common_export"), Def.bCommonExport);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	FString NormalizeSourceFile(const FString& Input)
	{
		if (Input.IsEmpty())
		{
			return FString();
		}

		FString Path = Input;
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		}
		return FPaths::ConvertRelativePathToFull(Path);
	}

	bool IsUnderRoot(FString Path, FString Root)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Path);
		FPaths::NormalizeDirectoryName(Root);
		return Path.Equals(Root, ESearchCase::IgnoreCase) || FPaths::IsUnderDirectory(Path, Root);
	}

	TArray<TSharedPtr<FJsonValue>> GetAllowedRootRows()
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		auto AddRoot = [&Rows](const FString& Label, const FString& Path)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("label"), Label);
			Row->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Path));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		};

		AddRoot(TEXT("project"), FPaths::ProjectDir());
		AddRoot(TEXT("content"), FPaths::ProjectContentDir());
		AddRoot(TEXT("saved"), FPaths::ProjectSavedDir());
		return Rows;
	}

	bool IsUnderDefaultImportRoots(const FString& SourceFile)
	{
		return IsUnderRoot(SourceFile, FPaths::ProjectDir()) ||
			IsUnderRoot(SourceFile, FPaths::ProjectContentDir()) ||
			IsUnderRoot(SourceFile, FPaths::ProjectSavedDir());
	}

	TSharedPtr<FJsonObject> ValidateDestinationPackage(const FString& DestinationPath)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("destination_path"), DestinationPath);

		if (DestinationPath.IsEmpty())
		{
			Result->SetBoolField(TEXT("provided"), false);
			Result->SetBoolField(TEXT("valid"), true);
			return Result;
		}

		Result->SetBoolField(TEXT("provided"), true);
		FText Reason;
		const bool bValid = FPackageName::IsValidLongPackageName(DestinationPath, false, &Reason);
		Result->SetBoolField(TEXT("valid"), bValid);
		if (!bValid)
		{
			Result->SetStringField(TEXT("reason"), Reason.ToString());
		}
		return Result;
	}

	UAssetImportData* FindAssetImportData(UObject* Asset)
	{
		if (!Asset)
		{
			return nullptr;
		}

		for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (!Property || Property->GetName() != TEXT("AssetImportData"))
			{
				continue;
			}

			if (const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Property))
			{
				return Cast<UAssetImportData>(ObjectProp->GetObjectPropertyValue_InContainer(Asset));
			}
		}
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> SourceFilesToJson(const UAssetImportData* ImportData)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		if (!ImportData)
		{
			return Rows;
		}

		TArray<FString> Files;
		ImportData->ExtractFilenames(Files);
		for (const FString& File : Files)
		{
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			const FString FullPath = NormalizeSourceFile(File);
			Row->SetStringField(TEXT("filename"), File);
			Row->SetStringField(TEXT("full_path"), FullPath);
			Row->SetStringField(TEXT("extension"), FPaths::GetExtension(File, false).ToLower());
			Row->SetBoolField(TEXT("exists"), FPaths::FileExists(FullPath));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}

	UObject* LoadAssetFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), OutAssetPath) || OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}

		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(OutAssetPath);
		if (!Asset)
		{
			OutError = FString::Printf(TEXT("Asset not found at '%s'"), *OutAssetPath);
		}
		return Asset;
	}
}

void FMonolithMeshInterchangeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("interchange"), TEXT("get_supported_formats"),
		TEXT("List Monolith Interchange import/export validation capabilities without mutating assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInterchangeActions::GetSupportedFormats),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("can_import"),
		TEXT("Validate whether a source file can be handed to an Interchange import workflow."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInterchangeActions::CanImport),
		FParamSchemaBuilder()
			.Required(TEXT("source_file"), TEXT("string"), TEXT("Source file to validate"))
			.Optional(TEXT("destination_path"), TEXT("string"), TEXT("Optional /Game destination package path"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("can_reimport"),
		TEXT("Check whether an existing asset has source import data usable for reimport."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInterchangeActions::CanReimport),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("get_import_data"),
		TEXT("Read import source metadata from an existing asset without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshInterchangeActions::GetImportData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Build());
}

FMonolithActionResult FMonolithMeshInterchangeActions::GetSupportedFormats(const TSharedPtr<FJsonObject>&)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("mutation_actions_implemented"), false);
	Result->SetBoolField(TEXT("interchange_available"), IsInterchangeAvailable());
	Result->SetArrayField(TEXT("modules"), GetModuleStatusRows());
	Result->SetArrayField(TEXT("formats"), GetFormatRows());
	Result->SetArrayField(TEXT("default_allowed_roots"), GetAllowedRootRows());
	Result->SetStringField(TEXT("policy"), TEXT("First milestone validates source files, destination packages, and reimport metadata. Import/export mutation remains deferred."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshInterchangeActions::CanImport(const TSharedPtr<FJsonObject>& Params)
{
	FString SourceFile;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'source_file'"));
	}

	bool bAllowExternal = false;
	Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);

	FString DestinationPath;
	Params->TryGetStringField(TEXT("destination_path"), DestinationPath);

	const FString NormalizedSource = NormalizeSourceFile(SourceFile);
	const FString Extension = FPaths::GetExtension(NormalizedSource, false).ToLower();
	const FInterchangeFormatDef* Format = FindFormatDef(Extension);
	const bool bFileExists = FPaths::FileExists(NormalizedSource);
	const bool bUnderRoots = IsUnderDefaultImportRoots(NormalizedSource);
	const bool bDestinationValid = !DestinationPath.IsEmpty()
		? FPackageName::IsValidLongPackageName(DestinationPath, false)
		: true;
	const bool bInterchangeAvailable = IsInterchangeAvailable();
	const bool bCanImport = bInterchangeAvailable && bFileExists && Format != nullptr && (bAllowExternal || bUnderRoots) && bDestinationValid;

	TArray<TSharedPtr<FJsonValue>> Issues;
	auto AddIssue = [&Issues](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	if (!bInterchangeAvailable)
	{
		AddIssue(TEXT("interchange_unavailable"), TEXT("No Interchange module was found in the current engine/project module set."));
	}
	if (!bFileExists)
	{
		AddIssue(TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
	}
	if (!Format)
	{
		AddIssue(TEXT("unsupported_extension"), FString::Printf(TEXT("No first-milestone format metadata for extension '%s'."), *Extension));
	}
	if (!bUnderRoots && !bAllowExternal)
	{
		AddIssue(TEXT("external_source_blocked"), TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
	}
	if (!bDestinationValid)
	{
		AddIssue(TEXT("invalid_destination_path"), TEXT("destination_path must be a valid long package path such as /Game/Imported/MyAsset."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("can_import"), bCanImport);
	Result->SetBoolField(TEXT("interchange_available"), bInterchangeAvailable);
	Result->SetStringField(TEXT("source_file"), SourceFile);
	Result->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
	Result->SetStringField(TEXT("extension"), Extension);
	Result->SetBoolField(TEXT("source_exists"), bFileExists);
	Result->SetBoolField(TEXT("under_default_roots"), bUnderRoots);
	Result->SetBoolField(TEXT("allow_external"), bAllowExternal);
	Result->SetObjectField(TEXT("destination"), ValidateDestinationPackage(DestinationPath));
	Result->SetArrayField(TEXT("issues"), Issues);
	if (Format)
	{
		Result->SetStringField(TEXT("category"), Format->Category);
		Result->SetBoolField(TEXT("scene_capable"), Format->bSceneCapable);
		Result->SetStringField(TEXT("description"), Format->Description);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshInterchangeActions::CanReimport(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	const UAssetImportData* ImportData = FindAssetImportData(Asset);
	TArray<TSharedPtr<FJsonValue>> SourceFiles = SourceFilesToJson(ImportData);
	bool bAnyExistingSource = false;
	for (const TSharedPtr<FJsonValue>& Value : SourceFiles)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Row) && Row && Row->IsValid())
		{
			bool bExists = false;
			(*Row)->TryGetBoolField(TEXT("exists"), bExists);
			bAnyExistingSource |= bExists;
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetBoolField(TEXT("has_import_data"), ImportData != nullptr);
	Result->SetBoolField(TEXT("can_reimport"), ImportData != nullptr && bAnyExistingSource && IsInterchangeAvailable());
	Result->SetBoolField(TEXT("interchange_available"), IsInterchangeAvailable());
	Result->SetNumberField(TEXT("source_file_count"), SourceFiles.Num());
	Result->SetArrayField(TEXT("source_files"), SourceFiles);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshInterchangeActions::GetImportData(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	const UAssetImportData* ImportData = FindAssetImportData(Asset);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetBoolField(TEXT("has_import_data"), ImportData != nullptr);
	Result->SetArrayField(TEXT("source_files"), SourceFilesToJson(ImportData));
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}
