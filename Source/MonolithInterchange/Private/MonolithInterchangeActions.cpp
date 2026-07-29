#include "MonolithInterchangeActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithInterchangeImportRollback.h"
#include "MonolithParamSchema.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetExportTask.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "EditorReimportHandler.h"
#include "EditorFramework/AssetImportData.h"
#include "Exporters/Exporter.h"
#include "Factories/Factory.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/SceneImportFactory.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "IAssetTools.h"
#include "InterchangeManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace
{
	enum class ERequestedImportKind : uint8
	{
		Any,
		Scene,
		StaticMesh,
		SkeletalMesh,
		Texture,
		Audio
	};

	struct FImportBackendAvailability
	{
		bool bInterchangeTranslator = false;
		UClass* LegacyFactoryClass = nullptr;

		bool IsAvailable() const
		{
			return bInterchangeTranslator || LegacyFactoryClass != nullptr;
		}
	};

	const TCHAR* ImportKindToString(ERequestedImportKind Kind)
	{
		switch (Kind)
		{
		case ERequestedImportKind::Scene:
			return TEXT("scene");
		case ERequestedImportKind::StaticMesh:
			return TEXT("static_mesh");
		case ERequestedImportKind::SkeletalMesh:
			return TEXT("skeletal_mesh");
		case ERequestedImportKind::Texture:
			return TEXT("texture");
		case ERequestedImportKind::Audio:
			return TEXT("audio");
		default:
			return TEXT("any");
		}
	}

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

	bool IsLexicallyUnderRoot(FString Path, FString Root)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Path);
		FPaths::NormalizeDirectoryName(Root);
#if PLATFORM_WINDOWS
		constexpr ESearchCase::Type PathCase = ESearchCase::IgnoreCase;
#else
		constexpr ESearchCase::Type PathCase = ESearchCase::CaseSensitive;
