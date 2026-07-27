#include "MonolithInterchangeActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "AssetExportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorReimportHandler.h"
#include "EditorFramework/AssetImportData.h"
#include "Exporters/Exporter.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "AutomatedAssetImportData.h"

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

	bool IsGamePackagePath(const FString& PackagePath)
	{
		return PackagePath == TEXT("/Game") || PackagePath.StartsWith(TEXT("/Game/"));
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
		const bool bLongPackageName = FPackageName::IsValidLongPackageName(DestinationPath, false, &Reason);
		const bool bUnderGameRoot = IsGamePackagePath(DestinationPath);
		const bool bValid = bLongPackageName && bUnderGameRoot;
		Result->SetBoolField(TEXT("valid"), bValid);
		Result->SetBoolField(TEXT("under_game_root"), bUnderGameRoot);
		if (!bValid)
		{
			Result->SetStringField(TEXT("reason"),
				bLongPackageName
					? TEXT("destination_path must be under /Game")
					: Reason.ToString());
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

	FString NormalizePackageFolder(FString DestinationPath)
	{
		DestinationPath.TrimStartAndEndInline();
		while (DestinationPath.Len() > 5 && DestinationPath.EndsWith(TEXT("/")))
		{
			DestinationPath.LeftChopInline(1);
		}
		return DestinationPath;
	}

	FString SanitizeAssetName(const FString& Input)
	{
		FString Sanitized = FPaths::GetBaseFilename(Input).Left(80);
		const FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`+=\t\r\n");
		for (int32 Index = 0; Index < InvalidChars.Len(); ++Index)
		{
			const FString InvalidChar = InvalidChars.Mid(Index, 1);
			Sanitized = Sanitized.Replace(*InvalidChar, TEXT("_"));
		}
		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}
		Sanitized.TrimStartAndEndInline();
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("ImportedAsset");
		}
		if (FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Asset_") + Sanitized;
		}
		return Sanitized;
	}

	FString JoinPackagePath(const FString& Folder, const FString& AssetName)
	{
		return NormalizePackageFolder(Folder) / AssetName;
	}

	void AddMessage(TArray<TSharedPtr<FJsonValue>>& Messages, const FString& Code, const FString& Message, const FString& Severity = TEXT("error"))
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("severity"), Severity);
		Obj->SetStringField(TEXT("code"), Code);
		Obj->SetStringField(TEXT("message"), Message);
		Messages.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> StringArrayToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(Value));
		}
		return Rows;
	}

	bool RequireConfirmOrDryRun(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>& Messages, bool& bOutDryRun)
	{
		bOutDryRun = false;
		if (Params.IsValid() && Params->HasField(TEXT("dry_run")) && !Params->TryGetBoolField(TEXT("dry_run"), bOutDryRun))
		{
			AddMessage(Messages, TEXT("invalid_dry_run"), TEXT("dry_run must be a boolean."));
			return false;
		}
		if (bOutDryRun)
		{
			return true;
		}

		bool bConfirm = false;
		if (Params.IsValid() && Params->HasField(TEXT("confirm")) && !Params->TryGetBoolField(TEXT("confirm"), bConfirm))
		{
			AddMessage(Messages, TEXT("invalid_confirm"), TEXT("confirm must be a boolean."));
			return false;
		}
		if (!bConfirm)
		{
			AddMessage(Messages, TEXT("confirmation_required"), TEXT("Mutation requires confirm=true or dry_run=true."));
			return false;
		}
		return true;
	}

	bool TryReadStringArray(const TSharedPtr<FJsonObject>& Params, const FString& Field, TArray<FString>& OutValues, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(Field, Values) || !Values || Values->Num() == 0)
		{
			OutError = FString::Printf(TEXT("Missing required non-empty array param '%s'"), *Field);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::String)
			{
				OutError = FString::Printf(TEXT("Param '%s' must contain only strings"), *Field);
				return false;
			}
			OutValues.Add(Value->AsString());
		}
		return true;
	}

	TSharedPtr<FJsonObject> AssetToJson(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Asset)
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("object_path"), Asset->GetPathName());
		Obj->SetStringField(TEXT("package_path"), Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("asset_name"), Asset->GetName());
		Obj->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> DirtyPackagesToJson(const TArray<UObject*>& Objects)
	{
		TSet<FString> Names;
		for (UObject* Obj : Objects)
		{
			if (Obj && Obj->GetOutermost() && Obj->GetOutermost()->IsDirty())
			{
				Names.Add(Obj->GetOutermost()->GetName());
			}
		}

		TArray<FString> Sorted = Names.Array();
		Sorted.Sort();
		return StringArrayToJson(Sorted);
	}

	bool ValidateOutputFileRoot(const FString& FilePath, bool bAllowExternal)
	{
		return bAllowExternal || IsUnderDefaultImportRoots(FilePath);
	}

	TSharedPtr<FJsonObject> ImportOneSource(const FString& SourceFile, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Messages;

		const FString NormalizedSource = NormalizeSourceFile(SourceFile);
		const FString Extension = FPaths::GetExtension(NormalizedSource, false).ToLower();
		const FInterchangeFormatDef* Format = FindFormatDef(Extension);
		Row->SetStringField(TEXT("source_file"), SourceFile);
		Row->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
		Row->SetStringField(TEXT("extension"), Extension);

		FString DestinationPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
		{
			AddMessage(Messages, TEXT("missing_destination_path"), TEXT("Missing required param 'destination_path'."));
		}
		DestinationPath = NormalizePackageFolder(DestinationPath);
		Row->SetStringField(TEXT("destination_path"), DestinationPath);

		FString ConflictPolicy;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("conflict_policy"), ConflictPolicy) || ConflictPolicy.IsEmpty())
		{
			AddMessage(Messages, TEXT("missing_conflict_policy"), TEXT("Missing required param 'conflict_policy'. Use fail, overwrite, rename, or reimport_only."));
		}
		ConflictPolicy = ConflictPolicy.ToLower();
		Row->SetStringField(TEXT("conflict_policy"), ConflictPolicy);

		bool bAllowExternal = false;
		Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
		Row->SetBoolField(TEXT("allow_external"), bAllowExternal);

		bool bDryRun = false;
		RequireConfirmOrDryRun(Params, Messages, bDryRun);
		Row->SetBoolField(TEXT("dry_run"), bDryRun);

		const bool bInterchangeAvailable = IsInterchangeAvailable();
		const bool bFileExists = FPaths::FileExists(NormalizedSource);
		const bool bUnderRoots = IsUnderDefaultImportRoots(NormalizedSource);
			const bool bDestinationValid = !DestinationPath.IsEmpty() &&
				FPackageName::IsValidLongPackageName(DestinationPath, false) &&
				IsGamePackagePath(DestinationPath);
		const FString ExpectedAssetName = SanitizeAssetName(NormalizedSource);
		const FString ExpectedPackage = !DestinationPath.IsEmpty() ? JoinPackagePath(DestinationPath, ExpectedAssetName) : FString();
		const bool bLikelyConflict = !ExpectedPackage.IsEmpty() && FPackageName::DoesPackageExist(ExpectedPackage);

		Row->SetBoolField(TEXT("interchange_available"), bInterchangeAvailable);
		Row->SetBoolField(TEXT("source_exists"), bFileExists);
		Row->SetBoolField(TEXT("under_default_roots"), bUnderRoots);
		Row->SetStringField(TEXT("expected_asset_name"), ExpectedAssetName);
		Row->SetStringField(TEXT("expected_package"), ExpectedPackage);
		Row->SetBoolField(TEXT("likely_package_conflict"), bLikelyConflict);

		if (!bInterchangeAvailable)
		{
			AddMessage(Messages, TEXT("interchange_unavailable"), TEXT("No Interchange module was found in the current engine/project module set."));
		}
		if (!bFileExists)
		{
			AddMessage(Messages, TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
		}
		if (!Format)
		{
			AddMessage(Messages, TEXT("unsupported_extension"), FString::Printf(TEXT("No format metadata for extension '%s'."), *Extension));
		}
		if (!bUnderRoots && !bAllowExternal)
		{
			AddMessage(Messages, TEXT("external_source_blocked"), TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
		}
		if (!bDestinationValid)
		{
				AddMessage(Messages, TEXT("invalid_destination_path"), TEXT("destination_path must be a valid /Game long package path such as /Game/Imported."));
		}
		if (ConflictPolicy != TEXT("fail") && ConflictPolicy != TEXT("overwrite") && ConflictPolicy != TEXT("rename") && ConflictPolicy != TEXT("reimport_only"))
		{
			AddMessage(Messages, TEXT("invalid_conflict_policy"), TEXT("conflict_policy must be fail, overwrite, rename, or reimport_only."));
		}
		if (ConflictPolicy == TEXT("fail") && bLikelyConflict)
		{
			AddMessage(Messages, TEXT("destination_conflict"), FString::Printf(TEXT("Likely destination package already exists: %s"), *ExpectedPackage));
		}
		if (ConflictPolicy == TEXT("reimport_only"))
		{
			AddMessage(Messages, TEXT("reimport_only_not_supported_for_new_import"), TEXT("Use interchange.reimport_asset for reimport_only workflows."));
		}

		if (Messages.Num() > 0)
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		if (bDryRun)
		{
			Row->SetStringField(TEXT("status"), TEXT("would_import"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
		ImportData->Filenames.Reset();
		ImportData->Filenames.Add(NormalizedSource);
		ImportData->DestinationPath = DestinationPath;
		ImportData->bReplaceExisting = ConflictPolicy == TEXT("overwrite");

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		TArray<UObject*> ImportedObjects = AssetTools.ImportAssetsAutomated(ImportData);
		if (ImportedObjects.Num() == 0)
		{
			AddMessage(Messages, TEXT("import_returned_no_objects"), TEXT("Unreal import returned no objects. Check file type, plugin availability, and import logs."));
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		TArray<TSharedPtr<FJsonValue>> ImportedRows;
		for (UObject* Imported : ImportedObjects)
		{
			ImportedRows.Add(MakeShared<FJsonValueObject>(AssetToJson(Imported)));
		}
		Row->SetStringField(TEXT("status"), TEXT("imported"));
		Row->SetArrayField(TEXT("imported_assets"), ImportedRows);
		Row->SetArrayField(TEXT("dirty_packages"), DirtyPackagesToJson(ImportedObjects));
		Row->SetArrayField(TEXT("messages"), Messages);
		return Row;
	}

	TSharedPtr<FJsonObject> ReimportOneAsset(const FString& AssetPathInput, const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Messages;
		Row->SetStringField(TEXT("asset_path"), AssetPathInput);

		bool bDryRun = false;
		RequireConfirmOrDryRun(Params, Messages, bDryRun);
		Row->SetBoolField(TEXT("dry_run"), bDryRun);

		FString AssetPath = FMonolithAssetUtils::ResolveAssetPath(AssetPathInput);
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
		if (!Asset)
		{
			AddMessage(Messages, TEXT("asset_not_found"), FString::Printf(TEXT("Asset not found at '%s'"), *AssetPath));
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		Row->SetStringField(TEXT("resolved_asset_path"), AssetPath);
		Row->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

		TArray<FString> SourceFilenames;
		const bool bCanReimport = FReimportManager::Instance()->CanReimport(Asset, &SourceFilenames);
		Row->SetBoolField(TEXT("can_reimport"), bCanReimport);
		Row->SetArrayField(TEXT("source_files"), StringArrayToJson(SourceFilenames));
		if (!bCanReimport)
		{
			AddMessage(Messages, TEXT("cannot_reimport"), TEXT("No registered reimport handler can reimport this asset."));
		}

		int32 SourceFileIndex = INDEX_NONE;
		if (Params.IsValid() && Params->HasTypedField<EJson::Number>(TEXT("source_file_index")))
		{
			SourceFileIndex = Params->GetIntegerField(TEXT("source_file_index"));
		}

		FString PreferredSource;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("source_file"), PreferredSource);
		}
		if (!PreferredSource.IsEmpty())
		{
			bool bAllowExternal = false;
			Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
			const FString NormalizedSource = NormalizeSourceFile(PreferredSource);
			if (!FPaths::FileExists(NormalizedSource))
			{
				AddMessage(Messages, TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
			}
			if (!IsUnderDefaultImportRoots(NormalizedSource) && !bAllowExternal)
			{
				AddMessage(Messages, TEXT("external_source_blocked"), TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
			}
			PreferredSource = NormalizedSource;
			Row->SetStringField(TEXT("preferred_source_file"), PreferredSource);
		}

		if (Messages.Num() > 0)
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}
		if (bDryRun)
		{
			Row->SetStringField(TEXT("status"), TEXT("would_reimport"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		const bool bForceNewFile = !PreferredSource.IsEmpty();
		const bool bSucceeded = FReimportManager::Instance()->Reimport(
			Asset,
			false,
			false,
			PreferredSource,
			nullptr,
			SourceFileIndex,
			bForceNewFile,
			true);

		Row->SetStringField(TEXT("status"), bSucceeded ? TEXT("reimported") : TEXT("error"));
		if (!bSucceeded)
		{
			AddMessage(Messages, TEXT("reimport_failed"), TEXT("Unreal reimport manager returned failure."));
		}
		TArray<UObject*> Objects;
		Objects.Add(Asset);
		Row->SetArrayField(TEXT("dirty_packages"), DirtyPackagesToJson(Objects));
		Row->SetArrayField(TEXT("messages"), Messages);
		return Row;
	}
}

void FMonolithInterchangeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("interchange"), TEXT("get_supported_formats"),
		TEXT("List Monolith Interchange import/export validation capabilities without mutating assets."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::GetSupportedFormats),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("can_import"),
		TEXT("Validate whether a source file can be handed to an Interchange import workflow."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::CanImport),
		FParamSchemaBuilder()
			.Required(TEXT("source_file"), TEXT("string"), TEXT("Source file to validate"))
			.Optional(TEXT("destination_path"), TEXT("string"), TEXT("Optional /Game destination package path"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("can_reimport"),
		TEXT("Check whether an existing asset has source import data usable for reimport."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::CanReimport),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("get_import_data"),
		TEXT("Read import source metadata from an existing asset without mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::GetImportData),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to inspect"))
			.Build());

	auto ImportSchema = FParamSchemaBuilder()
		.Required(TEXT("source_file"), TEXT("string"), TEXT("Source file to import"))
		.Required(TEXT("destination_path"), TEXT("string"), TEXT("Destination content folder such as /Game/Imported"))
		.Required(TEXT("conflict_policy"), TEXT("string"), TEXT("fail, overwrite, rename, or reimport_only"))
		.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
		.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without creating packages"), TEXT("false"))
		.Optional(TEXT("options"), TEXT("object"), TEXT("Forward-compatible pipeline options echoed in import_with_options responses"))
		.Build();

	Registry.RegisterAction(TEXT("interchange"), TEXT("import_asset"),
		TEXT("Import one source file with root, destination, conflict, confirmation, and dry-run guardrails."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);

	Registry.RegisterAction(TEXT("interchange"), TEXT("import_assets"),
		TEXT("Import multiple source files sequentially and return one result row per source."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAssets),
		FParamSchemaBuilder()
			.Required(TEXT("source_files"), TEXT("array"), TEXT("Source files to import"))
			.Required(TEXT("destination_path"), TEXT("string"), TEXT("Destination content folder such as /Game/Imported"))
			.Required(TEXT("conflict_policy"), TEXT("string"), TEXT("fail, overwrite, rename, or reimport_only"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without creating packages"), TEXT("false"))
			.Optional(TEXT("options"), TEXT("object"), TEXT("Forward-compatible pipeline options echoed in the response"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("import_scene"),
		TEXT("Typed scene import entrypoint over the guarded Interchange import implementation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_mesh"),
		TEXT("Typed mesh import entrypoint over the guarded Interchange import implementation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_skeletal_mesh"),
		TEXT("Typed skeletal mesh import entrypoint over the guarded Interchange import implementation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_texture"),
		TEXT("Typed texture import entrypoint over the guarded Interchange import implementation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_audio"),
		TEXT("Typed audio import entrypoint over the guarded Interchange import implementation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_with_options"),
		TEXT("Guarded import entrypoint that accepts a forward-compatible options object."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAsset),
		ImportSchema);

	Registry.RegisterAction(TEXT("interchange"), TEXT("update_reimport_path"),
		TEXT("Update an asset reimport source path after source/root validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::UpdateReimportPath),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to update"))
			.Required(TEXT("source_file"), TEXT("string"), TEXT("New source file path"))
			.Optional(TEXT("source_file_index"), TEXT("integer"), TEXT("Source file index to update"), TEXT("-1"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without updating import metadata"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("reimport_asset"),
		TEXT("Reimport one existing asset through Unreal's reimport manager."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ReimportAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to reimport"))
			.Optional(TEXT("source_file"), TEXT("string"), TEXT("Optional replacement source file"))
			.Optional(TEXT("source_file_index"), TEXT("integer"), TEXT("Source file index"), TEXT("-1"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without reimporting"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("reimport_assets"),
		TEXT("Reimport multiple assets sequentially and return one result row per asset."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ReimportAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Asset paths to reimport"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without reimporting"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("export_asset"),
		TEXT("Export one asset to a local file through UAssetExportTask after path validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ExportAsset),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to export"))
			.Required(TEXT("file_path"), TEXT("string"), TEXT("Output file path"))
			.Optional(TEXT("replace_existing"), TEXT("boolean"), TEXT("Overwrite an existing file"), TEXT("false"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow output outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without writing a file"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithInterchangeActions::GetSupportedFormats(const TSharedPtr<FJsonObject>&)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("mutation_actions_implemented"), true);
	Result->SetBoolField(TEXT("interchange_available"), IsInterchangeAvailable());
	Result->SetArrayField(TEXT("modules"), GetModuleStatusRows());
	Result->SetArrayField(TEXT("formats"), GetFormatRows());
	Result->SetArrayField(TEXT("default_allowed_roots"), GetAllowedRootRows());
	TArray<FString> ImplementedMutationActions = {
		TEXT("interchange.import_asset"),
		TEXT("interchange.import_assets"),
		TEXT("interchange.import_scene"),
		TEXT("interchange.import_mesh"),
		TEXT("interchange.import_skeletal_mesh"),
		TEXT("interchange.import_texture"),
		TEXT("interchange.import_audio"),
		TEXT("interchange.import_with_options"),
		TEXT("interchange.update_reimport_path"),
		TEXT("interchange.reimport_asset"),
		TEXT("interchange.reimport_assets"),
		TEXT("interchange.export_asset")
	};
	Result->SetArrayField(TEXT("implemented_mutation_actions"), StringArrayToJson(ImplementedMutationActions));
	Result->SetStringField(TEXT("policy"), TEXT("Import, reimport, and export mutations require confirm=true unless dry_run=true. File roots, destination packages, and conflict_policy are validated before writes."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::CanImport(const TSharedPtr<FJsonObject>& Params)
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
		? FPackageName::IsValidLongPackageName(DestinationPath, false) && IsGamePackagePath(DestinationPath)
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
		AddIssue(TEXT("invalid_destination_path"), TEXT("destination_path must be a valid /Game long package path such as /Game/Imported/MyAsset."));
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

FMonolithActionResult FMonolithInterchangeActions::CanReimport(const TSharedPtr<FJsonObject>& Params)
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

FMonolithActionResult FMonolithInterchangeActions::GetImportData(const TSharedPtr<FJsonObject>& Params)
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

FMonolithActionResult FMonolithInterchangeActions::ImportAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourceFile;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'source_file'"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(ImportOneSource(SourceFile, Params)));
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("row_count"), 1);
	Result->SetBoolField(TEXT("dry_run"), Params->HasTypedField<EJson::Boolean>(TEXT("dry_run")) && Params->GetBoolField(TEXT("dry_run")));
	if (Params->HasTypedField<EJson::Object>(TEXT("options")))
	{
		Result->SetObjectField(TEXT("options"), Params->GetObjectField(TEXT("options")));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::ImportAssets(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> SourceFiles;
	FString Error;
	if (!TryReadStringArray(Params, TEXT("source_files"), SourceFiles, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(SourceFiles.Num());
	for (const FString& SourceFile : SourceFiles)
	{
		Rows.Add(MakeShared<FJsonValueObject>(ImportOneSource(SourceFile, Params)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("row_count"), Rows.Num());
	Result->SetBoolField(TEXT("dry_run"), Params->HasTypedField<EJson::Boolean>(TEXT("dry_run")) && Params->GetBoolField(TEXT("dry_run")));
	if (Params->HasTypedField<EJson::Object>(TEXT("options")))
	{
		Result->SetObjectField(TEXT("options"), Params->GetObjectField(TEXT("options")));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::UpdateReimportPath(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	bool bDryRun = false;
	RequireConfirmOrDryRun(Params, Messages, bDryRun);

	FString SourceFile;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty())
	{
		AddMessage(Messages, TEXT("missing_source_file"), TEXT("Missing required param 'source_file'."));
	}

	bool bAllowExternal = false;
	Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
	const FString NormalizedSource = NormalizeSourceFile(SourceFile);
	if (!SourceFile.IsEmpty() && !FPaths::FileExists(NormalizedSource))
	{
		AddMessage(Messages, TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
	}
	if (!SourceFile.IsEmpty() && !IsUnderDefaultImportRoots(NormalizedSource) && !bAllowExternal)
	{
		AddMessage(Messages, TEXT("external_source_blocked"), TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
	}

	int32 SourceFileIndex = INDEX_NONE;
	if (Params.IsValid() && Params->HasTypedField<EJson::Number>(TEXT("source_file_index")))
	{
		SourceFileIndex = Params->GetIntegerField(TEXT("source_file_index"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("source_file"), SourceFile);
	Result->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
	Result->SetNumberField(TEXT("source_file_index"), SourceFileIndex);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);

	if (Messages.Num() > 0)
	{
		Result->SetStringField(TEXT("status"), TEXT("error"));
		Result->SetArrayField(TEXT("messages"), Messages);
		return FMonolithActionResult::Success(Result);
	}
	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("would_update_reimport_path"));
		Result->SetArrayField(TEXT("messages"), Messages);
		return FMonolithActionResult::Success(Result);
	}

	FReimportManager::Instance()->UpdateReimportPath(Asset, NormalizedSource, SourceFileIndex);
	Result->SetStringField(TEXT("status"), TEXT("updated_reimport_path"));
	Result->SetArrayField(TEXT("messages"), Messages);
	Result->SetArrayField(TEXT("source_files"), SourceFilesToJson(FindAssetImportData(Asset)));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::ReimportAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'asset_path'"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Add(MakeShared<FJsonValueObject>(ReimportOneAsset(AssetPath, Params)));
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("row_count"), 1);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::ReimportAssets(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> AssetPaths;
	FString Error;
	if (!TryReadStringArray(Params, TEXT("asset_paths"), AssetPaths, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(AssetPaths.Num());
	for (const FString& AssetPath : AssetPaths)
	{
		Rows.Add(MakeShared<FJsonValueObject>(ReimportOneAsset(AssetPath, Params)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("row_count"), Rows.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::ExportAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UObject* Asset = LoadAssetFromParams(Params, AssetPath, Error);
	if (!Asset)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Messages;
	bool bDryRun = false;
	RequireConfirmOrDryRun(Params, Messages, bDryRun);

	FString FilePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		AddMessage(Messages, TEXT("missing_file_path"), TEXT("Missing required param 'file_path'."));
	}
	if (!FilePath.IsEmpty() && FPaths::IsRelative(FilePath))
	{
		FilePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / FilePath);
	}
	const FString NormalizedFilePath = FPaths::ConvertRelativePathToFull(FilePath);

	bool bAllowExternal = false;
	Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
	bool bReplaceExisting = false;
	Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
	const bool bFileExists = !NormalizedFilePath.IsEmpty() && FPaths::FileExists(NormalizedFilePath);
	if (!NormalizedFilePath.IsEmpty() && !ValidateOutputFileRoot(NormalizedFilePath, bAllowExternal))
	{
		AddMessage(Messages, TEXT("external_output_blocked"), TEXT("Output path is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
	}
	if (bFileExists && !bReplaceExisting)
	{
		AddMessage(Messages, TEXT("output_exists"), TEXT("Output file already exists and replace_existing=false."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("file_path"), NormalizedFilePath);
	Result->SetBoolField(TEXT("file_exists"), bFileExists);
	Result->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);

	if (Messages.Num() > 0)
	{
		Result->SetStringField(TEXT("status"), TEXT("error"));
		Result->SetArrayField(TEXT("messages"), Messages);
		return FMonolithActionResult::Success(Result);
	}
	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("would_export"));
		Result->SetArrayField(TEXT("messages"), Messages);
		return FMonolithActionResult::Success(Result);
	}

	const FString Extension = FPaths::GetExtension(NormalizedFilePath, false);
	UExporter* Exporter = UExporter::FindExporter(Asset, *Extension);
	if (!Exporter)
	{
		Exporter = UExporter::FindExporter(Asset, TEXT(""));
	}
	if (!Exporter)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("No exporter found for asset %s (%s)"), *AssetPath, *Asset->GetClass()->GetName()));
	}

	const FString OutDir = FPaths::GetPath(NormalizedFilePath);
	if (!OutDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutDir))
	{
		IFileManager::Get().MakeDirectory(*OutDir, true);
	}

	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->AddToRoot();
	Task->Object = Asset;
	Task->Filename = NormalizedFilePath;
	Task->bSelected = false;
	Task->bReplaceIdentical = bReplaceExisting;
	Task->bPrompt = false;
	Task->bUseFileArchive = false;
	Task->bWriteEmptyFiles = false;
	Task->bAutomated = true;
	Task->Exporter = Exporter;
	const bool bSucceeded = UExporter::RunAssetExportTask(Task);
	Task->RemoveFromRoot();

	Result->SetStringField(TEXT("status"), bSucceeded ? TEXT("exported") : TEXT("error"));
	if (!bSucceeded)
	{
		AddMessage(Messages, TEXT("export_failed"), TEXT("Unreal exporter returned failure."));
	}
	Result->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*NormalizedFilePath)));
	Result->SetArrayField(TEXT("messages"), Messages);
	return FMonolithActionResult::Success(Result);
}