#endif
		return Path.Equals(Root, PathCase) || FPaths::IsUnderDirectory(Path, Root);
	}

	bool PathTraversesLinkBelowRoot(FString Path, FString Root)
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeFilename(Path);
		FPaths::NormalizeDirectoryName(Root);
		if (!IsLexicallyUnderRoot(Path, Root))
		{
			return false;
		}

		FString RelativePath = Path;
		FString RelativeBase = Root;
		if (!RelativeBase.EndsWith(TEXT("/")))
		{
			RelativeBase += TEXT("/");
		}
		if (!FPaths::MakePathRelativeTo(RelativePath, *RelativeBase))
		{
			return true;
		}

		FPaths::NormalizeFilename(RelativePath);
		TArray<FString> Components;
		RelativePath.ParseIntoArray(Components, TEXT("/"), true);
		FString CurrentPath = Root;
		for (const FString& Component : Components)
		{
			if (Component.IsEmpty() || Component == TEXT("."))
			{
				continue;
			}
			if (Component == TEXT(".."))
			{
				return true;
			}

			CurrentPath /= Component;
			if (FPlatformFileManager::Get().GetPlatformPhysical().IsSymlink(*CurrentPath) == ESymlinkResult::Symlink)
			{
				return true;
			}
		}
		return false;
	}

	bool IsUnderRootWithoutLinkTraversal(const FString& Path, const FString& Root)
	{
		return IsLexicallyUnderRoot(Path, Root) && !PathTraversesLinkBelowRoot(Path, Root);
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

	bool IsLexicallyUnderDefaultImportRoots(const FString& SourceFile)
	{
		return IsLexicallyUnderRoot(SourceFile, FPaths::ProjectDir()) ||
			IsLexicallyUnderRoot(SourceFile, FPaths::ProjectContentDir()) ||
			IsLexicallyUnderRoot(SourceFile, FPaths::ProjectSavedDir());
	}

	bool IsUnderDefaultImportRoots(const FString& SourceFile)
	{
		return IsUnderRootWithoutLinkTraversal(SourceFile, FPaths::ProjectDir()) ||
			IsUnderRootWithoutLinkTraversal(SourceFile, FPaths::ProjectContentDir()) ||
			IsUnderRootWithoutLinkTraversal(SourceFile, FPaths::ProjectSavedDir());
	}

	bool HasBlockedLinkTraversal(const FString& SourceFile)
	{
		return IsLexicallyUnderDefaultImportRoots(SourceFile) && !IsUnderDefaultImportRoots(SourceFile);
	}

	bool IsFormatCompatibleWithImportKind(const FInterchangeFormatDef* Format, ERequestedImportKind Kind)
	{
		if (!Format)
		{
			return false;
		}

		switch (Kind)
		{
		case ERequestedImportKind::Scene:
			return Format->bSceneCapable;
		case ERequestedImportKind::StaticMesh:
			return FString(Format->Category) == TEXT("mesh");
		case ERequestedImportKind::SkeletalMesh:
			return FString(Format->Extension) == TEXT("fbx");
		case ERequestedImportKind::Texture:
			return FString(Format->Category) == TEXT("texture");
		case ERequestedImportKind::Audio:
			return FString(Format->Category) == TEXT("audio");
		default:
			return true;
		}
	}

	bool IsFactoryClassCompatibleWithImportKind(UClass* FactoryClass, ERequestedImportKind Kind)
	{
		if (!FactoryClass)
		{
			return false;
		}

		switch (Kind)
		{
		case ERequestedImportKind::Scene:
			return FactoryClass->IsChildOf(USceneImportFactory::StaticClass());
		case ERequestedImportKind::StaticMesh:
		case ERequestedImportKind::SkeletalMesh:
			return FactoryClass->IsChildOf(UFbxFactory::StaticClass());
		default:
			return true;
		}
	}

	UClass* FindLegacyFactoryClass(const FString& SourceFile, ERequestedImportKind Kind)
	{
		UClass* BestFactoryClass = nullptr;
		int32 BestPriority = MIN_int32;
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* FactoryClass = *ClassIt;
			if (!FactoryClass ||
				!FactoryClass->IsChildOf(UFactory::StaticClass()) ||
				FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) ||
				!IsFactoryClassCompatibleWithImportKind(FactoryClass, Kind))
			{
				continue;
			}

			UFactory* Factory = Cast<UFactory>(FactoryClass->GetDefaultObject());
			if (!Factory || !Factory->bEditorImport || !Factory->FactoryCanImport(SourceFile))
			{
				continue;
			}

			if (!BestFactoryClass || Factory->ImportPriority > BestPriority)
			{
				BestFactoryClass = FactoryClass;
				BestPriority = Factory->ImportPriority;
			}
		}
		return BestFactoryClass;
	}

	bool CanUseInterchangeTranslator(const FString& SourceFile, ERequestedImportKind Kind)
	{
		if (Kind == ERequestedImportKind::Scene ||
			Kind == ERequestedImportKind::StaticMesh ||
			Kind == ERequestedImportKind::SkeletalMesh ||
			!UInterchangeManager::IsInterchangeImportEnabled())
		{
			return false;
		}

		UE::Interchange::FScopedSourceData ScopedSourceData(SourceFile);
		UInterchangeSourceData* SourceData = ScopedSourceData.GetSourceData();
		return SourceData &&
			UInterchangeManager::GetInterchangeManager().CanTranslateSourceData(SourceData, false);
	}

	FImportBackendAvailability GetImportBackendAvailability(const FString& SourceFile, ERequestedImportKind Kind)
	{
		FImportBackendAvailability Availability;
		Availability.bInterchangeTranslator = CanUseInterchangeTranslator(SourceFile, Kind);
		Availability.LegacyFactoryClass = FindLegacyFactoryClass(SourceFile, Kind);
		return Availability;
	}

	UFactory* CreateConfiguredFactory(UObject* Outer, UClass* FactoryClass, ERequestedImportKind Kind, const FString& Extension)
	{
		if (!Outer || !FactoryClass)
		{
			return nullptr;
		}

		if ((Kind == ERequestedImportKind::StaticMesh || Kind == ERequestedImportKind::SkeletalMesh) &&
			FactoryClass->IsChildOf(UFbxFactory::StaticClass()))
		{
			UFbxFactory* FbxFactory = NewObject<UFbxFactory>(Outer, FactoryClass);
			FbxFactory->ImportUI = NewObject<UFbxImportUI>(FbxFactory);
			FbxFactory->SetDetectImportTypeOnImport(false);
			FbxFactory->ImportUI->bAutomatedImportShouldDetectType = false;
			FbxFactory->ImportUI->bImportAsSkeletal = Kind == ERequestedImportKind::SkeletalMesh;
			FbxFactory->ImportUI->MeshTypeToImport =
				Kind == ERequestedImportKind::SkeletalMesh ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
			FbxFactory->ImportUI->bImportMesh = true;
			FbxFactory->ImportUI->bImportAnimations = false;
			FbxFactory->ImportUI->bIsObjImport = Extension == TEXT("obj");
			FbxFactory->ImportUI->bOverrideFullName = true;
			return FbxFactory;
		}

		return NewObject<UFactory>(Outer, FactoryClass);
	}

	bool ImportedObjectMatchesKind(UObject* ImportedObject, ERequestedImportKind Kind)
	{
		if (!ImportedObject)
		{
			return false;
		}

		switch (Kind)
		{
		case ERequestedImportKind::StaticMesh:
			return ImportedObject->IsA<UStaticMesh>();
		case ERequestedImportKind::SkeletalMesh:
			return ImportedObject->IsA<USkeletalMesh>();
		case ERequestedImportKind::Texture:
			return ImportedObject->IsA<UTexture>();
		case ERequestedImportKind::Audio:
			return ImportedObject->IsA<USoundWave>();
		default:
			return true;
		}
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
		if (!ImportData)
		{
			return {};
		}

		TArray<FString> Files;
		ImportData->ExtractFilenames(Files);
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Files.Num());
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

	TArray<TSharedPtr<FJsonValue>> SourceFilesToJson(const TArray<FString>& Files)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Files.Num());
		for (int32 Index = 0; Index < Files.Num(); ++Index)
		{
			const FString& File = Files[Index];
			const FString FullPath = NormalizeSourceFile(File);
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetStringField(TEXT("filename"), File);
			Row->SetStringField(TEXT("full_path"), FullPath);
			Row->SetStringField(TEXT("extension"), FPaths::GetExtension(File, false).ToLower());
			Row->SetBoolField(TEXT("exists"), FPaths::FileExists(FullPath));
			Row->SetBoolField(TEXT("under_default_roots"), IsUnderDefaultImportRoots(FullPath));
			Row->SetBoolField(TEXT("blocked_link_traversal"), HasBlockedLinkTraversal(FullPath));
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

	bool TryReadOptionalSourceFileIndex(
		const TSharedPtr<FJsonObject>& Params,
		int32& OutValue,
		TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		static const FString Field = TEXT("source_file_index");
		OutValue = INDEX_NONE;
		if (!Params.IsValid() || !Params->HasField(Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			AddMessage(
				Messages,
				TEXT("invalid_source_file_index"),
				FString::Printf(TEXT("%s must be an integer."), *Field));
			return false;
		}

		const double Number = Value->AsNumber();
		if (!FMath::IsFinite(Number) ||
			Number != FMath::TruncToDouble(Number) ||
			Number < static_cast<double>(MIN_int32) ||
			Number > static_cast<double>(MAX_int32))
		{
			AddMessage(
				Messages,
				TEXT("invalid_source_file_index"),
				FString::Printf(TEXT("%s must be a 32-bit integer."), *Field));
			return false;
		}

		OutValue = static_cast<int32>(Number);
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

	TSet<FName> SnapshotAssetObjectPathsUnder(const FString& DestinationPath)
	{
		TSet<FName> ObjectPaths;
		if (DestinationPath.IsEmpty())
		{
			return ObjectPaths;
		}

		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		TArray<FAssetData> AssetDataRows;
		AssetRegistryModule.Get().GetAssetsByPath(
			FName(*DestinationPath),
			AssetDataRows,
			true,
			false);
		for (const FAssetData& AssetData : AssetDataRows)
		{
			const FString ObjectPath = FString::Printf(
				TEXT("%s.%s"),
				*AssetData.PackageName.ToString(),
				*AssetData.AssetName.ToString());
			ObjectPaths.Add(FName(*ObjectPath));
		}

		const FString DestinationPrefix = DestinationPath + TEXT("/");
		for (TObjectIterator<UObject> ObjectIt; ObjectIt; ++ObjectIt)
		{
			UObject* Object = *ObjectIt;
			if (!Object || !Object->IsAsset() || !Object->GetOutermost())
			{
				continue;
			}

			const FString PackageName = Object->GetOutermost()->GetName();
			if (PackageName == DestinationPath || PackageName.StartsWith(DestinationPrefix))
			{
				ObjectPaths.Add(FName(*Object->GetPathName()));
			}
		}
		return ObjectPaths;
	}

	bool ValidateOutputFileRoot(const FString& FilePath, bool bAllowExternal)
	{
		return bAllowExternal || IsUnderDefaultImportRoots(FilePath);
	}

	TSharedPtr<FJsonObject> ImportOneSource(
		const FString& SourceFile,
		const TSharedPtr<FJsonObject>& Params,
		ERequestedImportKind RequestedKind = ERequestedImportKind::Any,
		TSet<FName>* ProspectivePackages = nullptr)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Messages;

		const FString NormalizedSource = NormalizeSourceFile(SourceFile);
		const FString Extension = FPaths::GetExtension(NormalizedSource, false).ToLower();
		const FInterchangeFormatDef* Format = FindFormatDef(Extension);
		Row->SetStringField(TEXT("source_file"), SourceFile);
		Row->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
		Row->SetStringField(TEXT("extension"), Extension);
		Row->SetStringField(TEXT("requested_import_kind"), ImportKindToString(RequestedKind));

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
			AddMessage(Messages, TEXT("missing_conflict_policy"), TEXT("Missing required param 'conflict_policy'. Use fail, overwrite, or rename."));
		}
		ConflictPolicy = ConflictPolicy.ToLower();
		Row->SetStringField(TEXT("conflict_policy"), ConflictPolicy);

		bool bAllowExternal = false;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
		}
		Row->SetBoolField(TEXT("allow_external"), bAllowExternal);

		bool bDryRun = false;
		RequireConfirmOrDryRun(Params, Messages, bDryRun);
		Row->SetBoolField(TEXT("dry_run"), bDryRun);

		const bool bFileExists = FPaths::FileExists(NormalizedSource);
		const bool bLexicallyUnderRoots = IsLexicallyUnderDefaultImportRoots(NormalizedSource);
		const bool bUnderRoots = IsUnderDefaultImportRoots(NormalizedSource);
		const bool bBlockedLinkTraversal = HasBlockedLinkTraversal(NormalizedSource);
		const bool bDestinationValid = !DestinationPath.IsEmpty() &&
			FPackageName::IsValidLongPackageName(DestinationPath, false) &&
			IsGamePackagePath(DestinationPath);
		const bool bFormatCompatible = IsFormatCompatibleWithImportKind(Format, RequestedKind);
		const FImportBackendAvailability Backend =
			bFileExists && bFormatCompatible
				? GetImportBackendAvailability(NormalizedSource, RequestedKind)
				: FImportBackendAvailability();
		const FString ExpectedAssetName = SanitizeAssetName(NormalizedSource);
		const FString ExpectedPackage = !DestinationPath.IsEmpty() ? JoinPackagePath(DestinationPath, ExpectedAssetName) : FString();
		const bool bReservedConflict =
			ProspectivePackages && ProspectivePackages->Contains(FName(*ExpectedPackage));
		const bool bLikelyConflict = !ExpectedPackage.IsEmpty() &&
			(FPackageName::DoesPackageExist(ExpectedPackage) ||
				FindPackage(nullptr, *ExpectedPackage) != nullptr ||
				bReservedConflict);
		FString ResolvedPackage = ExpectedPackage;
		FString ResolvedAssetName = ExpectedAssetName;
		if (ConflictPolicy == TEXT("rename") && bLikelyConflict)
		{
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
			AssetTools.CreateUniqueAssetName(ExpectedPackage, FString(), ResolvedPackage, ResolvedAssetName);
			for (int32 Suffix = 1;
				ProspectivePackages && ProspectivePackages->Contains(FName(*ResolvedPackage));
				++Suffix)
			{
				AssetTools.CreateUniqueAssetName(
					ExpectedPackage,
					FString::FromInt(Suffix),
					ResolvedPackage,
					ResolvedAssetName);
			}
		}

		Row->SetBoolField(TEXT("interchange_available"), IsInterchangeAvailable());
		Row->SetBoolField(TEXT("interchange_translator_available"), Backend.bInterchangeTranslator);
		Row->SetStringField(
			TEXT("legacy_factory_class"),
			Backend.LegacyFactoryClass ? Backend.LegacyFactoryClass->GetPathName() : FString());
		Row->SetBoolField(TEXT("source_exists"), bFileExists);
		Row->SetBoolField(TEXT("lexically_under_default_roots"), bLexicallyUnderRoots);
		Row->SetBoolField(TEXT("under_default_roots"), bUnderRoots);
		Row->SetBoolField(TEXT("blocked_link_traversal"), bBlockedLinkTraversal);
		Row->SetStringField(TEXT("expected_asset_name"), ExpectedAssetName);
		Row->SetStringField(TEXT("expected_package"), ExpectedPackage);
		Row->SetStringField(TEXT("resolved_asset_name"), ResolvedAssetName);
		Row->SetStringField(TEXT("resolved_package"), ResolvedPackage);
		Row->SetBoolField(TEXT("likely_package_conflict"), bLikelyConflict);

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
			AddMessage(
				Messages,
				bBlockedLinkTraversal ? TEXT("linked_source_blocked") : TEXT("external_source_blocked"),
				bBlockedLinkTraversal
					? TEXT("Source path traverses a symlink or junction below an allowed root. Use a direct path and allow_external=true only after caller-side policy allows it.")
					: TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
		}
		if (!bDestinationValid)
		{
			AddMessage(Messages, TEXT("invalid_destination_path"), TEXT("destination_path must be a valid /Game long package path such as /Game/Imported."));
		}
		if (ConflictPolicy != TEXT("fail") && ConflictPolicy != TEXT("overwrite") && ConflictPolicy != TEXT("rename"))
		{
			AddMessage(Messages, TEXT("invalid_conflict_policy"), TEXT("conflict_policy must be fail, overwrite, or rename."));
		}
		if (ConflictPolicy == TEXT("fail") && bLikelyConflict)
		{
			AddMessage(Messages, TEXT("destination_conflict"), FString::Printf(TEXT("Likely destination package already exists: %s"), *ExpectedPackage));
		}
		if (Format && !bFormatCompatible)
		{
			AddMessage(
				Messages,
				TEXT("typed_import_extension_mismatch"),
				FString::Printf(
					TEXT("Extension '%s' cannot satisfy requested import kind '%s'."),
					*Extension,
					ImportKindToString(RequestedKind)));
		}
		if (bFileExists && bFormatCompatible && !Backend.IsAvailable())
		{
			AddMessage(
				Messages,
				TEXT("importer_unavailable"),
				FString::Printf(
					TEXT("No registered Interchange translator or legacy factory can import '%s' as '%s'."),
					*Extension,
					ImportKindToString(RequestedKind)));
		}

		if (Messages.Num() > 0)
		{
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		if (bDryRun)
		{
			if (ProspectivePackages)
			{
				ProspectivePackages->Add(FName(*ResolvedPackage));
			}
			Row->SetStringField(TEXT("status"), TEXT("would_import"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		const bool bRequiresTypedResultValidation =
			RequestedKind != ERequestedImportKind::Any &&
			RequestedKind != ERequestedImportKind::Scene;
		const TSet<FName> PreExistingObjectPaths =
			bRequiresTypedResultValidation
				? SnapshotAssetObjectPathsUnder(DestinationPath)
				: TSet<FName>();

		UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
		ImportTask->AddToRoot();
		ImportTask->Filename = NormalizedSource;
		ImportTask->DestinationPath = DestinationPath;
		ImportTask->DestinationName = ResolvedAssetName;
		ImportTask->bAutomated = true;
		ImportTask->bAsync = false;
		ImportTask->bSave = false;
		ImportTask->bReplaceExisting = ConflictPolicy == TEXT("overwrite");
		ImportTask->bReplaceExistingSettings = ConflictPolicy == TEXT("overwrite");
		if (!Backend.bInterchangeTranslator && Backend.LegacyFactoryClass)
		{
			ImportTask->Factory = CreateConfiguredFactory(
				ImportTask,
				Backend.LegacyFactoryClass,
				RequestedKind,
				Extension);
			if (UFbxFactory* FbxFactory = Cast<UFbxFactory>(ImportTask->Factory))
			{
				ImportTask->Options = FbxFactory->ImportUI;
			}
			if (ImportTask->Factory)
			{
				ImportTask->Factory->SetAssetImportTask(ImportTask);
			}
		}

		TArray<UAssetImportTask*> ImportTasks;
		ImportTasks.Add(ImportTask);
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		AssetTools.ImportAssetTasks(ImportTasks);
		TArray<UObject*> ImportedObjects = ImportTask->GetObjects();
		ImportTask->RemoveFromRoot();
		if (ImportedObjects.Num() == 0)
		{
			AddMessage(Messages, TEXT("import_returned_no_objects"), TEXT("Unreal import returned no objects. Check file type, plugin availability, and import logs."));
			Row->SetStringField(TEXT("status"), TEXT("error"));
			Row->SetArrayField(TEXT("messages"), Messages);
			return Row;
		}

		TArray<TSharedPtr<FJsonValue>> ImportedRows;
		int32 MatchingKindCount = 0;
		for (UObject* Imported : ImportedObjects)
		{
			ImportedRows.Add(MakeShared<FJsonValueObject>(AssetToJson(Imported)));
			if (ImportedObjectMatchesKind(Imported, RequestedKind))
			{
				++MatchingKindCount;
			}
		}
		const TArray<TSharedPtr<FJsonValue>> DirtyPackagesBeforeValidation =
			DirtyPackagesToJson(ImportedObjects);
		if (bRequiresTypedResultValidation && MatchingKindCount == 0)
		{
			Row->SetArrayField(
				TEXT("dirty_packages_before_rollback"),
				DirtyPackagesBeforeValidation);
			AddMessage(
				Messages,
				TEXT("typed_import_result_mismatch"),
				FString::Printf(
					TEXT("Importer returned objects but none satisfied requested kind '%s'."),
					ImportKindToString(RequestedKind)));

			const FMonolithInterchangeRollbackResult Rollback =
				RollbackNewImportedObjects(ImportedObjects, PreExistingObjectPaths);
			Row->SetBoolField(TEXT("rollback_attempted"), true);
			Row->SetBoolField(TEXT("rollback_complete"), Rollback.IsComplete());
			Row->SetNumberField(TEXT("rollback_candidate_count"), Rollback.CandidateCount);
			Row->SetNumberField(TEXT("rollback_deleted_count"), Rollback.DeletedCount);
			Row->SetArrayField(
				TEXT("rollback_candidates"),
				StringArrayToJson(Rollback.CandidateObjectPaths));
			Row->SetArrayField(TEXT("rolled_back_assets"), StringArrayToJson(Rollback.DeletedObjectPaths));
			Row->SetArrayField(
				TEXT("pre_existing_import_results"),
				StringArrayToJson(Rollback.PreExistingObjectPaths));
			Row->SetArrayField(
				TEXT("unmanaged_import_results"),
				StringArrayToJson(Rollback.UnmanagedObjectPaths));

			if (Rollback.IsComplete())
			{
				AddMessage(
					Messages,
					TEXT("typed_import_rollback_complete"),
					TEXT("All newly imported assets were removed after typed result validation failed."));
				Row->SetBoolField(TEXT("partial_mutation"), false);
				Row->SetStringField(TEXT("status"), TEXT("error"));
				Row->SetArrayField(
					TEXT("dirty_packages"),
					TArray<TSharedPtr<FJsonValue>>());
			}
			else
			{
				AddMessage(
					Messages,
					TEXT("typed_import_partial_mutation"),
					TEXT("Typed result validation failed after Unreal returned pre-existing, unmanaged, or undeletable objects. The response is a partial mutation; inspect the reported assets before retrying."));
				Row->SetBoolField(TEXT("partial_mutation"), true);
				Row->SetStringField(TEXT("status"), TEXT("partial_import"));
				Row->SetArrayField(TEXT("dirty_packages"), DirtyPackagesBeforeValidation);
			}
		}
		else
		{
			Row->SetStringField(TEXT("status"), TEXT("imported"));
			Row->SetArrayField(TEXT("dirty_packages"), DirtyPackagesBeforeValidation);
		}
		Row->SetNumberField(TEXT("matching_kind_count"), MatchingKindCount);
		Row->SetArrayField(TEXT("imported_assets"), ImportedRows);
		Row->SetArrayField(TEXT("messages"), Messages);
		return Row;
	}

	FMonolithActionResult ImportSingleSource(
		const TSharedPtr<FJsonObject>& Params,
		ERequestedImportKind RequestedKind)
	{
		FString SourceFile;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("source_file"), SourceFile) || SourceFile.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("Missing required param 'source_file'"));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Add(MakeShared<FJsonValueObject>(ImportOneSource(SourceFile, Params, RequestedKind)));
		Result->SetArrayField(TEXT("rows"), Rows);
		Result->SetNumberField(TEXT("row_count"), 1);
		Result->SetStringField(TEXT("requested_import_kind"), ImportKindToString(RequestedKind));
		Result->SetBoolField(
			TEXT("dry_run"),
			Params->HasTypedField<EJson::Boolean>(TEXT("dry_run")) &&
			Params->GetBoolField(TEXT("dry_run")));
		return FMonolithActionResult::Success(Result);
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

		bool bAllowExternal = false;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("allow_external"), bAllowExternal);
		}
		Row->SetBoolField(TEXT("allow_external"), bAllowExternal);

		int32 SourceFileIndex = INDEX_NONE;
		TryReadOptionalSourceFileIndex(Params, SourceFileIndex, Messages);
		Row->SetNumberField(TEXT("source_file_index"), SourceFileIndex);
		if (SourceFileIndex != INDEX_NONE && !SourceFilenames.IsValidIndex(SourceFileIndex))
		{
			AddMessage(
				Messages,
				TEXT("invalid_source_file_index"),
				FString::Printf(
					TEXT("source_file_index %d is outside the available range 0..%d."),
					SourceFileIndex,
					SourceFilenames.Num() - 1));
		}

		FString PreferredSource;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("source_file"), PreferredSource);
		}
		const bool bReplacesAllSources = !PreferredSource.IsEmpty() && SourceFileIndex == INDEX_NONE;

		TArray<TSharedPtr<FJsonValue>> ValidatedSourceRows;
		for (int32 Index = 0; Index < SourceFilenames.Num(); ++Index)
		{
			const bool bSourceWillBeReplaced =
				bReplacesAllSources ||
				(!PreferredSource.IsEmpty() && SourceFileIndex == Index);
			const FString NormalizedStoredSource = NormalizeSourceFile(SourceFilenames[Index]);
			const bool bStoredSourceExists = FPaths::FileExists(NormalizedStoredSource);
			const bool bStoredSourceUnderRoots = IsUnderDefaultImportRoots(NormalizedStoredSource);
			const bool bStoredSourceBlockedLink = HasBlockedLinkTraversal(NormalizedStoredSource);

			TSharedPtr<FJsonObject> SourceRow = MakeShared<FJsonObject>();
			SourceRow->SetNumberField(TEXT("index"), Index);
			SourceRow->SetStringField(TEXT("filename"), SourceFilenames[Index]);
			SourceRow->SetStringField(TEXT("normalized_filename"), NormalizedStoredSource);
			SourceRow->SetBoolField(TEXT("exists"), bStoredSourceExists);
			SourceRow->SetBoolField(TEXT("under_default_roots"), bStoredSourceUnderRoots);
			SourceRow->SetBoolField(TEXT("blocked_link_traversal"), bStoredSourceBlockedLink);
			SourceRow->SetBoolField(TEXT("will_be_replaced"), bSourceWillBeReplaced);
			ValidatedSourceRows.Add(MakeShared<FJsonValueObject>(SourceRow));

			if (!bSourceWillBeReplaced && !bStoredSourceExists)
			{
				AddMessage(
					Messages,
					TEXT("source_missing"),
					FString::Printf(TEXT("Stored reimport source does not exist: %s"), *NormalizedStoredSource));
			}
			if (!bSourceWillBeReplaced && !bStoredSourceUnderRoots && !bAllowExternal)
			{
				AddMessage(
					Messages,
					bStoredSourceBlockedLink ? TEXT("linked_source_blocked") : TEXT("external_source_blocked"),
					bStoredSourceBlockedLink
						? FString::Printf(
							TEXT("Stored reimport source traverses a symlink or junction below an allowed root: %s"),
							*NormalizedStoredSource)
						: FString::Printf(
							TEXT("Stored reimport source is outside project/content/saved roots: %s"),
							*NormalizedStoredSource));
			}
		}
		Row->SetArrayField(TEXT("validated_source_files"), ValidatedSourceRows);

		if (!PreferredSource.IsEmpty())
		{
			const FString NormalizedSource = NormalizeSourceFile(PreferredSource);
			if (!FPaths::FileExists(NormalizedSource))
			{
				AddMessage(Messages, TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
			}
			if (!IsUnderDefaultImportRoots(NormalizedSource) && !bAllowExternal)
			{
				const bool bBlockedLinkTraversal = HasBlockedLinkTraversal(NormalizedSource);
				AddMessage(
					Messages,
					bBlockedLinkTraversal ? TEXT("linked_source_blocked") : TEXT("external_source_blocked"),
					bBlockedLinkTraversal
						? TEXT("Replacement source traverses a symlink or junction below an allowed root.")
						: TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
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
		TEXT("Validate whether a registered Interchange translator or legacy factory can import a source file."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::CanImport),
		FParamSchemaBuilder()
			.Required(TEXT("source_file"), TEXT("string"), TEXT("Source file to validate"))
			.Optional(TEXT("destination_path"), TEXT("string"), TEXT("Optional /Game destination package path"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("can_reimport"),
		TEXT("Check whether Unreal's reimport manager has a handler for an existing asset."),
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
		.Required(TEXT("conflict_policy"), TEXT("string"), TEXT("fail, overwrite, or rename"))
		.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
		.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
		.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without creating packages"), TEXT("false"))
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
			.Required(TEXT("conflict_policy"), TEXT("string"), TEXT("fail, overwrite, or rename"))
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow source files outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without creating packages"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("import_scene"),
		TEXT("Import only a scene-capable source through a registered scene importer."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportScene),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_mesh"),
		TEXT("Import a static mesh with an explicitly configured mesh factory."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportMesh),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_skeletal_mesh"),
		TEXT("Import an FBX skeletal mesh with skeletal detection fixed before import."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportSkeletalMesh),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_texture"),
		TEXT("Import only a texture source through a registered texture importer."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportTexture),
		ImportSchema);
	Registry.RegisterAction(TEXT("interchange"), TEXT("import_audio"),
		TEXT("Import only a wave-audio source through a registered audio importer."),
		FMonolithActionHandler::CreateStatic(&FMonolithInterchangeActions::ImportAudio),
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
			.Optional(TEXT("allow_external"), TEXT("boolean"), TEXT("Allow stored source files outside project/content/saved roots"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for mutation unless dry_run=true"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate without reimporting"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("interchange"), TEXT("export_asset"),
		TEXT("Export one asset through a matching UExporter after output-path validation."),
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
		TEXT("interchange.update_reimport_path"),
		TEXT("interchange.reimport_asset"),
		TEXT("interchange.reimport_assets"),
		TEXT("interchange.export_asset")
	};
	Result->SetArrayField(TEXT("implemented_mutation_actions"), StringArrayToJson(ImplementedMutationActions));
	Result->SetStringField(TEXT("policy"), TEXT("Import, reimport, and export mutations require confirm=true unless dry_run=true. Concrete backends, link-safe file roots, destination packages, and conflict_policy are validated before writes."));
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
	DestinationPath = NormalizePackageFolder(DestinationPath);

	const FString NormalizedSource = NormalizeSourceFile(SourceFile);
	const FString Extension = FPaths::GetExtension(NormalizedSource, false).ToLower();
	const FInterchangeFormatDef* Format = FindFormatDef(Extension);
	const bool bFileExists = FPaths::FileExists(NormalizedSource);
	const bool bLexicallyUnderRoots = IsLexicallyUnderDefaultImportRoots(NormalizedSource);
	const bool bUnderRoots = IsUnderDefaultImportRoots(NormalizedSource);
	const bool bBlockedLinkTraversal = HasBlockedLinkTraversal(NormalizedSource);
	const bool bDestinationValid = !DestinationPath.IsEmpty()
		? FPackageName::IsValidLongPackageName(DestinationPath, false) && IsGamePackagePath(DestinationPath)
		: true;
	const FImportBackendAvailability Backend =
		bFileExists && Format
			? GetImportBackendAvailability(NormalizedSource, ERequestedImportKind::Any)
			: FImportBackendAvailability();
	const bool bCanImport =
		bFileExists &&
		Format != nullptr &&
		Backend.IsAvailable() &&
		(bAllowExternal || bUnderRoots) &&
		bDestinationValid;

	TArray<TSharedPtr<FJsonValue>> Issues;
	auto AddIssue = [&Issues](const FString& Code, const FString& Message)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

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
		AddIssue(
			bBlockedLinkTraversal ? TEXT("linked_source_blocked") : TEXT("external_source_blocked"),
			bBlockedLinkTraversal
				? TEXT("Source path traverses a symlink or junction below an allowed root. Use a direct path and allow_external=true only after caller-side policy allows it.")
				: TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
	}
	if (!bDestinationValid)
	{
		AddIssue(TEXT("invalid_destination_path"), TEXT("destination_path must be a valid /Game content folder such as /Game/Imported."));
	}
	if (bFileExists && Format && !Backend.IsAvailable())
	{
		AddIssue(
			TEXT("importer_unavailable"),
			FString::Printf(
				TEXT("No registered Interchange translator or legacy factory can import extension '%s'."),
				*Extension));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("can_import"), bCanImport);
	Result->SetBoolField(TEXT("interchange_available"), IsInterchangeAvailable());
	Result->SetBoolField(TEXT("interchange_translator_available"), Backend.bInterchangeTranslator);
	Result->SetStringField(
		TEXT("legacy_factory_class"),
		Backend.LegacyFactoryClass ? Backend.LegacyFactoryClass->GetPathName() : FString());
	Result->SetStringField(TEXT("source_file"), SourceFile);
	Result->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
	Result->SetStringField(TEXT("extension"), Extension);
	Result->SetBoolField(TEXT("source_exists"), bFileExists);
	Result->SetBoolField(TEXT("lexically_under_default_roots"), bLexicallyUnderRoots);
	Result->SetBoolField(TEXT("under_default_roots"), bUnderRoots);
	Result->SetBoolField(TEXT("blocked_link_traversal"), bBlockedLinkTraversal);
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

	TArray<FString> HandlerSourceFiles;
	const bool bHandlerCanReimport =
		FReimportManager::Instance()->CanReimport(Asset, &HandlerSourceFiles);
	const UAssetImportData* ImportData = FindAssetImportData(Asset);
	TArray<TSharedPtr<FJsonValue>> SourceFiles = SourceFilesToJson(HandlerSourceFiles);
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
	Result->SetBoolField(TEXT("can_reimport"), bHandlerCanReimport);
	Result->SetBoolField(TEXT("handler_available"), bHandlerCanReimport);
	Result->SetBoolField(TEXT("any_source_exists"), bAnyExistingSource);
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
	return ImportSingleSource(Params, ERequestedImportKind::Any);
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
	TSet<FName> ProspectivePackages;
	const bool bDryRun =
		Params.IsValid() &&
		Params->HasTypedField<EJson::Boolean>(TEXT("dry_run")) &&
		Params->GetBoolField(TEXT("dry_run"));
	for (const FString& SourceFile : SourceFiles)
	{
		Rows.Add(MakeShared<FJsonValueObject>(
			ImportOneSource(
				SourceFile,
				Params,
				ERequestedImportKind::Any,
				bDryRun ? &ProspectivePackages : nullptr)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("rows"), Rows);
	Result->SetNumberField(TEXT("row_count"), Rows.Num());
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithInterchangeActions::ImportScene(const TSharedPtr<FJsonObject>& Params)
{
	return ImportSingleSource(Params, ERequestedImportKind::Scene);
}

FMonolithActionResult FMonolithInterchangeActions::ImportMesh(const TSharedPtr<FJsonObject>& Params)
{
	return ImportSingleSource(Params, ERequestedImportKind::StaticMesh);
}

FMonolithActionResult FMonolithInterchangeActions::ImportSkeletalMesh(const TSharedPtr<FJsonObject>& Params)
{
	return ImportSingleSource(Params, ERequestedImportKind::SkeletalMesh);
}

FMonolithActionResult FMonolithInterchangeActions::ImportTexture(const TSharedPtr<FJsonObject>& Params)
{
	return ImportSingleSource(Params, ERequestedImportKind::Texture);
}

FMonolithActionResult FMonolithInterchangeActions::ImportAudio(const TSharedPtr<FJsonObject>& Params)
{
	return ImportSingleSource(Params, ERequestedImportKind::Audio);
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
	const bool bSourceExists = !NormalizedSource.IsEmpty() && FPaths::FileExists(NormalizedSource);
	const bool bUnderRoots = !NormalizedSource.IsEmpty() && IsUnderDefaultImportRoots(NormalizedSource);
	const bool bBlockedLinkTraversal = !NormalizedSource.IsEmpty() && HasBlockedLinkTraversal(NormalizedSource);
	if (!SourceFile.IsEmpty() && !FPaths::FileExists(NormalizedSource))
	{
		AddMessage(Messages, TEXT("source_missing"), FString::Printf(TEXT("Source file does not exist: %s"), *NormalizedSource));
	}
	if (!SourceFile.IsEmpty() && !bUnderRoots && !bAllowExternal)
	{
		AddMessage(
			Messages,
			bBlockedLinkTraversal ? TEXT("linked_source_blocked") : TEXT("external_source_blocked"),
			bBlockedLinkTraversal
				? TEXT("Source path traverses a symlink or junction below an allowed root. Use a direct path and allow_external=true only after caller-side policy allows it.")
				: TEXT("Source file is outside project/content/saved roots. Pass allow_external=true only after caller-side policy allows it."));
	}

	int32 SourceFileIndex = INDEX_NONE;
	TryReadOptionalSourceFileIndex(Params, SourceFileIndex, Messages);
	if (SourceFileIndex < INDEX_NONE)
	{
		AddMessage(Messages, TEXT("invalid_source_file_index"), TEXT("source_file_index must be -1 or a zero-based source index."));
	}

	TArray<FString> ExistingSourceFiles;
	const bool bCanReimport =
		FReimportManager::Instance()->CanReimport(Asset, &ExistingSourceFiles);
	if (!bCanReimport)
	{
		AddMessage(Messages, TEXT("cannot_reimport"), TEXT("No registered reimport handler can update this asset's source path."));
	}
	if (SourceFileIndex >= 0 && !ExistingSourceFiles.IsValidIndex(SourceFileIndex))
	{
		AddMessage(
			Messages,
			TEXT("invalid_source_file_index"),
			FString::Printf(
				TEXT("source_file_index %d is outside the available range 0..%d."),
				SourceFileIndex,
				ExistingSourceFiles.Num() - 1));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("source_file"), SourceFile);
	Result->SetStringField(TEXT("normalized_source_file"), NormalizedSource);
	Result->SetNumberField(TEXT("source_file_index"), SourceFileIndex);
	Result->SetBoolField(TEXT("source_exists"), bSourceExists);
	Result->SetBoolField(TEXT("under_default_roots"), bUnderRoots);
	Result->SetBoolField(TEXT("blocked_link_traversal"), bBlockedLinkTraversal);
	Result->SetBoolField(TEXT("allow_external"), bAllowExternal);
	Result->SetBoolField(TEXT("can_reimport"), bCanReimport);
	Result->SetArrayField(TEXT("previous_source_files"), SourceFilesToJson(ExistingSourceFiles));
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
	TArray<FString> UpdatedSourceFiles;
	const bool bCanReimportAfterUpdate =
		FReimportManager::Instance()->CanReimport(Asset, &UpdatedSourceFiles);
	const int32 ExpectedIndex = SourceFileIndex == INDEX_NONE ? 0 : SourceFileIndex;
	bool bReadbackMatches = bCanReimportAfterUpdate && UpdatedSourceFiles.IsValidIndex(ExpectedIndex);
	if (bReadbackMatches)
	{
		const FString NormalizedReadback = NormalizeSourceFile(UpdatedSourceFiles[ExpectedIndex]);
#if PLATFORM_WINDOWS
		bReadbackMatches = NormalizedReadback.Equals(NormalizedSource, ESearchCase::IgnoreCase);
#else
		bReadbackMatches = NormalizedReadback.Equals(NormalizedSource, ESearchCase::CaseSensitive);
#endif
	}

	Result->SetBoolField(TEXT("can_reimport_after_update"), bCanReimportAfterUpdate);
	Result->SetBoolField(TEXT("readback_matches"), bReadbackMatches);
	Result->SetStringField(
		TEXT("status"),
		bReadbackMatches ? TEXT("updated_reimport_path") : TEXT("error"));
	if (!bReadbackMatches)
	{
		AddMessage(
			Messages,
			TEXT("reimport_path_readback_mismatch"),
			TEXT("The registered reimport handler did not report the requested source path after UpdateReimportPath."));
	}
	Result->SetArrayField(TEXT("messages"), Messages);
	Result->SetArrayField(TEXT("source_files"), SourceFilesToJson(UpdatedSourceFiles));
	TArray<UObject*> Objects;
	Objects.Add(Asset);
	Result->SetArrayField(TEXT("dirty_packages"), DirtyPackagesToJson(Objects));
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
	const FString Extension = FPaths::GetExtension(NormalizedFilePath, false);
	UExporter* Exporter = Extension.IsEmpty()
		? UExporter::FindExporter(Asset, TEXT(""))
		: UExporter::FindExporter(Asset, *Extension);
	if (!Exporter)
	{
		AddMessage(
			Messages,
			TEXT("exporter_unavailable"),
			FString::Printf(
				TEXT("No exporter can write asset %s (%s) to extension '%s'."),
				*AssetPath,
				*Asset->GetClass()->GetName(),
				*Extension));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
	Result->SetStringField(TEXT("file_path"), NormalizedFilePath);
	Result->SetBoolField(TEXT("file_exists"), bFileExists);
	Result->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("exporter_available"), Exporter != nullptr);
	Result->SetStringField(TEXT("exporter_class"), Exporter ? Exporter->GetClass()->GetPathName() : FString());

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
