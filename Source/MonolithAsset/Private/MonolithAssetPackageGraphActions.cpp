#include "MonolithAssetPackageGraphActions.h"
#include "MonolithAssetMoveActions.h"
#include "MonolithAssetResultCompat.h"

#include "MonolithParamSchema.h"

#include "AssetToolsModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IAssetTools.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectRedirector.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	static constexpr int32 ErrInvalidParams = -32602;
	static constexpr int32 ErrInternal = -32603;

	struct FRootRemap
	{
		FString SourceRoot;
		FString DestinationRoot;
	};

	struct FMutationOptions
	{
		bool bDryRun = false;
		bool bConfirm = false;
		bool bSave = true;
		bool bStrict = true;
	};

	struct FPackageCopyRow
	{
		FString SourcePackage;
		FString DestinationPackage;
		bool bSourceExists = false;
		bool bDestinationExists = false;
		FAssetData SourceAsset;
		FString SelectedStrategy = TEXT("duplicate_asset");
	};

	struct FContentMountSpec
	{
		FString MountPoint;
		FString ContentDir;
		FString PluginName;
		FString RelativePluginDir;
		FString ResolutionSource;
		bool bExistingSame = false;
		bool bExistingDifferent = false;
		bool bDirectoryExists = false;
	};

	struct FReferenceFixupOptions
	{
		FMutationOptions Mutation;
		int32 MaxPackages = 1000;
		bool bRequireTargets = true;
	};

	struct FReferenceFixupStats
	{
		int32 CheckedPackageCount = 0;
		int32 CheckedObjectCount = 0;
		int32 CandidateCount = 0;
		int32 AppliedCount = 0;
		bool bTruncated = false;
		bool bHasBlockingErrors = false;
		TSet<FString> ChangedPackages;
		TArray<TSharedPtr<FJsonValue>> References;
		TArray<TSharedPtr<FJsonValue>> Warnings;
	};

	enum class EDependencyKind : uint8
	{
		Hard,
		Soft
	};

	static TSharedPtr<FJsonObject> ErrorData(const FString& Field, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("field"), Field);
		Data->SetStringField(TEXT("detail"), Detail);
		return Data;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(Value));
		}
		return Rows;
	}

	static FString NormalizeRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Root.EndsWith(TEXT("/")) && Root.Len() > 1)
		{
			Root.LeftChopInline(1);
		}
		return Root;
	}

	static FString NormalizePackagePath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Path.Contains(TEXT(".")))
		{
			Path = FPackageName::ObjectPathToPackageName(Path);
		}
		if (Path.EndsWith(TEXT("_C")))
		{
			Path.LeftChopInline(2);
		}
		while (Path.EndsWith(TEXT("/")) && Path.Len() > 1)
		{
			Path.LeftChopInline(1);
		}
		return Path;
	}

	static bool IsValidPackageOrRoot(const FString& Path)
	{
		return Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("//")) && Path.Len() > 1;
	}

	static bool IsUnderRoot(const FString& PackagePath, const FString& Root)
	{
		return PackagePath.Equals(Root, ESearchCase::IgnoreCase)
			|| PackagePath.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static bool IsUnderAnyRoot(const FString& PackagePath, const TArray<FString>& Roots)
	{
		for (const FString& Root : Roots)
		{
			if (IsUnderRoot(PackagePath, Root))
			{
				return true;
			}
		}
		return false;
	}

	static bool ReadStringArrayParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		bool bRequired,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			if (bRequired)
			{
				OutError = FString::Printf(TEXT("Missing required array param '%s'"), FieldName);
				return false;
			}
			return true;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName);
				return false;
			}
			StringValue = NormalizePackagePath(StringValue);
			if (!StringValue.IsEmpty())
			{
				OutValues.AddUnique(StringValue);
			}
		}

		if (bRequired && OutValues.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Param '%s' must contain at least one path"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadBoolParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->HasTypedField<EJson::Boolean>(FieldName)
			|| !Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName);
			return false;
		}
		return true;
	}

	static bool ReadStringParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetStringField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a string"), FieldName);
			return false;
		}
		InOutValue.TrimStartAndEndInline();
		return true;
	}

	static bool ReadMutationOptions(
		const TSharedPtr<FJsonObject>& Params,
		FMutationOptions& InOutOptions,
		FString& OutError)
	{
		if (!ReadBoolParam(Params, TEXT("dry_run"), InOutOptions.bDryRun, OutError)
			|| !ReadBoolParam(Params, TEXT("confirm"), InOutOptions.bConfirm, OutError)
			|| !ReadBoolParam(Params, TEXT("save"), InOutOptions.bSave, OutError)
			|| !ReadBoolParam(Params, TEXT("strict"), InOutOptions.bStrict, OutError))
		{
			return false;
		}
		if (!InOutOptions.bDryRun && !InOutOptions.bConfirm)
		{
			OutError = TEXT("Mutating package graph actions require dry_run=true or confirm=true");
			return false;
		}
		return true;
	}

	static bool ReadIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("Param '%s' must be a number"), FieldName);
			return false;
		}
		InOutValue = FMath::Clamp(static_cast<int32>(Number), 1, 10000);
		return true;
	}

	static FString NormalizeMountPoint(FString MountPoint)
	{
		MountPoint.TrimStartAndEndInline();
		MountPoint.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (MountPoint.Contains(TEXT("//")))
		{
			MountPoint.ReplaceInline(TEXT("//"), TEXT("/"));
		}
		if (!MountPoint.EndsWith(TEXT("/")))
		{
			MountPoint += TEXT("/");
		}
		return MountPoint;
	}

	static bool ValidateMountPoint(const FString& MountPoint, bool bAllowCoreMountPoints, FString& OutError)
	{
		if (!MountPoint.StartsWith(TEXT("/")) || !MountPoint.EndsWith(TEXT("/")) || MountPoint.Len() <= 2)
		{
			OutError = FString::Printf(TEXT("Mount point must use rooted package syntax with leading and trailing slashes, e.g. /ShooterMaps/: %s"), *MountPoint);
			return false;
		}
		if (MountPoint.Contains(TEXT("//")))
		{
			OutError = FString::Printf(TEXT("Mount point must not contain duplicate slashes: %s"), *MountPoint);
			return false;
		}
		if (!bAllowCoreMountPoints
			&& (MountPoint.Equals(TEXT("/Game/"), ESearchCase::IgnoreCase)
				|| MountPoint.Equals(TEXT("/Engine/"), ESearchCase::IgnoreCase)
				|| MountPoint.Equals(TEXT("/Script/"), ESearchCase::IgnoreCase)))
		{
			OutError = FString::Printf(TEXT("Refusing to override core mount point %s; pass allow_core_mount_points=true only for explicit diagnostics"), *MountPoint);
			return false;
		}
		return true;
	}

	static FString NormalizeContentDir(FString ContentDir)
	{
		ContentDir.TrimStartAndEndInline();
		ContentDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (FPaths::IsRelative(ContentDir))
		{
			ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), ContentDir);
		}
		else
		{
			ContentDir = FPaths::ConvertRelativePathToFull(ContentDir);
		}
		FPaths::NormalizeDirectoryName(ContentDir);
		if (!ContentDir.EndsWith(TEXT("/")))
		{
			ContentDir += TEXT("/");
		}
		return ContentDir;
	}

	static bool NormalizeRelativePluginDir(FString& InOutRelativePluginDir, FString& OutError)
	{
		InOutRelativePluginDir.TrimStartAndEndInline();
		InOutRelativePluginDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (InOutRelativePluginDir.IsEmpty())
		{
			OutError = TEXT("project_plugin_dir must not be empty");
			return false;
		}
		if (!FPaths::IsRelative(InOutRelativePluginDir) || InOutRelativePluginDir.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("project_plugin_dir must be relative to the project Plugins directory: %s"), *InOutRelativePluginDir);
			return false;
		}

		TArray<FString> Segments;
		InOutRelativePluginDir.ParseIntoArray(Segments, TEXT("/"), true);
		for (const FString& Segment : Segments)
		{
			if (Segment == TEXT(".") || Segment == TEXT(".."))
			{
				OutError = FString::Printf(TEXT("project_plugin_dir must not contain '.' or '..' path segments: %s"), *InOutRelativePluginDir);
				return false;
			}
		}
		while (InOutRelativePluginDir.EndsWith(TEXT("/")))
		{
			InOutRelativePluginDir.LeftChopInline(1);
		}
		return true;
	}

	static FString ResolveRelativePluginContentDir(FString RelativePluginDir)
	{
		RelativePluginDir.TrimStartAndEndInline();
		RelativePluginDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (RelativePluginDir.StartsWith(TEXT("/")))
		{
			RelativePluginDir.RightChopInline(1);
		}
		while (RelativePluginDir.EndsWith(TEXT("/")))
		{
			RelativePluginDir.LeftChopInline(1);
		}
		const FString PluginDir = RelativePluginDir.EndsWith(TEXT("/Content"), ESearchCase::IgnoreCase)
			? RelativePluginDir
			: RelativePluginDir / TEXT("Content");
		return NormalizeContentDir(FPaths::ProjectPluginsDir() / PluginDir);
	}

	static bool ResolveContentMountSpec(
		const TSharedPtr<FJsonObject>& Object,
		bool bAllowCoreMountPoints,
		FContentMountSpec& OutSpec,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			OutError = TEXT("Each mount_points entry must be an object");
			return false;
		}

		Object->TryGetStringField(TEXT("plugin_name"), OutSpec.PluginName);
		Object->TryGetStringField(TEXT("relative_plugin_dir"), OutSpec.RelativePluginDir);
		Object->TryGetStringField(TEXT("mount_point"), OutSpec.MountPoint);
		if (OutSpec.MountPoint.IsEmpty())
		{
			Object->TryGetStringField(TEXT("root"), OutSpec.MountPoint);
		}
		FString ProjectPluginDir;
		Object->TryGetStringField(TEXT("project_plugin_dir"), ProjectPluginDir);
		if (!ProjectPluginDir.IsEmpty())
		{
			if (!OutSpec.RelativePluginDir.IsEmpty() && !OutSpec.RelativePluginDir.Equals(ProjectPluginDir, ESearchCase::IgnoreCase))
			{
				OutError = TEXT("Mount spec must not set both relative_plugin_dir and project_plugin_dir to different values");
				return false;
			}
			OutSpec.RelativePluginDir = ProjectPluginDir;
		}
		Object->TryGetStringField(TEXT("content_dir"), OutSpec.ContentDir);
		OutSpec.PluginName.TrimStartAndEndInline();
		OutSpec.RelativePluginDir.TrimStartAndEndInline();
		if (!OutSpec.RelativePluginDir.IsEmpty() && !NormalizeRelativePluginDir(OutSpec.RelativePluginDir, OutError))
		{
			return false;
		}

		const int32 ResolverCount = (OutSpec.ContentDir.IsEmpty() ? 0 : 1)
			+ (OutSpec.PluginName.IsEmpty() ? 0 : 1)
			+ (OutSpec.RelativePluginDir.IsEmpty() ? 0 : 1);
		if (ResolverCount != 1)
		{
			OutError = TEXT("Each mount_points entry must set exactly one resolver: content_dir, plugin_name, or project_plugin_dir");
			return false;
		}

		if (!OutSpec.ContentDir.IsEmpty())
		{
			OutSpec.ContentDir = NormalizeContentDir(OutSpec.ContentDir);
			OutSpec.ResolutionSource = TEXT("content_dir");
		}
		else if (!OutSpec.PluginName.IsEmpty())
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(OutSpec.PluginName);
			if (Plugin.IsValid())
			{
				if (!Plugin->CanContainContent())
				{
					OutError = FString::Printf(TEXT("Plugin '%s' cannot contain content"), *OutSpec.PluginName);
					return false;
				}
				const FString PluginMountedPath = NormalizeMountPoint(Plugin->GetMountedAssetPath().IsEmpty()
					? FString::Printf(TEXT("/%s/"), *OutSpec.PluginName)
					: Plugin->GetMountedAssetPath());
				if (OutSpec.MountPoint.IsEmpty())
				{
					OutSpec.MountPoint = PluginMountedPath;
				}
				else if (!NormalizeMountPoint(OutSpec.MountPoint).Equals(PluginMountedPath, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("Mount point for plugin '%s' must match plugin mounted asset path %s"), *OutSpec.PluginName, *PluginMountedPath);
					return false;
				}
				OutSpec.ContentDir = NormalizeContentDir(Plugin->GetContentDir());
				OutSpec.ResolutionSource = TEXT("plugin_manager");
			}
			else
			{
				OutError = FString::Printf(TEXT("Plugin '%s' is not loaded; use project_plugin_dir or content_dir to select an explicit filesystem resolver"), *OutSpec.PluginName);
				return false;
			}
		}
		else if (!OutSpec.RelativePluginDir.IsEmpty())
		{
			OutSpec.ContentDir = ResolveRelativePluginContentDir(OutSpec.RelativePluginDir);
			OutSpec.ResolutionSource = TEXT("project_plugin_dir");
		}

		if (OutSpec.MountPoint.IsEmpty())
		{
			OutError = TEXT("Each mount_points entry requires root/mount_point unless plugin_name provides the mounted asset path");
			return false;
		}
		OutSpec.MountPoint = NormalizeMountPoint(OutSpec.MountPoint);
		if (!ValidateMountPoint(OutSpec.MountPoint, bAllowCoreMountPoints, OutError))
		{
			return false;
		}

		OutSpec.bDirectoryExists = FPaths::DirectoryExists(OutSpec.ContentDir);
		const bool bMountExists = FPackageName::MountPointExists(OutSpec.MountPoint);
		const FString ExistingContentDir = bMountExists
			? NormalizeContentDir(FPackageName::GetContentPathForPackageRoot(OutSpec.MountPoint))
			: FString();
		OutSpec.bExistingSame = bMountExists
			&& ExistingContentDir.Equals(OutSpec.ContentDir, ESearchCase::IgnoreCase);
		OutSpec.bExistingDifferent = bMountExists && !OutSpec.bExistingSame;
		return true;
	}

	static TSharedPtr<FJsonObject> MakeMountPointRow(
		const FContentMountSpec& Spec,
		const FString& Status,
		const FString& Reason = FString())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("mount_point"), Spec.MountPoint);
		Row->SetStringField(TEXT("content_dir"), Spec.ContentDir);
		Row->SetStringField(TEXT("resolution_source"), Spec.ResolutionSource);
		if (!Spec.PluginName.IsEmpty())
		{
			Row->SetStringField(TEXT("plugin_name"), Spec.PluginName);
		}
		if (!Spec.RelativePluginDir.IsEmpty())
		{
			Row->SetStringField(TEXT("project_plugin_dir"), Spec.RelativePluginDir);
		}
		Row->SetBoolField(TEXT("directory_exists"), Spec.bDirectoryExists);
		Row->SetBoolField(TEXT("existing_same"), Spec.bExistingSame);
		Row->SetBoolField(TEXT("existing_different"), Spec.bExistingDifferent);
		Row->SetStringField(TEXT("status"), Status);
		if (!Reason.IsEmpty())
		{
			Row->SetStringField(TEXT("reason"), Reason);
		}
		return Row;
	}

	static bool ReadContentMountSpecs(
		const TSharedPtr<FJsonObject>& Params,
		bool bAllowCoreMountPoints,
		TArray<FContentMountSpec>& OutSpecs,
		FString& OutError)
	{
		OutSpecs.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("mount_points"), Values) || !Values)
		{
			OutError = TEXT("Missing required array param 'mount_points'");
			return false;
		}
		if (Values->Num() == 0)
		{
			OutError = TEXT("Param 'mount_points' must contain at least one mount point spec");
			return false;
		}

		TMap<FString, FString> SeenRoots;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FContentMountSpec Spec;
			if (!ResolveContentMountSpec(Value.IsValid() ? Value->AsObject() : nullptr, bAllowCoreMountPoints, Spec, OutError))
			{
				return false;
			}
			if (const FString* SeenContentDir = SeenRoots.Find(Spec.MountPoint))
			{
				if (SeenContentDir->Equals(Spec.ContentDir, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(TEXT("Duplicate mount point spec for %s -> %s"), *Spec.MountPoint, *Spec.ContentDir);
				}
				else
				{
					OutError = FString::Printf(TEXT("Conflicting mount point specs for %s: %s vs %s"), *Spec.MountPoint, **SeenContentDir, *Spec.ContentDir);
				}
				return false;
			}
			SeenRoots.Add(Spec.MountPoint, Spec.ContentDir);
			OutSpecs.Add(MoveTemp(Spec));
		}
		return true;
	}

	static bool ReadDependencyKinds(
		const TSharedPtr<FJsonObject>& Params,
		bool bDefaultHard,
		bool bDefaultSoft,
		TArray<EDependencyKind>& OutKinds,
		FString& OutError)
	{
		OutKinds.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("dependency_kinds"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Kind;
				if (!Value.IsValid() || !Value->TryGetString(Kind))
				{
					OutError = TEXT("Param 'dependency_kinds' must be an array of 'hard'/'soft' strings");
					return false;
				}
				if (Kind.Equals(TEXT("hard"), ESearchCase::IgnoreCase))
				{
					OutKinds.AddUnique(EDependencyKind::Hard);
				}
				else if (Kind.Equals(TEXT("soft"), ESearchCase::IgnoreCase))
				{
					OutKinds.AddUnique(EDependencyKind::Soft);
				}
				else
				{
					OutError = FString::Printf(TEXT("Unsupported dependency kind '%s'; expected 'hard' or 'soft'"), *Kind);
					return false;
				}
			}
		}
		else
		{
			if (bDefaultHard)
			{
				OutKinds.Add(EDependencyKind::Hard);
			}
			if (bDefaultSoft)
			{
				OutKinds.Add(EDependencyKind::Soft);
			}
		}

		if (OutKinds.Num() == 0)
		{
			OutError = TEXT("At least one dependency kind must be enabled");
			return false;
		}
		return true;
	}

	static bool ReadRootRemaps(const TSharedPtr<FJsonObject>& Params, TArray<FRootRemap>& OutRemaps, FString& OutError)
	{
		OutRemaps.Reset();
		const TSharedPtr<FJsonObject>* RemapObject = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("root_remaps"), RemapObject) || !RemapObject || !RemapObject->IsValid())
		{
			OutError = TEXT("Missing required object param 'root_remaps'");
			return false;
		}

		for (const auto& Pair : (*RemapObject)->Values)
		{
			FString Destination;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Destination))
			{
				OutError = TEXT("Param 'root_remaps' must map source root strings to destination root strings");
				return false;
			}

			FRootRemap Remap;
			Remap.SourceRoot = NormalizeRoot(FString(Pair.Key.Len(), *Pair.Key));
			Remap.DestinationRoot = NormalizeRoot(Destination);
			if (!IsValidPackageOrRoot(Remap.SourceRoot) || !IsValidPackageOrRoot(Remap.DestinationRoot))
			{
				OutError = FString::Printf(
					TEXT("Invalid root remap '%s' -> '%s'; roots must be long package roots"),
					*Remap.SourceRoot,
					*Remap.DestinationRoot);
				return false;
			}
			OutRemaps.Add(Remap);
		}

		OutRemaps.Sort([](const FRootRemap& A, const FRootRemap& B)
		{
			return A.SourceRoot.Len() > B.SourceRoot.Len();
		});

		if (OutRemaps.Num() == 0)
		{
			OutError = TEXT("Param 'root_remaps' must contain at least one mapping");
			return false;
		}
		return true;
	}

	static TArray<FString> SourceRootsFromRemaps(const TArray<FRootRemap>& Remaps)
	{
		TArray<FString> Roots;
		for (const FRootRemap& Remap : Remaps)
		{
			Roots.AddUnique(Remap.SourceRoot);
		}
		return Roots;
	}

	static TArray<FString> DestinationRootsFromRemaps(const TArray<FRootRemap>& Remaps)
	{
		TArray<FString> Roots;
		for (const FRootRemap& Remap : Remaps)
		{
			Roots.AddUnique(Remap.DestinationRoot);
		}
		return Roots;
	}

	static TSharedPtr<FJsonObject> CloneParams(const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (Params.IsValid())
		{
			Clone->Values = Params->Values;
		}
		return Clone;
	}

	static void SetStringArrayField(TSharedPtr<FJsonObject> Object, const TCHAR* FieldName, const TArray<FString>& Values)
	{
		if (Object.IsValid())
		{
			Object->SetArrayField(FieldName, StringsToJson(Values));
		}
	}

	static TArray<FString> DestinationPackagesFromPlan(const TSharedPtr<FJsonObject>& Plan)
	{
		TArray<FString> DestinationPackages;
		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			return DestinationPackages;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}
			FString DestinationPackage;
			if (Row->TryGetStringField(TEXT("destination_package"), DestinationPackage))
			{
				DestinationPackages.AddUnique(NormalizePackagePath(DestinationPackage));
			}
		}
		DestinationPackages.Sort();
		return DestinationPackages;
	}

	static bool PackageExists(IAssetRegistry& AssetRegistry, const FString& PackagePath);

	static int32 CountPlannedPackageMutations(
		IAssetRegistry& AssetRegistry,
		const TSharedPtr<FJsonObject>& Plan,
		const FString& CollisionPolicy)
	{
		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			return 0;
		}

		int32 MissingDestinationCount = 0;
		bool bHasBlockingCollision = false;
		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			FString DestinationPackage;
			if (!Row.IsValid()
				|| !Row->TryGetStringField(TEXT("destination_package"), DestinationPackage))
			{
				continue;
			}
			const bool bDestinationExists = PackageExists(
				AssetRegistry,
				NormalizePackagePath(DestinationPackage));
			if (bDestinationExists)
			{
				bHasBlockingCollision |= CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase);
			}
			else
			{
				++MissingDestinationCount;
			}
		}
		return bHasBlockingCollision ? 0 : MissingDestinationCount;
	}

	static TArray<FString> DestinationPackagesFromCopyReport(const TSharedPtr<FJsonObject>& CopyReport)
	{
		TArray<FString> DestinationPackages;
		const TArray<TSharedPtr<FJsonValue>>* CopyRows = nullptr;
		if (!CopyReport.IsValid() || !CopyReport->TryGetArrayField(TEXT("copies"), CopyRows) || !CopyRows)
		{
			return DestinationPackages;
		}

		for (const TSharedPtr<FJsonValue>& Value : *CopyRows)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}

			FString Status;
			Row->TryGetStringField(TEXT("status"), Status);
			if (Status.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase))
			{
				continue;
			}

			FString DestinationPackage;
			if (Row->TryGetStringField(TEXT("destination_package"), DestinationPackage))
			{
				DestinationPackages.AddUnique(NormalizePackagePath(DestinationPackage));
			}
		}
		DestinationPackages.Sort();
		return DestinationPackages;
	}

	static TSharedPtr<FJsonObject> MakePhaseRow(
		const FString& PhaseName,
		const FString& Status,
		bool bOk,
		const FString& ActionName,
		const FString& Detail = FString())
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("phase"), PhaseName);
		Row->SetStringField(TEXT("status"), Status);
		Row->SetBoolField(TEXT("ok"), bOk);
		if (!ActionName.IsEmpty())
		{
			Row->SetStringField(TEXT("action"), ActionName);
		}
		if (!Detail.IsEmpty())
		{
			Row->SetStringField(TEXT("detail"), Detail);
		}
		return Row;
	}

	static bool IsWorkflowStrategy(const FString& Value)
	{
		return Value.Equals(TEXT("plan_only"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_only"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_fixup"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("copy_fixup_validate"), ESearchCase::IgnoreCase);
	}

	static bool IsCopyStrategy(const FString& Value)
	{
		return Value.Equals(TEXT("auto"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("advanced_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("header_patched_advanced_copy"), ESearchCase::IgnoreCase);
	}

	static bool IsExecutablePackageCopyStrategy(const FString& Value, bool bAllowRawPackageCopy)
	{
		return Value.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("advanced_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("header_patched_advanced_copy"), ESearchCase::IgnoreCase)
			|| (bAllowRawPackageCopy && Value.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase));
	}

	static bool IsKnownNonManualCopyStrategy(const FString& Value)
	{
		return Value.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("advanced_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("header_patched_advanced_copy"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase);
	}

	static bool ReadStrategyAlias(
		const TSharedPtr<FJsonObject>& Params,
		FString& InOutWorkflow,
		FString& InOutCopyStrategy,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(TEXT("strategy")))
		{
			return true;
		}

		FString Strategy;
		if (!Params->TryGetStringField(TEXT("strategy"), Strategy))
		{
			OutError = TEXT("Param 'strategy' must be a string");
			return false;
		}
		Strategy.TrimStartAndEndInline();
		if (IsWorkflowStrategy(Strategy))
		{
			InOutWorkflow = Strategy;
			return true;
		}
		if (IsCopyStrategy(Strategy))
		{
			InOutCopyStrategy = Strategy;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unsupported strategy '%s'; expected workflow plan_only/copy_only/copy_fixup/copy_fixup_validate or copy strategy auto/duplicate_asset/advanced_copy/raw_package_file_copy/header_patched_advanced_copy"),
			*Strategy);
		return false;
	}

	static bool IsPackageSelected(const FString& PackagePath, const TArray<FString>& Roots, const TArray<FString>& Packages)
	{
		if (Packages.Contains(PackagePath))
		{
			return true;
		}
		return IsUnderAnyRoot(PackagePath, Roots);
	}

	static FString PackagePairKey(const FString& SourcePackage, const FString& DestinationPackage)
	{
		return NormalizePackagePath(SourcePackage) + TEXT(" -> ") + NormalizePackagePath(DestinationPackage);
	}

	static TMap<FString, FString> BuildSelectedStrategyMap(const TArray<TSharedPtr<FJsonValue>>& StrategyRows)
	{
		TMap<FString, FString> StrategiesByPackagePair;
		for (const TSharedPtr<FJsonValue>& Value : StrategyRows)
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Row.IsValid())
			{
				continue;
			}

			FString SourcePackage;
			FString DestinationPackage;
			FString SelectedStrategy;
			if (Row->TryGetStringField(TEXT("source_package"), SourcePackage)
				&& Row->TryGetStringField(TEXT("destination_package"), DestinationPackage)
				&& Row->TryGetStringField(TEXT("selected_strategy"), SelectedStrategy))
			{
				StrategiesByPackagePair.Add(PackagePairKey(SourcePackage, DestinationPackage), SelectedStrategy);
			}
		}
		return StrategiesByPackagePair;
	}

	static bool BuildCopyStrategyPlan(
		const TSharedPtr<FJsonObject>& Plan,
		const FString& RequestedCopyStrategy,
		const TArray<FString>& HeaderPatchedRoots,
		const TArray<FString>& HeaderPatchedPackages,
		const TArray<FString>& RawPackageRoots,
		const TArray<FString>& RawPackagePackages,
		const TArray<FString>& ManualCopyRoots,
		const TArray<FString>& ManualCopyPackages,
		bool bAllowRawPackageCopy,
		TArray<TSharedPtr<FJsonValue>>& OutStrategyRows,
		int32& OutUnsupportedCount,
		int32& OutExecutableCount)
	{
		OutStrategyRows.Reset();
		OutUnsupportedCount = 0;
		OutExecutableCount = 0;

		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> PackageRow = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!PackageRow.IsValid())
			{
				continue;
			}

			FString SourcePackage;
			FString DestinationPackage;
			PackageRow->TryGetStringField(TEXT("source_package"), SourcePackage);
			PackageRow->TryGetStringField(TEXT("destination_package"), DestinationPackage);
			SourcePackage = NormalizePackagePath(SourcePackage);
			DestinationPackage = NormalizePackagePath(DestinationPackage);

			FString SelectedStrategy = RequestedCopyStrategy;
			FString Reason = TEXT("requested_strategy");
			bool bExecutableByThisAction = IsExecutablePackageCopyStrategy(SelectedStrategy, bAllowRawPackageCopy);

			const bool bManualSelected = IsPackageSelected(SourcePackage, ManualCopyRoots, ManualCopyPackages);
			const bool bHeaderPatchedSelected = IsPackageSelected(SourcePackage, HeaderPatchedRoots, HeaderPatchedPackages);
			const bool bRawSelected = IsPackageSelected(SourcePackage, RawPackageRoots, RawPackagePackages);

			if (RequestedCopyStrategy.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				if (bManualSelected)
				{
					SelectedStrategy = TEXT("manual_single_object_duplicate");
					Reason = TEXT("manual_copy_selector");
					bExecutableByThisAction = false;
				}
				else if (bHeaderPatchedSelected)
				{
					SelectedStrategy = TEXT("header_patched_advanced_copy");
					Reason = TEXT("header_patched_selector");
					bExecutableByThisAction = true;
				}
				else if (bRawSelected)
				{
					SelectedStrategy = TEXT("raw_package_file_copy");
					Reason = bAllowRawPackageCopy ? TEXT("raw_package_selector") : TEXT("allow_raw_package_copy_false");
					bExecutableByThisAction = bAllowRawPackageCopy;
				}
				else
				{
					SelectedStrategy = TEXT("duplicate_asset");
					Reason = TEXT("default_duplicate_asset");
					bExecutableByThisAction = true;
				}
			}
			else if (SelectedStrategy.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase) && !bAllowRawPackageCopy)
			{
				bExecutableByThisAction = false;
				Reason = TEXT("allow_raw_package_copy_false");
			}
			else if (!IsExecutablePackageCopyStrategy(SelectedStrategy, bAllowRawPackageCopy))
			{
				bExecutableByThisAction = false;
				Reason = TEXT("strategy_execution_deferred");
			}

			const bool bSupportedStrategy = IsKnownNonManualCopyStrategy(SelectedStrategy);
			const FString Status = bExecutableByThisAction
				? TEXT("ready")
				: (bSupportedStrategy ? TEXT("blocked") : TEXT("unsupported"));

			if (bExecutableByThisAction)
			{
				++OutExecutableCount;
			}
			else
			{
				++OutUnsupportedCount;
			}

			TSharedPtr<FJsonObject> StrategyRow = MakeShared<FJsonObject>();
			StrategyRow->SetStringField(TEXT("source_package"), SourcePackage);
			StrategyRow->SetStringField(TEXT("destination_package"), DestinationPackage);
			StrategyRow->SetStringField(TEXT("requested_strategy"), RequestedCopyStrategy);
			StrategyRow->SetStringField(TEXT("selected_strategy"), SelectedStrategy);
			StrategyRow->SetStringField(TEXT("status"), Status);
			StrategyRow->SetStringField(TEXT("reason"), Reason);
			StrategyRow->SetBoolField(TEXT("executable_by_this_action"), bExecutableByThisAction);
			OutStrategyRows.Add(MakeShared<FJsonValueObject>(StrategyRow));
		}

		return OutUnsupportedCount == 0;
	}

	static bool ApplyRootRemap(const FString& SourcePackage, const TArray<FRootRemap>& Remaps, FString& OutDestination)
	{
		for (const FRootRemap& Remap : Remaps)
		{
			if (IsUnderRoot(SourcePackage, Remap.SourceRoot))
			{
				const FString Suffix = SourcePackage.Mid(Remap.SourceRoot.Len());
				OutDestination = Remap.DestinationRoot + Suffix;
				return true;
			}
		}
		return false;
	}

	static FString DependencyKindToString(EDependencyKind Kind)
	{
		return Kind == EDependencyKind::Hard ? TEXT("hard") : TEXT("soft");
	}

	static UE::AssetRegistry::EDependencyQuery DependencyQueryForKind(EDependencyKind Kind)
	{
		return Kind == EDependencyKind::Hard
			? UE::AssetRegistry::EDependencyQuery::Hard
			: UE::AssetRegistry::EDependencyQuery::Soft;
	}

	static void AppendDependencies(
		IAssetRegistry& AssetRegistry,
		const FString& PackagePath,
		const TArray<EDependencyKind>& Kinds,
		TArray<TPair<FString, EDependencyKind>>& OutDependencies)
	{
		for (EDependencyKind Kind : Kinds)
		{
			TArray<FAssetIdentifier> Dependencies;
			AssetRegistry.GetDependencies(
				FName(*PackagePath),
				Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package,
				DependencyQueryForKind(Kind));

			for (const FAssetIdentifier& Dependency : Dependencies)
			{
				const FString DependencyPackage = NormalizePackagePath(Dependency.PackageName.ToString());
				if (!DependencyPackage.IsEmpty())
				{
					OutDependencies.Add(TPair<FString, EDependencyKind>(DependencyPackage, Kind));
				}
			}
		}
	}

	static TSharedPtr<FJsonObject> EdgeToJson(
		const FString& Source,
		const FString& Target,
		EDependencyKind Kind,
		bool bWillCopy,
		const FString& DestinationSource,
		const FString& DestinationTarget)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_package"), Source);
		Row->SetStringField(TEXT("target_package"), Target);
		Row->SetStringField(TEXT("kind"), DependencyKindToString(Kind));
		Row->SetBoolField(TEXT("target_will_copy"), bWillCopy);
		Row->SetStringField(TEXT("destination_source_package"), DestinationSource);
		Row->SetStringField(TEXT("destination_target_package"), DestinationTarget);
		return Row;
	}

	static TArray<FString> ScanPackagesUnderRoots(IAssetRegistry& AssetRegistry, const TArray<FString>& Roots)
	{
		TArray<FString> Packages;
		for (const FString& Root : Roots)
		{
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Root));
			Filter.bRecursivePaths = true;

			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);
			for (const FAssetData& Asset : Assets)
			{
				Packages.AddUnique(Asset.PackageName.ToString());
			}
		}
		Packages.Sort();
		return Packages;
	}

	static bool PackageExists(IAssetRegistry& AssetRegistry, const FString& PackagePath)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
		if (Assets.Num() > 0)
		{
			return true;
		}

		FString ExistingFilename;
		if (FPackageName::DoesPackageExist(PackagePath, &ExistingFilename))
		{
			return true;
		}

		return FindPackage(nullptr, *PackagePath) != nullptr;
	}

	static FString ObjectPathForPackageAndObjectName(const FString& PackagePath, const FString& ObjectName)
	{
		return PackagePath + TEXT(".") + ObjectName;
	}

	static FString PrimaryObjectPathForPackage(const FString& PackagePath)
	{
		return ObjectPathForPackageAndObjectName(PackagePath, FPaths::GetBaseFilename(PackagePath));
	}

	static bool TryRemapObjectPath(
		const FString& InObjectPath,
		const TArray<FRootRemap>& Remaps,
		FString& OutObjectPath,
		FString& OutSourcePackage,
		FString& OutDestinationPackage)
	{
		FString NormalizedObjectPath = InObjectPath;
		NormalizedObjectPath.TrimStartAndEndInline();
		NormalizedObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (NormalizedObjectPath.IsEmpty())
		{
			return false;
		}

		FSoftObjectPath SoftPath(NormalizedObjectPath);
		FString AssetPath = SoftPath.GetAssetPathString();
		if (AssetPath.IsEmpty())
		{
			AssetPath = NormalizedObjectPath;
		}

		FString ObjectName;
		if (AssetPath.Contains(TEXT(".")))
		{
			OutSourcePackage = NormalizePackagePath(FPackageName::ObjectPathToPackageName(AssetPath));
			ObjectName = FPackageName::ObjectPathToObjectName(AssetPath);
		}
		else
		{
			OutSourcePackage = NormalizePackagePath(AssetPath);
			ObjectName = FPaths::GetBaseFilename(OutSourcePackage);
		}

		if (!ApplyRootRemap(OutSourcePackage, Remaps, OutDestinationPackage))
		{
			return false;
		}

		OutObjectPath = ObjectPathForPackageAndObjectName(OutDestinationPackage, ObjectName);
		const FString SubPath = SoftPath.GetSubPathString();
		if (!SubPath.IsEmpty())
		{
			OutObjectPath += TEXT(":") + SubPath;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> MakeReferenceRow(
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const FString& ReferenceKind,
		const FString& OldPath,
		const FString& NewPath,
		bool bApplied,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package_path"), PackagePath);
		Row->SetStringField(TEXT("object_path"), ObjectPath);
		Row->SetStringField(TEXT("property_path"), PropertyPath);
		Row->SetStringField(TEXT("reference_kind"), ReferenceKind);
		Row->SetStringField(TEXT("old_path"), OldPath);
		Row->SetStringField(TEXT("new_path"), NewPath);
		Row->SetBoolField(TEXT("applied"), bApplied);
		Row->SetStringField(TEXT("status"), Status);
		return Row;
	}

	static void AddWarning(FReferenceFixupStats& Stats, const FString& PackagePath, const FString& Detail)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("package_path"), PackagePath);
		Warning->SetStringField(TEXT("detail"), Detail);
		Stats.Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	}

	static bool DoesPackageContainWorld(UPackage* Package)
	{
		if (!Package)
		{
			return false;
		}

		bool bContainsWorld = false;
		ForEachObjectWithPackage(Package, [&bContainsWorld](UObject* Object)
		{
			if (Object && Object->IsA<UWorld>())
			{
				bContainsWorld = true;
				return false;
			}
			return true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		}, EGetObjectsFlags::IncludeNestedObjects);
#else
		}, /*bIncludeNestedObjects=*/true);
#endif
		return bContainsWorld;
	}

	static bool SavePackageIfRequested(UPackage* Package, bool bSave, FString& OutSavedFilename, FString& OutError)
	{
		if (!bSave || !Package)
		{
			return true;
		}

		const FString PackageName = Package->GetName();
		const FString Extension = DoesPackageContainWorld(Package)
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		OutSavedFilename = FPackageName::LongPackageNameToFilename(PackageName, Extension);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, nullptr, *OutSavedFilename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("UPackage::SavePackage failed for '%s'"), *OutSavedFilename);
			return false;
		}
		return true;
	}

	static bool SelectPrimaryAssetForPackage(
		IAssetRegistry& AssetRegistry,
		const FString& PackagePath,
		FAssetData& OutAssetData,
		FString& OutError)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
		if (Assets.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Source package has no asset data: %s"), *PackagePath);
			return false;
		}

		const FString ExpectedName = FPaths::GetBaseFilename(PackagePath);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(ExpectedName, ESearchCase::IgnoreCase))
			{
				OutAssetData = Asset;
				return true;
			}
		}

		OutAssetData = Assets[0];
		return true;
	}

	static bool ExtractPackageMapRows(
		IAssetRegistry& AssetRegistry,
		const TSharedPtr<FJsonObject>& Plan,
		const FString& CollisionPolicy,
		const TMap<FString, FString>* SelectedStrategiesByPackagePair,
		TArray<FPackageCopyRow>& OutRows,
		TArray<TSharedPtr<FJsonValue>>& OutErrors)
	{
		OutRows.Reset();
		OutErrors.Reset();

		const TArray<TSharedPtr<FJsonValue>>* PackageMap = nullptr;
		if (!Plan.IsValid() || !Plan->TryGetArrayField(TEXT("package_map"), PackageMap) || !PackageMap)
		{
			TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("detail"), TEXT("Plan result did not include package_map"));
			OutErrors.Add(MakeShared<FJsonValueObject>(Error));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PackageMap)
		{
			const TSharedPtr<FJsonObject> RowObject = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!RowObject.IsValid())
			{
				continue;
			}

			FPackageCopyRow Row;
			RowObject->TryGetStringField(TEXT("source_package"), Row.SourcePackage);
			RowObject->TryGetStringField(TEXT("destination_package"), Row.DestinationPackage);
			RowObject->TryGetBoolField(TEXT("source_exists"), Row.bSourceExists);
			bool bDestinationExistsFromPlan = false;
			RowObject->TryGetBoolField(TEXT("destination_exists"), bDestinationExistsFromPlan);
			Row.SourcePackage = NormalizePackagePath(Row.SourcePackage);
			Row.DestinationPackage = NormalizePackagePath(Row.DestinationPackage);
			Row.bDestinationExists = bDestinationExistsFromPlan || PackageExists(AssetRegistry, Row.DestinationPackage);
			if (SelectedStrategiesByPackagePair)
			{
				if (const FString* SelectedStrategy = SelectedStrategiesByPackagePair->Find(PackagePairKey(Row.SourcePackage, Row.DestinationPackage)))
				{
					Row.SelectedStrategy = *SelectedStrategy;
				}
			}

			FString ErrorText;
			const bool bHasSourceAsset = SelectPrimaryAssetForPackage(AssetRegistry, Row.SourcePackage, Row.SourceAsset, ErrorText);
			if (!Row.bSourceExists || !bHasSourceAsset)
			{
				TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
				Error->SetStringField(TEXT("source_package"), Row.SourcePackage);
				Error->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
				Error->SetStringField(TEXT("reason"), TEXT("source_missing"));
				Error->SetStringField(TEXT("detail"), ErrorText.IsEmpty() ? TEXT("Source package does not exist") : ErrorText);
				OutErrors.Add(MakeShared<FJsonValueObject>(Error));
				continue;
			}

			if (Row.bDestinationExists && CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
				Error->SetStringField(TEXT("source_package"), Row.SourcePackage);
				Error->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
				Error->SetStringField(TEXT("reason"), TEXT("destination_exists"));
				Error->SetStringField(TEXT("detail"), TEXT("Destination package exists; use collision_policy=skip_existing to leave it untouched"));
				OutErrors.Add(MakeShared<FJsonValueObject>(Error));
				continue;
			}

			OutRows.Add(Row);
		}

		return OutErrors.Num() == 0;
	}

	static bool ResolvePackageCopyFilenames(
		const FPackageCopyRow& Row,
		FString& OutSourceFilename,
		FString& OutDestinationFilename,
		FString& OutError)
	{
		if (!FPackageName::DoesPackageExist(Row.SourcePackage, &OutSourceFilename))
		{
			OutError = FString::Printf(TEXT("Source package does not exist on disk: %s"), *Row.SourcePackage);
			return false;
		}

		const FString Extension = FPaths::GetExtension(OutSourceFilename, /*bIncludeDot=*/true);
		if (Extension.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Could not infer package extension for source package: %s"), *Row.SourcePackage);
			return false;
		}

		OutDestinationFilename = FPackageName::LongPackageNameToFilename(Row.DestinationPackage, Extension);
		return true;
	}

	static bool CopyRawPackageFile(const FPackageCopyRow& Row, FString& OutSourceFilename, FString& OutDestinationFilename, FString& OutError)
	{
		if (!ResolvePackageCopyFilenames(Row, OutSourceFilename, OutDestinationFilename, OutError))
		{
			return false;
		}

		if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutDestinationFilename), /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("Could not create destination package directory: %s"), *FPaths::GetPath(OutDestinationFilename));
			return false;
		}

		const uint32 CopyResult = IFileManager::Get().Copy(*OutDestinationFilename, *OutSourceFilename, /*Replace=*/false, /*EvenIfReadOnly=*/false);
		if (CopyResult != COPY_OK)
		{
			OutError = FString::Printf(
				TEXT("Raw package file copy failed (%u): %s -> %s"),
				CopyResult,
				*OutSourceFilename,
				*OutDestinationFilename);
			return false;
		}
		return true;
	}

	static bool RunAdvancedCopyPackageMap(
		const TMap<FString, FString>& SourceAndDestPackages,
		bool bUseHeaderPatching,
		bool bSave,
		FString& OutError)
	{
		if (SourceAndDestPackages.Num() == 0)
		{
			return true;
		}

		IConsoleVariable* HeaderPatchCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("AssetTools.UseHeaderPatchingAdvancedCopy"));
		const int32 PreviousHeaderPatchValue = HeaderPatchCVar ? HeaderPatchCVar->GetInt() : 0;
		if (bUseHeaderPatching && !HeaderPatchCVar)
		{
			OutError = TEXT("AssetTools.UseHeaderPatchingAdvancedCopy cvar is not available in this editor build");
			return false;
		}

		if (HeaderPatchCVar)
		{
			HeaderPatchCVar->Set(bUseHeaderPatching ? 1 : 0, ECVF_SetByCode);
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const bool bCopied = AssetTools.AdvancedCopyPackages(
			SourceAndDestPackages,
			/*bForceAutosave=*/bSave,
			/*bCopyOverAllDestinationOverlaps=*/false);

		if (HeaderPatchCVar)
		{
			HeaderPatchCVar->Set(PreviousHeaderPatchValue, ECVF_SetByCode);
		}

		if (!bCopied)
		{
			OutError = bUseHeaderPatching
				? TEXT("IAssetTools::AdvancedCopyPackages failed with header patching enabled")
				: TEXT("IAssetTools::AdvancedCopyPackages failed");
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> MakeCopyReport(
		const FString& ActionName,
		const FMutationOptions& Mutation,
		const FString& CollisionPolicy,
		const TSharedPtr<FJsonObject>& Plan,
		const TArray<TSharedPtr<FJsonValue>>& PreflightErrors,
		const TArray<TSharedPtr<FJsonValue>>& CopyRows,
		const TArray<TSharedPtr<FJsonValue>>& SavedRows,
		int32 WouldCopyCount,
		int32 CopiedCount,
		int32 SkippedCount,
		int32 SavedCount,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("asset"));
		Result->SetStringField(TEXT("action"), ActionName);
		Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
		Result->SetBoolField(TEXT("save"), Mutation.bSave);
		Result->SetStringField(TEXT("collision_policy"), CollisionPolicy);
		Result->SetStringField(TEXT("status"), Status);
		Result->SetObjectField(TEXT("plan"), Plan);
		Result->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
		Result->SetArrayField(TEXT("copies"), CopyRows);
		Result->SetArrayField(TEXT("saved_packages"), SavedRows);
		Result->SetNumberField(TEXT("would_copy_count"), WouldCopyCount);
		Result->SetNumberField(TEXT("copied_count"), CopiedCount);
		Result->SetNumberField(TEXT("skipped_count"), SkippedCount);
		Result->SetNumberField(TEXT("saved_count"), SavedCount);
		Result->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
		Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.fixup_copied_references"));
		return Result;
	}

	static FMonolithActionResult CopyPackageGraphWithSelectedStrategies(
		const TSharedPtr<FJsonObject>& Plan,
		const TArray<TSharedPtr<FJsonValue>>& StrategyRows,
		const TSharedPtr<FJsonObject>& Params,
		const FMutationOptions& Mutation)
	{
		FString Error;
		FString CollisionPolicy = TEXT("fail_if_exists");
		if (!ReadStringParam(Params, TEXT("collision_policy"), CollisionPolicy, Error))
		{
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
		}
		if (!CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase)
			&& !CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase))
		{
			Error = FString::Printf(TEXT("Unsupported collision_policy '%s'; expected fail_if_exists or skip_existing"), *CollisionPolicy);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		const TMap<FString, FString> SelectedStrategiesByPackagePair = BuildSelectedStrategyMap(StrategyRows);

		TArray<FPackageCopyRow> Rows;
		TArray<TSharedPtr<FJsonValue>> PreflightErrors;
		ExtractPackageMapRows(AssetRegistry, Plan, CollisionPolicy, &SelectedStrategiesByPackagePair, Rows, PreflightErrors);
		if (PreflightErrors.Num() > 0 && !Mutation.bDryRun)
		{
			TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(
				TEXT("copy_package_graph_with_strategy"),
				Mutation,
				CollisionPolicy,
				Plan,
				PreflightErrors,
				TArray<TSharedPtr<FJsonValue>>(),
				TArray<TSharedPtr<FJsonValue>>(),
				0,
				0,
				0,
				0,
				TEXT("preflight_failed"));
			return FMonolithActionResult::Error(TEXT("copy_package_graph_with_strategy preflight failed"), ErrInvalidParams)
				.WithErrorData(ErrorResult);
		}

		TArray<TSharedPtr<FJsonValue>> CopyRows;
		TArray<TSharedPtr<FJsonValue>> SavedRows;
		TMap<FString, TSharedPtr<FJsonObject>> CopyRowsByPackagePair;
		int32 WouldCopyCount = 0;
		int32 CopiedCount = 0;
		int32 SkippedCount = 0;
		int32 SavedCount = 0;

		TArray<FString> FilesToScan;
		TArray<FPackageCopyRow> DuplicateRows;
		TArray<FPackageCopyRow> RawRows;
		TMap<FString, FString> AdvancedCopyMap;
		TMap<FString, FString> HeaderPatchedAdvancedCopyMap;

		for (const FPackageCopyRow& Row : Rows)
		{
			const bool bSkipExisting = Row.bDestinationExists && CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase);
			if (bSkipExisting)
			{
				++SkippedCount;
			}
			else
			{
				++WouldCopyCount;
			}

			TSharedPtr<FJsonObject> CopyRow = MakeShared<FJsonObject>();
			CopyRow->SetStringField(TEXT("source_package"), Row.SourcePackage);
			CopyRow->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
			CopyRow->SetStringField(TEXT("selected_strategy"), Row.SelectedStrategy);
			CopyRow->SetStringField(TEXT("source_asset"), Row.SourceAsset.GetSoftObjectPath().ToString());
			CopyRow->SetStringField(TEXT("status"), bSkipExisting ? TEXT("skip_existing") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("pending")));
			CopyRows.Add(MakeShared<FJsonValueObject>(CopyRow));
			CopyRowsByPackagePair.Add(PackagePairKey(Row.SourcePackage, Row.DestinationPackage), CopyRow);

			if (bSkipExisting || Mutation.bDryRun)
			{
				continue;
			}

			if (Row.SelectedStrategy.Equals(TEXT("duplicate_asset"), ESearchCase::IgnoreCase))
			{
				DuplicateRows.Add(Row);
			}
			else if (Row.SelectedStrategy.Equals(TEXT("raw_package_file_copy"), ESearchCase::IgnoreCase))
			{
				RawRows.Add(Row);
			}
			else if (Row.SelectedStrategy.Equals(TEXT("advanced_copy"), ESearchCase::IgnoreCase))
			{
				AdvancedCopyMap.Add(Row.SourcePackage, Row.DestinationPackage);
			}
			else if (Row.SelectedStrategy.Equals(TEXT("header_patched_advanced_copy"), ESearchCase::IgnoreCase))
			{
				HeaderPatchedAdvancedCopyMap.Add(Row.SourcePackage, Row.DestinationPackage);
			}
		}

		if (!Mutation.bDryRun && WouldCopyCount > 0)
		{
			if (!Mutation.bSave && (AdvancedCopyMap.Num() > 0 || HeaderPatchedAdvancedCopyMap.Num() > 0))
			{
				Error = TEXT("advanced_copy and header_patched_advanced_copy require save=true because Unreal AssetTools may save copied packages during AdvancedCopyPackages");
				TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
				ErrorResult->SetStringField(TEXT("failure_detail"), Error);
				return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorResult);
			}

			FScopedTransaction Transaction(NSLOCTEXT("MonolithAsset", "CopyPackageGraphWithStrategy", "Monolith Copy Package Graph With Strategy"));
			IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

			for (const FPackageCopyRow& Row : DuplicateRows)
			{
				UObject* SourceAsset = Row.SourceAsset.GetAsset();
				if (!SourceAsset)
				{
					Error = FString::Printf(TEXT("Could not load source asset '%s'"), *Row.SourceAsset.GetSoftObjectPath().ToString());
					TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
					ErrorResult->SetStringField(TEXT("failure_detail"), Error);
					return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
				}

				const FString DestinationPackagePath = FPackageName::GetLongPackagePath(Row.DestinationPackage);
				const FString DestinationAssetName = FPaths::GetBaseFilename(Row.DestinationPackage);
				UObject* Duplicated = AssetTools.DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceAsset);
				if (!Duplicated)
				{
					Error = FString::Printf(TEXT("DuplicateAsset failed: %s -> %s"), *Row.SourcePackage, *Row.DestinationPackage);
					TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
					ErrorResult->SetStringField(TEXT("failure_detail"), Error);
					return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
				}

				++CopiedCount;
				Duplicated->MarkPackageDirty();
				AssetRegistry.AssetCreated(Duplicated);

				if (TSharedPtr<FJsonObject>* AppliedRow = CopyRowsByPackagePair.Find(PackagePairKey(Row.SourcePackage, Row.DestinationPackage)))
				{
					(*AppliedRow)->SetStringField(TEXT("duplicated_asset"), Duplicated->GetPathName());
					(*AppliedRow)->SetStringField(TEXT("status"), TEXT("copied"));
				}

				FString SavedFilename;
				FString SaveError;
				if (!SavePackageIfRequested(Duplicated->GetOutermost(), Mutation.bSave, SavedFilename, SaveError))
				{
					TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
					ErrorResult->SetStringField(TEXT("failure_detail"), SaveError);
					return FMonolithActionResult::Error(SaveError).WithErrorData(ErrorResult);
				}
				if (Mutation.bSave)
				{
					++SavedCount;
					TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
					SavedRow->SetStringField(TEXT("package_path"), Row.DestinationPackage);
					SavedRow->SetStringField(TEXT("filename"), SavedFilename);
					SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
					FilesToScan.AddUnique(SavedFilename);
				}
			}

			for (const FPackageCopyRow& Row : RawRows)
			{
				FString SourceFilename;
				FString DestinationFilename;
				if (!CopyRawPackageFile(Row, SourceFilename, DestinationFilename, Error))
				{
					TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
					ErrorResult->SetStringField(TEXT("failure_detail"), Error);
					return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
				}

				++CopiedCount;
				++SavedCount;
				FilesToScan.AddUnique(DestinationFilename);

				if (TSharedPtr<FJsonObject>* AppliedRow = CopyRowsByPackagePair.Find(PackagePairKey(Row.SourcePackage, Row.DestinationPackage)))
				{
					(*AppliedRow)->SetStringField(TEXT("source_filename"), SourceFilename);
					(*AppliedRow)->SetStringField(TEXT("copied_file"), DestinationFilename);
					(*AppliedRow)->SetStringField(TEXT("status"), TEXT("copied"));
				}

				TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
				SavedRow->SetStringField(TEXT("package_path"), Row.DestinationPackage);
				SavedRow->SetStringField(TEXT("filename"), DestinationFilename);
				SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
			}

			auto MarkAdvancedRowsCopied = [&CopyRowsByPackagePair, &CopiedCount, &SavedCount, &SavedRows, &FilesToScan, &Error](const TMap<FString, FString>& PackageMap, const FString& StrategyName) -> bool
			{
				for (const TPair<FString, FString>& Pair : PackageMap)
				{
					FString ExistingDestinationFilename;
					if (!FPackageName::DoesPackageExist(Pair.Value, &ExistingDestinationFilename))
					{
						Error = FString::Printf(TEXT("%s did not create destination package '%s'"), *StrategyName, *Pair.Value);
						return false;
					}

					++CopiedCount;

					FString SourceFilename;
					FString DestinationFilename;
					FString FilenameError;
					FPackageCopyRow FilenameRow;
					FilenameRow.SourcePackage = Pair.Key;
					FilenameRow.DestinationPackage = Pair.Value;
					if (ResolvePackageCopyFilenames(FilenameRow, SourceFilename, DestinationFilename, FilenameError))
					{
						++SavedCount;
						FilesToScan.AddUnique(ExistingDestinationFilename);
						TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
						SavedRow->SetStringField(TEXT("package_path"), Pair.Value);
						SavedRow->SetStringField(TEXT("filename"), ExistingDestinationFilename);
						SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
					}

					if (TSharedPtr<FJsonObject>* AppliedRow = CopyRowsByPackagePair.Find(PackagePairKey(Pair.Key, Pair.Value)))
					{
						(*AppliedRow)->SetStringField(TEXT("advanced_copy_strategy"), StrategyName);
						(*AppliedRow)->SetStringField(TEXT("copied_file"), ExistingDestinationFilename);
						(*AppliedRow)->SetStringField(TEXT("status"), TEXT("copied"));
					}
				}
				return true;
			};

			if (!RunAdvancedCopyPackageMap(AdvancedCopyMap, /*bUseHeaderPatching=*/false, Mutation.bSave, Error))
			{
				TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
				ErrorResult->SetStringField(TEXT("failure_detail"), Error);
				return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
			}
			if (!MarkAdvancedRowsCopied(AdvancedCopyMap, TEXT("advanced_copy")))
			{
				TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
				ErrorResult->SetStringField(TEXT("failure_detail"), Error);
				return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
			}

			if (!RunAdvancedCopyPackageMap(HeaderPatchedAdvancedCopyMap, /*bUseHeaderPatching=*/true, Mutation.bSave, Error))
			{
				TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
				ErrorResult->SetStringField(TEXT("failure_detail"), Error);
				return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
			}
			if (!MarkAdvancedRowsCopied(HeaderPatchedAdvancedCopyMap, TEXT("header_patched_advanced_copy")))
			{
				TSharedPtr<FJsonObject> ErrorResult = MakeCopyReport(TEXT("copy_package_graph_with_strategy"), Mutation, CollisionPolicy, Plan, PreflightErrors, CopyRows, SavedRows, WouldCopyCount, CopiedCount, SkippedCount, SavedCount, TEXT("failed"));
				ErrorResult->SetStringField(TEXT("failure_detail"), Error);
				return FMonolithActionResult::Error(Error).WithErrorData(ErrorResult);
			}

			if (FilesToScan.Num() > 0)
			{
				AssetRegistry.ScanFilesSynchronous(FilesToScan, /*bForceRescan=*/true);
			}
		}

		return FMonolithActionResult::Success(MakeCopyReport(
			TEXT("copy_package_graph_with_strategy"),
			Mutation,
			CollisionPolicy,
			Plan,
			PreflightErrors,
			CopyRows,
			SavedRows,
			WouldCopyCount,
			CopiedCount,
			SkippedCount,
			SavedCount,
			Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")));
	}

	static FMonolithActionResult CleanupRedirectorsForCopiedRoots(
		const TArray<FString>& DestinationRoots,
		const TArray<FString>& PackagePaths,
		const FMutationOptions& Mutation)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> RedirectorAssets;
		for (const FString& PackagePath : PackagePaths)
		{
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssetsByPackageName(FName(*PackagePath), Assets, /*bIncludeOnlyOnDiskAssets=*/false);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.IsRedirector())
				{
					RedirectorAssets.AddUnique(Asset);
				}
			}
		}
		RedirectorAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.GetObjectPathString() < Right.GetObjectPathString();
		});
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("namespace"), TEXT("asset"));
		Result->SetStringField(TEXT("action"), TEXT("cleanup_copied_redirectors"));
		Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
		Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
		Result->SetArrayField(TEXT("package_paths"), StringsToJson(PackagePaths));
		Result->SetNumberField(TEXT("redirector_count"), RedirectorAssets.Num());
		if (RedirectorAssets.IsEmpty())
		{
			Result->SetBoolField(TEXT("ok"), true);
			Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
			return FMonolithActionResult::Success(Result);
		}

		TArray<TSharedPtr<FJsonValue>> MoveRows;
		TArray<FString> AllowedSourceRoots = DestinationRoots;
		TArray<FString> AllowedDestinationRoots;
		TArray<TSharedPtr<FJsonValue>> MappingRows;
		for (const FAssetData& RedirectorAsset : RedirectorAssets)
		{
			FString DestinationExportPath;
			if (!RedirectorAsset.GetTagValue(FName(TEXT("DestinationObject")), DestinationExportPath))
			{
				Result->SetStringField(TEXT("status"), TEXT("blocked"));
				Result->SetBoolField(TEXT("ok"), false);
				return FMonolithActionResult::Error(
					FString::Printf(
						TEXT("affected copied redirector is missing DestinationObject tag: %s"),
						*RedirectorAsset.GetObjectPathString()))
					.WithErrorData(Result);
			}
			const FString DestinationObjectPath = FPackageName::ExportTextPathToObjectPath(DestinationExportPath);
			const FString DestinationPackage = FPackageName::ObjectPathToPackageName(DestinationObjectPath);
			if (!FPackageName::IsValidObjectPath(DestinationObjectPath)
				|| !FPackageName::IsValidLongPackageName(DestinationPackage, /*bIncludeReadOnlyRoots=*/true))
			{
				Result->SetStringField(TEXT("status"), TEXT("blocked"));
				Result->SetBoolField(TEXT("ok"), false);
				return FMonolithActionResult::Error(
					FString::Printf(
						TEXT("affected copied redirector has an invalid destination: %s"),
						*RedirectorAsset.GetObjectPathString()))
					.WithErrorData(Result);
			}

			const FString SourcePackage = RedirectorAsset.PackageName.ToString();
			AllowedSourceRoots.AddUnique(FPackageName::GetLongPackagePath(SourcePackage));
			AllowedDestinationRoots.AddUnique(FPackageName::GetLongPackagePath(DestinationPackage));
			TSharedPtr<FJsonObject> MoveRow = MakeShared<FJsonObject>();
			MoveRow->SetStringField(TEXT("source"), SourcePackage);
			MoveRow->SetStringField(TEXT("destination"), DestinationPackage);
			MoveRow->SetStringField(TEXT("source_object_path"), RedirectorAsset.GetObjectPathString());
			MoveRow->SetStringField(TEXT("destination_object_path"), DestinationObjectPath);
			MoveRows.Add(MakeShared<FJsonValueObject>(MoveRow));

			TSharedPtr<FJsonObject> MappingRow = MakeShared<FJsonObject>();
			MappingRow->SetStringField(TEXT("source"), SourcePackage);
			MappingRow->SetStringField(TEXT("destination"), DestinationPackage);
			MappingRow->SetStringField(TEXT("destination_object_path"), DestinationObjectPath);
			MappingRows.Add(MakeShared<FJsonValueObject>(MappingRow));
		}

		Result->SetArrayField(TEXT("redirector_mappings"), MappingRows);
		TSharedPtr<FJsonObject> CleanupParams = MakeShared<FJsonObject>();
		CleanupParams->SetArrayField(TEXT("moves"), MoveRows);
		CleanupParams->SetArrayField(TEXT("allowed_source_roots"), StringsToJson(AllowedSourceRoots));
		CleanupParams->SetArrayField(TEXT("allowed_destination_roots"), StringsToJson(AllowedDestinationRoots));
		CleanupParams->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		CleanupParams->SetBoolField(TEXT("confirm"), Mutation.bConfirm);
		const FMonolithActionResult CleanupResult =
			FMonolithAssetMoveActions::CleanupMovedRedirectors(CleanupParams);
		TArray<TSharedPtr<FJsonValue>> CleanupReports;
		if (CleanupResult.Result.IsValid())
		{
			CleanupReports.Add(MakeShared<FJsonValueObject>(CleanupResult.Result));
		}
		else if (const TSharedPtr<FJsonObject> CleanupErrorData =
			MonolithAsset::GetErrorDataObject(CleanupResult))
		{
			CleanupReports.Add(MakeShared<FJsonValueObject>(CleanupErrorData));
		}
		Result->SetArrayField(TEXT("cleanup_reports"), CleanupReports);
		if (!CleanupResult.bSuccess)
		{
			Result->SetBoolField(TEXT("ok"), false);
			FString CleanupStatus = TEXT("failed");
			if (const TSharedPtr<FJsonObject> CleanupErrorData =
				MonolithAsset::GetErrorDataObject(CleanupResult))
			{
				CleanupErrorData->TryGetStringField(TEXT("status"), CleanupStatus);
			}
			Result->SetStringField(TEXT("status"), CleanupStatus);
			return FMonolithActionResult::Error(
				FString::Printf(
					TEXT("copied redirector cleanup failed: %s"),
					*CleanupResult.ErrorMessage),
				CleanupResult.ErrorCode)
				.WithErrorData(Result);
		}
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
		return FMonolithActionResult::Success(Result);
	}

	static bool FixupObjectProperty(
		FObjectPropertyBase* ObjectProperty,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		UObject* OldObject = ObjectProperty ? ObjectProperty->GetObjectPropertyValue(ValuePtr) : nullptr;
		if (!OldObject)
		{
			return false;
		}

		FString NewObjectPath;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldObject->GetPathName(), Remaps, NewObjectPath, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		UObject* NewObject = FSoftObjectPath(NewObjectPath).TryLoad();
		if (!NewObject)
		{
			const FString Status = TEXT("target_missing");
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("hard_object"),
				OldObject->GetPathName(),
				NewObjectPath,
				false,
				Status)));
			Stats.bHasBlockingErrors |= Options.bRequireTargets;
			return false;
		}

		if (ObjectProperty->PropertyClass && !NewObject->IsA(ObjectProperty->PropertyClass))
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("hard_object"),
				OldObject->GetPathName(),
				NewObjectPath,
				false,
				TEXT("target_type_mismatch"))));
			Stats.bHasBlockingErrors = Options.bRequireTargets;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		if (bApply)
		{
			ObjectProperty->SetObjectPropertyValue(ValuePtr, NewObject);
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("hard_object"),
			OldObject->GetPathName(),
			NewObjectPath,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupSoftObjectProperty(
		FSoftObjectProperty* SoftObjectProperty,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		const FSoftObjectPtr OldSoftPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
		const FSoftObjectPath OldPath = OldSoftPtr.ToSoftObjectPath();
		const FString OldPathString = OldPath.ToString();
		if (OldPathString.IsEmpty())
		{
			return false;
		}

		FString NewPathString;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldPathString, Remaps, NewPathString, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		const bool bTargetExists = PackageExists(FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(), DestinationPackage);
		if (Options.bRequireTargets && !bTargetExists)
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("soft_object"),
				OldPathString,
				NewPathString,
				false,
				TEXT("target_missing"))));
			Stats.bHasBlockingErrors = true;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		if (bApply)
		{
			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(NewPathString)));
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("soft_object"),
			OldPathString,
			NewPathString,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupSoftObjectPathStruct(
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		FSoftObjectPath* OldPath = static_cast<FSoftObjectPath*>(ValuePtr);
		if (!OldPath || OldPath->IsNull())
		{
			return false;
		}

		FString NewPathString;
		FString SourcePackage;
		FString DestinationPackage;
		if (!TryRemapObjectPath(OldPath->ToString(), Remaps, NewPathString, SourcePackage, DestinationPackage))
		{
			return false;
		}

		++Stats.CandidateCount;
		const bool bTargetExists = PackageExists(FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get(), DestinationPackage);
		if (Options.bRequireTargets && !bTargetExists)
		{
			Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
				PackagePath,
				ObjectPath,
				PropertyPath,
				TEXT("soft_object_path"),
				OldPath->ToString(),
				NewPathString,
				false,
				TEXT("target_missing"))));
			Stats.bHasBlockingErrors = true;
			return false;
		}

		const bool bApply = !Options.Mutation.bDryRun;
		const FString OldPathString = OldPath->ToString();
		if (bApply)
		{
			*OldPath = FSoftObjectPath(NewPathString);
			++Stats.AppliedCount;
			Stats.ChangedPackages.Add(PackagePath);
		}

		Stats.References.Add(MakeShared<FJsonValueObject>(MakeReferenceRow(
			PackagePath,
			ObjectPath,
			PropertyPath,
			TEXT("soft_object_path"),
			OldPathString,
			NewPathString,
			bApply,
			bApply ? TEXT("applied") : TEXT("dry_run"))));
		return bApply;
	}

	static bool FixupPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats);

	static bool FixupStructProperties(
		UStruct* Struct,
		void* StructValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& Prefix,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		bool bChanged = false;
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* ChildProperty = *It;
			if (!ChildProperty || ChildProperty->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}

			void* ChildValuePtr = ChildProperty->ContainerPtrToValuePtr<void>(StructValuePtr);
			const FString ChildPath = Prefix.IsEmpty()
				? ChildProperty->GetName()
				: Prefix + TEXT(".") + ChildProperty->GetName();
			bChanged |= FixupPropertyValue(ChildProperty, ChildValuePtr, PackagePath, ObjectPath, ChildPath, Remaps, Options, Stats);
		}
		return bChanged;
	}

	static bool FixupPropertyValue(
		FProperty* Property,
		void* ValuePtr,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& PropertyPath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		if (!Property || !ValuePtr)
		{
			return false;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			return FixupSoftObjectProperty(SoftObjectProperty, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return FixupObjectProperty(ObjectProperty, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				return FixupSoftObjectPathStruct(ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
			}
			return FixupStructProperties(StructProperty->Struct, ValuePtr, PackagePath, ObjectPath, PropertyPath, Remaps, Options, Stats);
		}

		if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			bool bChanged = false;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				bChanged |= FixupPropertyValue(
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			return bChanged;
		}

		if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			bool bChanged = false;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				bChanged |= FixupPropertyValue(
					SetProperty->ElementProp,
					Helper.GetElementPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			if (bChanged && !Options.Mutation.bDryRun)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			bool bChanged = false;
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				bChanged |= FixupPropertyValue(
					MapProperty->KeyProp,
					Helper.GetKeyPtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Key"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
				bChanged |= FixupPropertyValue(
					MapProperty->ValueProp,
					Helper.GetValuePtr(Index),
					PackagePath,
					ObjectPath,
					FString::Printf(TEXT("%s{%d}.Value"), *PropertyPath, Index),
					Remaps,
					Options,
					Stats);
			}
			if (bChanged && !Options.Mutation.bDryRun)
			{
				Helper.Rehash();
			}
			return bChanged;
		}

		return false;
	}

	static bool FixupPackageReferences(
		const FString& PackagePath,
		const TArray<FRootRemap>& Remaps,
		const FReferenceFixupOptions& Options,
		FReferenceFixupStats& Stats)
	{
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		if (!Package)
		{
			Package = LoadPackage(nullptr, *PackagePath, LOAD_None);
		}
		if (!Package)
		{
			AddWarning(Stats, PackagePath, TEXT("Could not load package"));
			Stats.bHasBlockingErrors = Options.Mutation.bStrict;
			return false;
		}

		++Stats.CheckedPackageCount;
		TArray<UObject*> Objects;
		ForEachObjectWithPackage(Package, [&Objects](UObject* Object)
		{
			if (Object && !Object->HasAnyFlags(RF_Transient | RF_ClassDefaultObject))
			{
				Objects.Add(Object);
			}
			return true;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8
		}, EGetObjectsFlags::IncludeNestedObjects);
#else
		}, /*bIncludeNestedObjects=*/true);
#endif

		bool bPackageChanged = false;
		for (UObject* Object : Objects)
		{
			if (!Object || !Object->GetClass())
			{
				continue;
			}
			++Stats.CheckedObjectCount;

			const int32 AppliedBefore = Stats.AppliedCount;
			if (!Options.Mutation.bDryRun)
			{
				Object->Modify();
			}
			const bool bChanged = FixupStructProperties(
				Object->GetClass(),
				Object,
				PackagePath,
				Object->GetPathName(),
				FString(),
				Remaps,
				Options,
				Stats);
			if (bChanged && !Options.Mutation.bDryRun && Stats.AppliedCount > AppliedBefore)
			{
				Object->MarkPackageDirty();
				bPackageChanged = true;
			}
		}

		if (bPackageChanged)
		{
			Package->MarkPackageDirty();
		}
		return bPackageChanged;
	}
}

void FMonolithAssetPackageGraphActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("register_content_mount_points"),
		TEXT("Safely register explicit Unreal content mount points before package graph planning/copying. Defaults to dry-run; requires confirm=true for process mount-table mutation."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::RegisterContentMountPoints),
		FParamSchemaBuilder()
			.Required(TEXT("mount_points"), TEXT("array"), TEXT("Mount point specs with root/mount_point plus exactly one resolver: content_dir, plugin_name, or project_plugin_dir"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview mount registrations without mutating the process mount table"), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required when dry_run=false"), TEXT("false"))
			.Optional(TEXT("allow_override"), TEXT("bool"), TEXT("Allow mounting a root that already resolves to a different local content directory"), TEXT("false"))
			.Optional(TEXT("allow_core_mount_points"), TEXT("bool"), TEXT("Allow /Game/, /Engine/, or /Script/ mount-point specs; normally refused"), TEXT("false"))
			.Optional(TEXT("scan_asset_registry"), TEXT("bool"), TEXT("After confirmed registration, synchronously scan the mounted roots in AssetRegistry"), TEXT("true"))
			.Optional(TEXT("force_rescan"), TEXT("bool"), TEXT("Force AssetRegistry rescan when scan_asset_registry=true"), TEXT("false"))
			.Optional(TEXT("probe_packages"), TEXT("array"), TEXT("Optional packages to test with FPackageName::DoesPackageExist after preflight/registration"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));

	Registry.RegisterAction(TEXT("asset"), TEXT("plan_package_graph_copy"),
		TEXT("Plan a package graph copy/remap from AssetRegistry dependencies without loading, copying, or fixing up assets"),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::PlanPackageGraphCopy),
		FParamSchemaBuilder()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to include in the plan"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots, e.g. {\"/Game/UI\":\"/SpeedMaps/UI\"}"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap"), TEXT("512"))
			.Optional(TEXT("strategy"), TEXT("string"), TEXT("Explicit planning strategy; only registry_only_plan is currently implemented"), TEXT("registry_only_plan"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));

	Registry.RegisterAction(TEXT("asset"), TEXT("copy_package_graph_with_remap"),
		TEXT("Copy a planned package dependency graph by duplicating source assets to root-remapped destination packages. Requires dry_run=true or confirm=true; never overwrites existing destinations."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap),
		FParamSchemaBuilder()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to copy"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap"), TEXT("512"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.Optional(TEXT("collision_policy"), TEXT("string"), TEXT("fail_if_exists or skip_existing. Overwrite is intentionally unsupported."), TEXT("fail_if_exists"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Return the copy plan without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save duplicated destination packages"), TEXT("true"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));

	Registry.RegisterAction(TEXT("asset"), TEXT("copy_package_graph_with_strategy"),
		TEXT("Orchestrate a guarded package graph copy strategy: plan-only, copy-only, copy+fixup, or copy+fixup+dependency-closure validation."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy),
		FParamSchemaBuilder()
			.Required(TEXT("root_packages"), TEXT("array"), TEXT("Source package roots to copy or plan"))
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("workflow"), TEXT("string"), TEXT("plan_only, copy_only, copy_fixup, or copy_fixup_validate"), TEXT("copy_fixup_validate"))
			.Optional(TEXT("strategy"), TEXT("string"), TEXT("Compatibility alias: accepts workflow values or copy_strategy values"), TEXT(""))
			.Optional(TEXT("copy_strategy"), TEXT("string"), TEXT("auto, duplicate_asset, advanced_copy, raw_package_file_copy, or header_patched_advanced_copy"), TEXT("auto"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to follow: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Traversal safety cap for plan/copy"), TEXT("512"))
			.Optional(TEXT("fixup_max_packages"), TEXT("integer"), TEXT("Fixup safety cap"), TEXT("1000"))
			.Optional(TEXT("closure_max_packages"), TEXT("integer"), TEXT("Dependency-closure validation safety cap"), TEXT("1000"))
			.Optional(TEXT("check_collisions"), TEXT("bool"), TEXT("Report existing destination packages"), TEXT("true"))
			.Optional(TEXT("collision_policy"), TEXT("string"), TEXT("fail_if_exists or skip_existing. Overwrite is intentionally unsupported."), TEXT("fail_if_exists"))
			.Optional(TEXT("header_patched_roots"), TEXT("array"), TEXT("Source roots that select header_patched_advanced_copy when copy_strategy=auto"))
			.Optional(TEXT("header_patched_packages"), TEXT("array"), TEXT("Source packages that select header_patched_advanced_copy when copy_strategy=auto"))
			.Optional(TEXT("raw_package_roots"), TEXT("array"), TEXT("Source roots that select raw_package_file_copy when copy_strategy=auto"))
			.Optional(TEXT("raw_package_packages"), TEXT("array"), TEXT("Source packages that select raw_package_file_copy when copy_strategy=auto"))
			.Optional(TEXT("manual_copy_roots"), TEXT("array"), TEXT("Source roots that are known to require manual single-object duplication"))
			.Optional(TEXT("manual_copy_packages"), TEXT("array"), TEXT("Source packages that are known to require manual single-object duplication"))
			.Optional(TEXT("allow_raw_package_copy"), TEXT("bool"), TEXT("Opt-in flag required before raw package file copy rows can execute"), TEXT("false"))
			.Optional(TEXT("cleanup_redirectors"), TEXT("bool"), TEXT("Delete only exact affected copied redirectors with intact destinations and zero hard/soft referencers through asset.cleanup_moved_redirectors; never opens a modal fixup report"), TEXT("false"))
			.Optional(TEXT("allowed_external_roots"), TEXT("array"), TEXT("External roots allowed during dependency-closure validation"))
			.Optional(TEXT("legacy_source_roots"), TEXT("array"), TEXT("Source roots that should not remain referenced; defaults to root_remaps source roots when omitted"))
			.Optional(TEXT("require_targets"), TEXT("bool"), TEXT("Fail fixup when a remapped reference target package is missing"), TEXT("true"))
			.Optional(TEXT("run_fixup_on_dry_run"), TEXT("bool"), TEXT("Run fixup dry-run against existing destination packages instead of only reporting planned params"), TEXT("false"))
			.Optional(TEXT("run_closure_on_dry_run"), TEXT("bool"), TEXT("Run dependency closure dry-run against existing destination packages instead of only reporting planned params"), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Return the orchestrated copy plan without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save duplicated or changed packages"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("bool"), TEXT("Treat fixup blockers as errors"), TEXT("true"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));

	Registry.RegisterAction(TEXT("asset"), TEXT("fixup_copied_references"),
		TEXT("Rewrite reflected hard and soft references inside copied destination packages from source roots to root-remapped destination roots. Requires dry_run=true or confirm=true."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::FixupCopiedReferences),
		FParamSchemaBuilder()
			.Required(TEXT("root_remaps"), TEXT("object"), TEXT("Object mapping source roots to destination roots"))
			.Optional(TEXT("destination_roots"), TEXT("array"), TEXT("Destination roots to scan; defaults to root_remaps destinations"))
			.Optional(TEXT("package_paths"), TEXT("array"), TEXT("Specific destination packages to fix up"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Fixup safety cap"), TEXT("1000"))
			.Optional(TEXT("require_targets"), TEXT("bool"), TEXT("Fail if a remapped reference target package is missing"), TEXT("true"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Report reference rewrites without mutating assets"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required for mutation when dry_run is false"), TEXT("false"))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save changed packages"), TEXT("true"))
			.Optional(TEXT("strict"), TEXT("bool"), TEXT("Preflight every candidate and reject load/fixup blockers or max_packages truncation before mutation"), TEXT("true"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));

	Registry.RegisterAction(TEXT("asset"), TEXT("validate_dependency_closure"),
		TEXT("Validate that destination packages do not depend on disallowed package roots after a copy/remap plan"),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetPackageGraphActions::ValidateDependencyClosure),
		FParamSchemaBuilder()
			.Required(TEXT("destination_roots"), TEXT("array"), TEXT("Destination roots whose package closure should be validated"))
			.Optional(TEXT("package_paths"), TEXT("array"), TEXT("Specific destination packages to validate; omitted scans destination_roots"))
			.Optional(TEXT("allowed_external_roots"), TEXT("array"), TEXT("External roots allowed in dependencies, e.g. /Script, /Engine"))
			.Optional(TEXT("legacy_source_roots"), TEXT("array"), TEXT("Source roots that should not remain referenced"))
			.Optional(TEXT("dependency_kinds"), TEXT("array"), TEXT("Dependency kinds to validate: hard, soft. Default: both"))
			.Optional(TEXT("max_packages"), TEXT("integer"), TEXT("Validation safety cap"), TEXT("1000"))
			.StrictComplexTypes()
			.Build(),
		TEXT("PackageGraph"));
}

FMonolithActionResult FMonolithAssetPackageGraphActions::RegisterContentMountPoints(const TSharedPtr<FJsonObject>& Params)
{
	FMutationOptions Mutation;
	Mutation.bDryRun = true;

	FString Error;
	if (!ReadMutationOptions(Params, Mutation, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mutation_guard"), Error));
	}

	bool bAllowOverride = false;
	bool bAllowCoreMountPoints = false;
	bool bScanAssetRegistry = true;
	bool bForceRescan = false;
	if (!ReadBoolParam(Params, TEXT("allow_override"), bAllowOverride, Error)
		|| !ReadBoolParam(Params, TEXT("allow_core_mount_points"), bAllowCoreMountPoints, Error)
		|| !ReadBoolParam(Params, TEXT("scan_asset_registry"), bScanAssetRegistry, Error)
		|| !ReadBoolParam(Params, TEXT("force_rescan"), bForceRescan, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	TArray<FString> ProbePackages;
	if (!ReadStringArrayParam(Params, TEXT("probe_packages"), false, ProbePackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("probe_packages"), Error));
	}

	TArray<FContentMountSpec> Specs;
	if (!ReadContentMountSpecs(Params, bAllowCoreMountPoints, Specs, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mount_points"), Error));
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	TArray<TSharedPtr<FJsonValue>> PreflightErrors;
	TArray<FString> RootsToScan;
	int32 WouldRegisterCount = 0;
	int32 RegisteredCount = 0;
	int32 AlreadyRegisteredCount = 0;
	int32 ConflictCount = 0;
	int32 MissingDirCount = 0;
	TArray<int32> RegisterIndices;

	for (int32 Index = 0; Index < Specs.Num(); ++Index)
	{
		FContentMountSpec& Spec = Specs[Index];
		if (!Spec.bDirectoryExists)
		{
			++MissingDirCount;
			TSharedPtr<FJsonObject> Row = MakeMountPointRow(Spec, TEXT("blocked"), TEXT("content_dir_missing"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			PreflightErrors.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		if (Spec.bExistingDifferent && !bAllowOverride)
		{
			++ConflictCount;
			TSharedPtr<FJsonObject> Row = MakeMountPointRow(Spec, TEXT("blocked"), TEXT("mount_point_conflicts_with_existing_root"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			PreflightErrors.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		if (Spec.bExistingSame)
		{
			++AlreadyRegisteredCount;
			Rows.Add(MakeShared<FJsonValueObject>(MakeMountPointRow(Spec, TEXT("already_registered"))));
			continue;
		}

		++WouldRegisterCount;
		RegisterIndices.Add(Index);
		if (Mutation.bDryRun)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeMountPointRow(Spec, TEXT("would_register"))));
		}
	}

	if (PreflightErrors.Num() > 0 && !Mutation.bDryRun)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
		ErrorResult->SetStringField(TEXT("action"), TEXT("register_content_mount_points"));
		ErrorResult->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		ErrorResult->SetArrayField(TEXT("mount_points"), Rows);
		ErrorResult->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
		ErrorResult->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
		return FMonolithActionResult::Error(TEXT("register_content_mount_points preflight failed"), ErrInvalidParams)
			.WithErrorData(ErrorResult);
	}

	// Mount-point registration is process-global. A failure partway through the
	// loop previously left the roots registered before it in place while the
	// action still returned bSuccess=true, so callers continued into package
	// planning against a half-mounted process. Track what this invocation
	// registered so it can be undone.
	TArray<TPair<FString, FString>> RegisteredByThisCall;
	if (!Mutation.bDryRun)
	{
		for (const int32 Index : RegisterIndices)
		{
			FContentMountSpec& Spec = Specs[Index];
			FPackageName::RegisterMountPoint(Spec.MountPoint, Spec.ContentDir);
			const bool bMounted = FPackageName::MountPointExists(Spec.MountPoint);
			const FString RegisteredContentDir = bMounted
				? NormalizeContentDir(FPackageName::GetContentPathForPackageRoot(Spec.MountPoint))
				: FString();
			if (!bMounted || !RegisteredContentDir.Equals(Spec.ContentDir, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> Row = MakeMountPointRow(Spec, TEXT("failed"), TEXT("register_mount_point_failed"));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
				PreflightErrors.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}

			++RegisteredCount;
			RegisteredByThisCall.Emplace(Spec.MountPoint, Spec.ContentDir);
			RootsToScan.AddUnique(Spec.MountPoint);
			Rows.Add(MakeShared<FJsonValueObject>(MakeMountPointRow(Spec, TEXT("registered"))));
		}

		if (PreflightErrors.Num() > 0)
		{
			// Undo this invocation's registrations so the process is left as it
			// was found, then report the failure instead of a success the caller
			// would build on.
			for (int32 Index = RegisteredByThisCall.Num() - 1; Index >= 0; --Index)
			{
				FPackageName::UnRegisterMountPoint(
					RegisteredByThisCall[Index].Key,
					RegisteredByThisCall[Index].Value);
			}

			TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
			ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
			ErrorResult->SetStringField(TEXT("action"), TEXT("register_content_mount_points"));
			ErrorResult->SetStringField(TEXT("status"), TEXT("register_failed"));
			ErrorResult->SetBoolField(TEXT("rolled_back"), true);
			ErrorResult->SetNumberField(
				TEXT("rolled_back_count"),
				RegisteredByThisCall.Num());
			ErrorResult->SetArrayField(TEXT("mount_points"), Rows);
			ErrorResult->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
			ErrorResult->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
			return FMonolithActionResult::Error(
				TEXT("register_content_mount_points failed to register every requested root; this invocation's registrations were rolled back"),
				ErrInternal)
				.WithErrorData(ErrorResult);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ScannedRoots;
	if (!Mutation.bDryRun && bScanAssetRegistry && RootsToScan.Num() > 0)
	{
		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		AssetRegistry.ScanPathsSynchronous(RootsToScan, bForceRescan);
		ScannedRoots = StringsToJson(RootsToScan);
	}

	TArray<TSharedPtr<FJsonValue>> ProbeRows;
	for (const FString& ProbePackage : ProbePackages)
	{
		FString ExistingFilename;
		TSharedPtr<FJsonObject> Probe = MakeShared<FJsonObject>();
		Probe->SetStringField(TEXT("package"), ProbePackage);
		Probe->SetBoolField(TEXT("exists"), FPackageName::DoesPackageExist(ProbePackage, &ExistingFilename));
		if (!ExistingFilename.IsEmpty())
		{
			Probe->SetStringField(TEXT("filename"), ExistingFilename);
		}
		ProbeRows.Add(MakeShared<FJsonValueObject>(Probe));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("register_content_mount_points"));
	Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirm"), Mutation.bConfirm);
	Result->SetBoolField(TEXT("ok"), PreflightErrors.Num() == 0);
	Result->SetStringField(TEXT("status"), PreflightErrors.Num() > 0 ? TEXT("preflight_failed") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")));
	Result->SetBoolField(TEXT("allow_override"), bAllowOverride);
	Result->SetBoolField(TEXT("allow_core_mount_points"), bAllowCoreMountPoints);
	Result->SetBoolField(TEXT("scan_asset_registry"), bScanAssetRegistry);
	Result->SetBoolField(TEXT("force_rescan"), bForceRescan);
	Result->SetArrayField(TEXT("mount_points"), Rows);
	Result->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
	Result->SetArrayField(TEXT("scanned_roots"), ScannedRoots);
	Result->SetArrayField(TEXT("probe_packages"), ProbeRows);
	Result->SetNumberField(TEXT("mount_point_count"), Specs.Num());
	Result->SetNumberField(TEXT("would_register_count"), WouldRegisterCount);
	Result->SetNumberField(TEXT("registered_count"), RegisteredCount);
	Result->SetNumberField(TEXT("already_registered_count"), AlreadyRegisteredCount);
	Result->SetNumberField(TEXT("conflict_count"), ConflictCount);
	Result->SetNumberField(TEXT("missing_dir_count"), MissingDirCount);
	Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.plan_package_graph_copy"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::PlanPackageGraphCopy(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> RootPackages;
	TArray<FRootRemap> Remaps;
	TArray<EDependencyKind> DependencyKinds;
	FString Error;
	if (!ReadStringArrayParam(Params, TEXT("root_packages"), true, RootPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_packages"), Error));
	}
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}
	if (!ReadDependencyKinds(Params, true, true, DependencyKinds, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("dependency_kinds"), Error));
	}

	int32 MaxPackages = 512;
	bool bCheckCollisions = true;
	if (!ReadIntParam(Params, TEXT("max_packages"), MaxPackages, Error)
		|| !ReadBoolParam(Params, TEXT("check_collisions"), bCheckCollisions, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	FString Strategy = TEXT("registry_only_plan");
	Params->TryGetStringField(TEXT("strategy"), Strategy);
	if (!Strategy.Equals(TEXT("registry_only_plan"), ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(TEXT("Unsupported strategy '%s'; only 'registry_only_plan' is implemented"), *Strategy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("strategy"), Error));
	}

	const TArray<FString> SourceRoots = SourceRootsFromRemaps(Remaps);
	for (const FString& RootPackage : RootPackages)
	{
		if (!IsValidPackageOrRoot(RootPackage) || !IsUnderAnyRoot(RootPackage, SourceRoots))
		{
			Error = FString::Printf(TEXT("Root package '%s' is not under any root_remaps source root"), *RootPackage);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_packages"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FString> Queue = RootPackages;
	TSet<FString> PlannedSet;
	TArray<FString> PlannedPackages;
	TArray<TSharedPtr<FJsonValue>> Edges;
	TArray<TSharedPtr<FJsonValue>> ExternalDependencies;
	TArray<TSharedPtr<FJsonValue>> Collisions;
	bool bTruncated = false;

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		if (PlannedSet.Num() >= MaxPackages)
		{
			bTruncated = true;
			break;
		}

		const FString CurrentPackage = Queue[QueueIndex];
		if (PlannedSet.Contains(CurrentPackage))
		{
			continue;
		}
		PlannedSet.Add(CurrentPackage);
		PlannedPackages.Add(CurrentPackage);

		FString CurrentDestination;
		ApplyRootRemap(CurrentPackage, Remaps, CurrentDestination);

		TArray<TPair<FString, EDependencyKind>> Dependencies;
		AppendDependencies(AssetRegistry, CurrentPackage, DependencyKinds, Dependencies);
		for (const TPair<FString, EDependencyKind>& Dependency : Dependencies)
		{
			FString DependencyDestination;
			const bool bDependencyWillCopy = ApplyRootRemap(Dependency.Key, Remaps, DependencyDestination);
			Edges.Add(MakeShared<FJsonValueObject>(EdgeToJson(
				CurrentPackage,
				Dependency.Key,
				Dependency.Value,
				bDependencyWillCopy,
				CurrentDestination,
				DependencyDestination)));

			if (bDependencyWillCopy)
			{
				if (!PlannedSet.Contains(Dependency.Key) && !Queue.Contains(Dependency.Key))
				{
					Queue.Add(Dependency.Key);
				}
			}
			else
			{
				ExternalDependencies.Add(MakeShared<FJsonValueObject>(EdgeToJson(
					CurrentPackage,
					Dependency.Key,
					Dependency.Value,
					false,
					CurrentDestination,
					FString())));
			}
		}
	}

	PlannedPackages.Sort();

	TArray<TSharedPtr<FJsonValue>> PackageMap;
	for (const FString& SourcePackage : PlannedPackages)
	{
		FString DestinationPackage;
		ApplyRootRemap(SourcePackage, Remaps, DestinationPackage);

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_package"), SourcePackage);
		Row->SetStringField(TEXT("destination_package"), DestinationPackage);
		Row->SetBoolField(TEXT("source_exists"), PackageExists(AssetRegistry, SourcePackage));
		const bool bDestinationExists = bCheckCollisions && PackageExists(AssetRegistry, DestinationPackage);
		Row->SetBoolField(TEXT("destination_exists"), bDestinationExists);
		PackageMap.Add(MakeShared<FJsonValueObject>(Row));

		if (bDestinationExists)
		{
			TSharedPtr<FJsonObject> Collision = MakeShared<FJsonObject>();
			Collision->SetStringField(TEXT("source_package"), SourcePackage);
			Collision->SetStringField(TEXT("destination_package"), DestinationPackage);
			Collisions.Add(MakeShared<FJsonValueObject>(Collision));
		}
	}

	TArray<TSharedPtr<FJsonValue>> RemapRows;
	for (const FRootRemap& Remap : Remaps)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_root"), Remap.SourceRoot);
		Row->SetStringField(TEXT("destination_root"), Remap.DestinationRoot);
		RemapRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("plan_package_graph_copy"));
	Result->SetStringField(TEXT("strategy"), Strategy);
	Result->SetBoolField(TEXT("read_only"), true);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	// A truncated plan omits dependencies queued after the cap, so copying from it
	// would silently produce an incomplete graph. Consumers must treat the plan as
	// non-executable unless the caller opts into a partial copy.
	Result->SetBoolField(TEXT("executable"), !bTruncated);
	if (bTruncated)
	{
		Result->SetStringField(
			TEXT("not_executable_reason"),
			TEXT("dependency traversal reached max_packages; raise max_packages or opt into allow_partial_copy"));
	}
	Result->SetNumberField(TEXT("max_packages"), MaxPackages);
	Result->SetArrayField(TEXT("root_remaps"), RemapRows);
	Result->SetArrayField(TEXT("root_packages"), StringsToJson(RootPackages));
	Result->SetArrayField(TEXT("package_map"), PackageMap);
	Result->SetArrayField(TEXT("dependency_edges"), Edges);
	Result->SetArrayField(TEXT("external_dependencies"), ExternalDependencies);
	Result->SetArrayField(TEXT("collisions"), Collisions);
	Result->SetNumberField(TEXT("package_count"), PlannedPackages.Num());
	Result->SetNumberField(TEXT("dependency_edge_count"), Edges.Num());
	Result->SetNumberField(TEXT("external_dependency_count"), ExternalDependencies.Num());
	Result->SetNumberField(TEXT("collision_count"), Collisions.Num());
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::CopyPackageGraphWithRemap(const TSharedPtr<FJsonObject>& Params)
{
	FMutationOptions Mutation;
	FString Error;
	if (!ReadMutationOptions(Params, Mutation, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mutation_guard"), Error));
	}

	FString CollisionPolicy = TEXT("fail_if_exists");
	if (!ReadStringParam(Params, TEXT("collision_policy"), CollisionPolicy, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
	}
	if (!CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase)
		&& !CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(TEXT("Unsupported collision_policy '%s'; expected fail_if_exists or skip_existing"), *CollisionPolicy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("collision_policy"), Error));
	}

	TSharedPtr<FJsonObject> PlanParams = MakeShared<FJsonObject>();
	if (Params.IsValid())
	{
		PlanParams->Values = Params->Values;
	}
	PlanParams->SetStringField(TEXT("strategy"), TEXT("registry_only_plan"));
	PlanParams->SetBoolField(TEXT("check_collisions"), true);

	FMonolithActionResult PlanResult = PlanPackageGraphCopy(PlanParams);
	if (!PlanResult.bSuccess)
	{
		return PlanResult;
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FPackageCopyRow> Rows;
	TArray<TSharedPtr<FJsonValue>> PreflightErrors;
	ExtractPackageMapRows(AssetRegistry, PlanResult.Result, CollisionPolicy, nullptr, Rows, PreflightErrors);
	if (PreflightErrors.Num() > 0 && !Mutation.bDryRun)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
		ErrorResult->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_remap"));
		ErrorResult->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
		ErrorResult->SetObjectField(TEXT("plan"), PlanResult.Result);
		ErrorResult->SetStringField(TEXT("status"), TEXT("preflight_failed"));
		ErrorResult->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
		return FMonolithActionResult::Error(TEXT("copy_package_graph_with_remap preflight failed"), ErrInvalidParams)
			.WithErrorData(ErrorResult);
	}

	TArray<TSharedPtr<FJsonValue>> CopyRows;
	TArray<TSharedPtr<FJsonValue>> SavedRows;
	int32 WouldCopyCount = 0;
	int32 CopiedCount = 0;
	int32 SkippedCount = 0;
	int32 SavedCount = 0;

	for (const FPackageCopyRow& Row : Rows)
	{
		const bool bSkipExisting = Row.bDestinationExists && CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase);
		if (bSkipExisting)
		{
			++SkippedCount;
		}
		else
		{
			++WouldCopyCount;
		}

		TSharedPtr<FJsonObject> CopyRow = MakeShared<FJsonObject>();
		CopyRow->SetStringField(TEXT("source_package"), Row.SourcePackage);
		CopyRow->SetStringField(TEXT("destination_package"), Row.DestinationPackage);
		CopyRow->SetStringField(TEXT("source_asset"), Row.SourceAsset.GetSoftObjectPath().ToString());
		CopyRow->SetStringField(TEXT("status"), bSkipExisting ? TEXT("skip_existing") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("pending")));
		CopyRows.Add(MakeShared<FJsonValueObject>(CopyRow));
	}

	if (!Mutation.bDryRun && WouldCopyCount > 0)
	{
		FScopedTransaction Transaction(NSLOCTEXT("MonolithAsset", "CopyPackageGraphWithRemap", "Monolith Copy Package Graph With Remap"));
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FPackageCopyRow& Row = Rows[Index];
			const bool bSkipExisting = Row.bDestinationExists && CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase);
			if (bSkipExisting)
			{
				continue;
			}

			UObject* SourceAsset = Row.SourceAsset.GetAsset();
			if (!SourceAsset)
			{
				Error = FString::Printf(TEXT("Could not load source asset '%s'"), *Row.SourceAsset.GetSoftObjectPath().ToString());
				return FMonolithActionResult::Error(Error).WithErrorData(PlanResult.Result);
			}

			const FString DestinationPackagePath = FPackageName::GetLongPackagePath(Row.DestinationPackage);
			const FString DestinationAssetName = FPaths::GetBaseFilename(Row.DestinationPackage);
			UObject* Duplicated = AssetTools.DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceAsset);
			if (!Duplicated)
			{
				Error = FString::Printf(TEXT("DuplicateAsset failed: %s -> %s"), *Row.SourcePackage, *Row.DestinationPackage);
				return FMonolithActionResult::Error(Error).WithErrorData(PlanResult.Result);
			}

			++CopiedCount;
			Duplicated->MarkPackageDirty();
			AssetRegistry.AssetCreated(Duplicated);

			TSharedPtr<FJsonObject> AppliedRow = CopyRows[Index]->AsObject();
			if (AppliedRow.IsValid())
			{
				AppliedRow->SetStringField(TEXT("duplicated_asset"), Duplicated->GetPathName());
				AppliedRow->SetStringField(TEXT("status"), TEXT("copied"));
			}

			FString SavedFilename;
			FString SaveError;
			if (!SavePackageIfRequested(Duplicated->GetOutermost(), Mutation.bSave, SavedFilename, SaveError))
			{
				return FMonolithActionResult::Error(SaveError).WithErrorData(PlanResult.Result);
			}
			if (Mutation.bSave)
			{
				++SavedCount;
				TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
				SavedRow->SetStringField(TEXT("package_path"), Row.DestinationPackage);
				SavedRow->SetStringField(TEXT("filename"), SavedFilename);
				SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_remap"));
	Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Mutation.bSave);
	Result->SetStringField(TEXT("collision_policy"), CollisionPolicy);
	Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
	Result->SetObjectField(TEXT("plan"), PlanResult.Result);
	Result->SetArrayField(TEXT("preflight_errors"), PreflightErrors);
	Result->SetArrayField(TEXT("copies"), CopyRows);
	Result->SetArrayField(TEXT("saved_packages"), SavedRows);
	Result->SetNumberField(TEXT("would_copy_count"), WouldCopyCount);
	Result->SetNumberField(TEXT("copied_count"), CopiedCount);
	Result->SetNumberField(TEXT("skipped_count"), SkippedCount);
	Result->SetNumberField(TEXT("saved_count"), SavedCount);
	Result->SetNumberField(TEXT("preflight_error_count"), PreflightErrors.Num());
	Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.fixup_copied_references"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::CopyPackageGraphWithStrategy(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	FString Workflow = TEXT("copy_fixup_validate");
	FString CopyStrategy = TEXT("auto");
	FString CollisionPolicy = TEXT("fail_if_exists");
	if (!ReadStrategyAlias(Params, Workflow, CopyStrategy, Error)
		|| !ReadStringParam(Params, TEXT("workflow"), Workflow, Error)
		|| !ReadStringParam(Params, TEXT("copy_strategy"), CopyStrategy, Error)
		|| !ReadStringParam(Params, TEXT("collision_policy"), CollisionPolicy, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("strategy"), Error));
	}

	const bool bPlanOnly = Workflow.Equals(TEXT("plan_only"), ESearchCase::IgnoreCase);
	const bool bCopyOnly = Workflow.Equals(TEXT("copy_only"), ESearchCase::IgnoreCase);
	const bool bCopyFixup = Workflow.Equals(TEXT("copy_fixup"), ESearchCase::IgnoreCase);
	const bool bCopyFixupValidate = Workflow.Equals(TEXT("copy_fixup_validate"), ESearchCase::IgnoreCase);
	if (!IsWorkflowStrategy(Workflow))
	{
		Error = FString::Printf(
			TEXT("Unsupported workflow '%s'; expected plan_only, copy_only, copy_fixup, or copy_fixup_validate"),
			*Workflow);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("workflow"), Error));
	}
	if (!IsCopyStrategy(CopyStrategy))
	{
		Error = FString::Printf(
			TEXT("Unsupported copy_strategy '%s'; expected auto, duplicate_asset, advanced_copy, raw_package_file_copy, or header_patched_advanced_copy"),
			*CopyStrategy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("copy_strategy"), Error));
	}
	if (!CollisionPolicy.Equals(TEXT("fail_if_exists"), ESearchCase::IgnoreCase)
		&& !CollisionPolicy.Equals(TEXT("skip_existing"), ESearchCase::IgnoreCase))
	{
		Error = FString::Printf(
			TEXT("Unsupported collision_policy '%s'; expected fail_if_exists or skip_existing"),
			*CollisionPolicy);
		return FMonolithActionResult::Error(Error, ErrInvalidParams)
			.WithErrorData(ErrorData(TEXT("collision_policy"), Error));
	}

	TArray<FRootRemap> Remaps;
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}

	TArray<FString> HeaderPatchedRoots;
	TArray<FString> HeaderPatchedPackages;
	TArray<FString> RawPackageRoots;
	TArray<FString> RawPackagePackages;
	TArray<FString> ManualCopyRoots;
	TArray<FString> ManualCopyPackages;
	bool bRunFixupOnDryRun = false;
	bool bRunClosureOnDryRun = false;
	bool bAllowRawPackageCopy = false;
	bool bCleanupRedirectors = false;
	int32 FixupMaxPackages = 1000;
	int32 ClosureMaxPackages = 1000;
	if (!ReadStringArrayParam(Params, TEXT("header_patched_roots"), false, HeaderPatchedRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("header_patched_packages"), false, HeaderPatchedPackages, Error)
		|| !ReadStringArrayParam(Params, TEXT("raw_package_roots"), false, RawPackageRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("raw_package_packages"), false, RawPackagePackages, Error)
		|| !ReadStringArrayParam(Params, TEXT("manual_copy_roots"), false, ManualCopyRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("manual_copy_packages"), false, ManualCopyPackages, Error)
		|| !ReadBoolParam(Params, TEXT("allow_raw_package_copy"), bAllowRawPackageCopy, Error)
		|| !ReadBoolParam(Params, TEXT("cleanup_redirectors"), bCleanupRedirectors, Error)
		|| !ReadBoolParam(Params, TEXT("run_fixup_on_dry_run"), bRunFixupOnDryRun, Error)
		|| !ReadBoolParam(Params, TEXT("run_closure_on_dry_run"), bRunClosureOnDryRun, Error)
		|| !ReadIntParam(Params, TEXT("fixup_max_packages"), FixupMaxPackages, Error)
		|| !ReadIntParam(Params, TEXT("closure_max_packages"), ClosureMaxPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("copy_package_graph_with_strategy"));
	Result->SetStringField(TEXT("workflow"), Workflow);
	Result->SetStringField(TEXT("copy_strategy"), CopyStrategy);

	TArray<TSharedPtr<FJsonValue>> Phases;
	const TArray<FString> DestinationRoots = DestinationRootsFromRemaps(Remaps);
	const TArray<FString> SourceRoots = SourceRootsFromRemaps(Remaps);

	auto FailWithPhaseReport = [&Result, &Phases](const FString& Message, int32 Code, const FMonolithActionResult& ChildResult)
	{
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetStringField(TEXT("status"), TEXT("failed"));
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("child_error_message"), ChildResult.ErrorMessage);
		Result->SetNumberField(TEXT("child_error_code"), ChildResult.ErrorCode);
		if (const TSharedPtr<FJsonObject> ChildErrorData =
			MonolithAsset::GetErrorDataObject(ChildResult))
		{
			Result->SetObjectField(TEXT("child_error_data"), ChildErrorData);
		}
		return FMonolithActionResult::Error(Message, Code).WithErrorData(Result);
	};

	TSharedPtr<FJsonObject> PlanParams = CloneParams(Params);
	PlanParams->SetStringField(TEXT("strategy"), TEXT("registry_only_plan"));
	FMonolithActionResult PlanResult = PlanPackageGraphCopy(PlanParams);
	if (!PlanResult.bSuccess)
	{
		Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("plan"), TEXT("failed"), false, TEXT("asset.plan_package_graph_copy"), PlanResult.ErrorMessage)));
		return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy plan phase failed"), PlanResult.ErrorCode, PlanResult);
	}

	TArray<TSharedPtr<FJsonValue>> StrategyRows;
	int32 UnsupportedStrategyCount = 0;
	int32 ExecutableStrategyCount = 0;
	const bool bAllStrategiesExecutable = BuildCopyStrategyPlan(
		PlanResult.Result,
		CopyStrategy,
		HeaderPatchedRoots,
		HeaderPatchedPackages,
		RawPackageRoots,
		RawPackagePackages,
		ManualCopyRoots,
		ManualCopyPackages,
		bAllowRawPackageCopy,
		StrategyRows,
		UnsupportedStrategyCount,
		ExecutableStrategyCount);

	const TArray<FString> DestinationPackages = DestinationPackagesFromPlan(PlanResult.Result);
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("plan"),
		TEXT("success"),
		true,
		TEXT("asset.plan_package_graph_copy"))));
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("strategy_plan"),
		bAllStrategiesExecutable ? TEXT("ready") : TEXT("unsupported"),
		bAllStrategiesExecutable,
		TEXT("asset.copy_package_graph_with_strategy"))));

	Result->SetObjectField(TEXT("plan"), PlanResult.Result);
	Result->SetArrayField(TEXT("strategy_plan"), StrategyRows);
	Result->SetNumberField(TEXT("strategy_plan_count"), StrategyRows.Num());
	Result->SetNumberField(TEXT("unsupported_strategy_count"), UnsupportedStrategyCount);
	Result->SetNumberField(TEXT("executable_strategy_count"), ExecutableStrategyCount);
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("package_paths"), StringsToJson(DestinationPackages));
	Result->SetArrayField(TEXT("planned_package_paths"), StringsToJson(DestinationPackages));

	if (bPlanOnly)
	{
		TArray<FString> NextActions;
		NextActions.Add(TEXT("asset.copy_package_graph_with_strategy"));
		NextActions.Add(TEXT("asset.copy_package_graph_with_remap"));
		Result->SetBoolField(TEXT("read_only"), true);
		Result->SetBoolField(TEXT("dry_run"), true);
		Result->SetBoolField(TEXT("confirmed"), false);
		Result->SetBoolField(TEXT("save"), false);
		Result->SetBoolField(TEXT("ok"), bAllStrategiesExecutable);
		Result->SetStringField(TEXT("status"), TEXT("plan_only"));
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
		return FMonolithActionResult::Success(Result);
	}

	FMutationOptions Mutation;
	if (!ReadMutationOptions(Params, Mutation, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("mutation_guard"), Error));
	}

	if (!bAllStrategiesExecutable)
	{
		TArray<FString> NextActions;
		NextActions.Add(TEXT("asset.copy_package_graph_with_strategy"));
		NextActions.Add(TEXT("asset.copy_package_graph_with_remap"));
		Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
		Result->SetBoolField(TEXT("save"), Mutation.bSave);
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetStringField(TEXT("status"), TEXT("unsupported_copy_strategy"));
		Result->SetArrayField(TEXT("phases"), Phases);
		Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
		if (Mutation.bDryRun)
		{
			return FMonolithActionResult::Success(Result);
		}
		return FMonolithActionResult::Error(
			TEXT("copy_package_graph_with_strategy selected copy strategies that are not executable by this action"),
			ErrInvalidParams).WithErrorData(Result);
	}

	IAssetRegistry& CleanupPreflightRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const int32 PlannedPackageMutationCount = CountPlannedPackageMutations(
		CleanupPreflightRegistry,
		PlanResult.Result,
		CollisionPolicy);
	Result->SetNumberField(TEXT("planned_package_mutation_count"), PlannedPackageMutationCount);
	if (bCleanupRedirectors && !Mutation.bDryRun && PlannedPackageMutationCount > 0)
	{
		IAssetRegistry& AssetRegistry = CleanupPreflightRegistry;
		const bool bCleanupContextReady = GIsEditor
			&& !IsRunningCommandlet()
			&& IsInGameThread()
			&& !AssetRegistry.IsLoadingAssets()
			&& ISourceControlModule::Get().IsEnabled()
			&& ISourceControlModule::Get().GetProvider().IsAvailable();
		if (!bCleanupContextReady)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("redirector_cleanup_precondition"),
				TEXT("blocked"),
				false,
				TEXT("asset.cleanup_copied_redirectors"),
				TEXT("cleanup was rejected before copy mutation"))));
			Result->SetArrayField(TEXT("phases"), Phases);
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("status"), TEXT("redirector_cleanup_precondition_failed"));
			Result->SetBoolField(TEXT("is_editor"), GIsEditor);
			Result->SetBoolField(TEXT("is_commandlet"), IsRunningCommandlet());
			Result->SetBoolField(TEXT("is_in_game_thread"), IsInGameThread());
			Result->SetBoolField(TEXT("asset_registry_loading"), AssetRegistry.IsLoadingAssets());
			Result->SetBoolField(
				TEXT("source_control_enabled"),
				ISourceControlModule::Get().IsEnabled());
			Result->SetBoolField(
				TEXT("source_control_available"),
				ISourceControlModule::Get().IsEnabled()
					&& ISourceControlModule::Get().GetProvider().IsAvailable());
			return FMonolithActionResult::Error(
				TEXT("cleanup_redirectors=true requires an editor game-thread call, a completed AssetRegistry scan, and available source control before copy mutation"),
				ErrInvalidParams)
				.WithErrorData(Result);
		}
	}

	FMonolithActionResult CopyResult = CopyPackageGraphWithSelectedStrategies(PlanResult.Result, StrategyRows, Params, Mutation);
	if (!CopyResult.bSuccess)
	{
		Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("copy"), TEXT("failed"), false, TEXT("asset.copy_package_graph_with_strategy"), CopyResult.ErrorMessage)));
		return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy copy phase failed"), CopyResult.ErrorCode, CopyResult);
	}

	double PreflightErrorCount = 0.0;
	CopyResult.Result->TryGetNumberField(TEXT("preflight_error_count"), PreflightErrorCount);
	bool bOk = PreflightErrorCount <= 0.0;
	Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
		TEXT("copy"),
		PreflightErrorCount > 0.0 ? TEXT("preflight_errors") : (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")),
		PreflightErrorCount <= 0.0,
		TEXT("asset.copy_package_graph_with_strategy"))));

	Result->SetObjectField(TEXT("copy_report"), CopyResult.Result);
	const TArray<FString> AffectedDestinationPackages = DestinationPackagesFromCopyReport(CopyResult.Result);
	Result->SetArrayField(TEXT("package_paths"), StringsToJson(AffectedDestinationPackages));
	Result->SetNumberField(TEXT("affected_package_count"), AffectedDestinationPackages.Num());

	const bool bStrategyNeedsFixup = bCopyFixup || bCopyFixupValidate;
	if (bStrategyNeedsFixup)
	{
		TSharedPtr<FJsonObject> FixupParams = CloneParams(Params);
		FixupParams->Values.Remove(TEXT("max_packages"));
		FixupParams->SetNumberField(TEXT("max_packages"), FixupMaxPackages);
		SetStringArrayField(FixupParams, TEXT("destination_roots"), DestinationRoots);
		SetStringArrayField(FixupParams, TEXT("package_paths"), AffectedDestinationPackages);
		FixupParams->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
		FixupParams->SetBoolField(TEXT("confirm"), Mutation.bConfirm);
		FixupParams->SetBoolField(TEXT("save"), Mutation.bSave);
		FixupParams->SetBoolField(TEXT("strict"), Mutation.bStrict);

		if (AffectedDestinationPackages.Num() == 0)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("fixup"),
				TEXT("skipped"),
				true,
				TEXT("asset.fixup_copied_references"),
				TEXT("no copied destination packages; collision_policy may have skipped all rows"))));
			Result->SetObjectField(TEXT("planned_fixup_params"), FixupParams);
		}
		else if (Mutation.bDryRun && !bRunFixupOnDryRun)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("fixup"),
				TEXT("skipped"),
				true,
				TEXT("asset.fixup_copied_references"),
				TEXT("dry_run does not create destination packages; set run_fixup_on_dry_run=true to scan existing destinations"))));
			Result->SetObjectField(TEXT("planned_fixup_params"), FixupParams);
		}
		else
		{
			FMonolithActionResult FixupResult = FixupCopiedReferences(FixupParams);
			if (!FixupResult.bSuccess)
			{
				Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("fixup"), TEXT("failed"), false, TEXT("asset.fixup_copied_references"), FixupResult.ErrorMessage)));
				return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy fixup phase failed"), FixupResult.ErrorCode, FixupResult);
			}

			bool bFixupOk = true;
			FixupResult.Result->TryGetBoolField(TEXT("ok"), bFixupOk);
			bOk &= bFixupOk;
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("fixup"),
				bFixupOk ? (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")) : TEXT("issues"),
				bFixupOk,
				TEXT("asset.fixup_copied_references"))));
			Result->SetObjectField(TEXT("fixup_report"), FixupResult.Result);
		}
	}

	if (bCleanupRedirectors)
	{
		if (AffectedDestinationPackages.Num() == 0)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("redirector_cleanup"),
				TEXT("skipped"),
				true,
				TEXT("asset.cleanup_copied_redirectors"),
				TEXT("no copied destination packages; collision_policy may have skipped all rows"))));
		}
		else
		{
			FMonolithActionResult RedirectorCleanupResult = CleanupRedirectorsForCopiedRoots(DestinationRoots, AffectedDestinationPackages, Mutation);
			if (!RedirectorCleanupResult.bSuccess)
			{
				Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("redirector_cleanup"), TEXT("failed"), false, TEXT("asset.cleanup_copied_redirectors"), RedirectorCleanupResult.ErrorMessage)));
				return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy redirector cleanup phase failed"), RedirectorCleanupResult.ErrorCode, RedirectorCleanupResult);
			}

			bool bRedirectorCleanupOk = true;
			RedirectorCleanupResult.Result->TryGetBoolField(TEXT("ok"), bRedirectorCleanupOk);
			bOk &= bRedirectorCleanupOk;
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("redirector_cleanup"),
				bRedirectorCleanupOk ? (Mutation.bDryRun ? TEXT("dry_run") : TEXT("success")) : TEXT("issues"),
				bRedirectorCleanupOk,
				TEXT("asset.cleanup_copied_redirectors"))));
			Result->SetObjectField(TEXT("redirector_cleanup_report"), RedirectorCleanupResult.Result);
		}
	}

	if (bCopyFixupValidate)
	{
		TSharedPtr<FJsonObject> ClosureParams = CloneParams(Params);
		ClosureParams->Values.Remove(TEXT("max_packages"));
		ClosureParams->SetNumberField(TEXT("max_packages"), ClosureMaxPackages);
		SetStringArrayField(ClosureParams, TEXT("destination_roots"), DestinationRoots);
		SetStringArrayField(ClosureParams, TEXT("package_paths"), AffectedDestinationPackages);
		if (!Params.IsValid() || !Params->HasField(TEXT("legacy_source_roots")))
		{
			SetStringArrayField(ClosureParams, TEXT("legacy_source_roots"), SourceRoots);
		}

		if (AffectedDestinationPackages.Num() == 0)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("closure"),
				TEXT("skipped"),
				true,
				TEXT("asset.validate_dependency_closure"),
				TEXT("no copied destination packages; collision_policy may have skipped all rows"))));
			Result->SetObjectField(TEXT("planned_closure_params"), ClosureParams);
		}
		else if (Mutation.bDryRun && !bRunClosureOnDryRun)
		{
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("closure"),
				TEXT("skipped"),
				true,
				TEXT("asset.validate_dependency_closure"),
				TEXT("dry_run does not create destination packages; set run_closure_on_dry_run=true to validate existing destinations"))));
			Result->SetObjectField(TEXT("planned_closure_params"), ClosureParams);
		}
		else
		{
			FMonolithActionResult ClosureResult = ValidateDependencyClosure(ClosureParams);
			if (!ClosureResult.bSuccess)
			{
				Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(TEXT("closure"), TEXT("failed"), false, TEXT("asset.validate_dependency_closure"), ClosureResult.ErrorMessage)));
				return FailWithPhaseReport(TEXT("copy_package_graph_with_strategy closure phase failed"), ClosureResult.ErrorCode, ClosureResult);
			}

			bool bClosureOk = true;
			ClosureResult.Result->TryGetBoolField(TEXT("ok"), bClosureOk);
			bOk &= bClosureOk;
			Phases.Add(MakeShared<FJsonValueObject>(MakePhaseRow(
				TEXT("closure"),
				bClosureOk ? TEXT("success") : TEXT("violations"),
				bClosureOk,
				TEXT("asset.validate_dependency_closure"))));
			Result->SetObjectField(TEXT("closure_report"), ClosureResult.Result);
		}
	}

	Result->SetBoolField(TEXT("dry_run"), Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Mutation.bSave);
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetStringField(TEXT("status"), Mutation.bDryRun ? TEXT("dry_run") : (bOk ? TEXT("success") : TEXT("issues")));
	Result->SetArrayField(TEXT("phases"), Phases);
	TArray<FString> NextActions;
	NextActions.Add(TEXT("asset.fixup_copied_references"));
	NextActions.Add(TEXT("asset.validate_dependency_closure"));
	NextActions.Add(TEXT("material.repair_copied_material_instance_parameters"));
	NextActions.Add(TEXT("ui.repair_slate_font_references"));
	Result->SetArrayField(TEXT("next_recommended_actions"), StringsToJson(NextActions));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::FixupCopiedReferences(const TSharedPtr<FJsonObject>& Params)
{
	FReferenceFixupOptions Options;
	FString Error;
	if (!ReadMutationOptions(Params, Options.Mutation, Error)
		|| !ReadIntParam(Params, TEXT("max_packages"), Options.MaxPackages, Error)
		|| !ReadBoolParam(Params, TEXT("require_targets"), Options.bRequireTargets, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	TArray<FRootRemap> Remaps;
	if (!ReadRootRemaps(Params, Remaps, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("root_remaps"), Error));
	}

	TArray<FString> DestinationRoots;
	TArray<FString> PackagePaths;
	if (!ReadStringArrayParam(Params, TEXT("destination_roots"), false, DestinationRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("package_paths"), false, PackagePaths, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}
	if (DestinationRoots.Num() == 0)
	{
		DestinationRoots = DestinationRootsFromRemaps(Remaps);
	}
	for (const FString& DestinationRoot : DestinationRoots)
	{
		if (!IsValidPackageOrRoot(DestinationRoot))
		{
			Error = FString::Printf(TEXT("Invalid destination root '%s'"), *DestinationRoot);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("destination_roots"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (PackagePaths.Num() == 0)
	{
		PackagePaths = ScanPackagesUnderRoots(AssetRegistry, DestinationRoots);
	}
	PackagePaths.Sort();

	const bool bTruncated = PackagePaths.Num() > Options.MaxPackages;
	if (bTruncated)
	{
		PackagePaths.SetNum(Options.MaxPackages);
	}

	auto VisitPackages = [&PackagePaths, &DestinationRoots, &Remaps](
		const FReferenceFixupOptions& VisitOptions,
		FReferenceFixupStats& VisitStats)
	{
		for (const FString& PackagePath : PackagePaths)
		{
			if (!IsUnderAnyRoot(PackagePath, DestinationRoots))
			{
				AddWarning(VisitStats, PackagePath, TEXT("Package is outside destination_roots and was skipped"));
				if (VisitOptions.Mutation.bStrict)
				{
					VisitStats.bHasBlockingErrors = true;
				}
				continue;
			}
			FixupPackageReferences(PackagePath, Remaps, VisitOptions, VisitStats);
		}
	};

	auto MakeFixupError = [&PackagePaths](const FReferenceFixupStats& ErrorStats, const TCHAR* Status)
	{
		TSharedPtr<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
		ErrorResult->SetStringField(TEXT("namespace"), TEXT("asset"));
		ErrorResult->SetStringField(TEXT("action"), TEXT("fixup_copied_references"));
		ErrorResult->SetArrayField(TEXT("checked_packages"), StringsToJson(PackagePaths));
		ErrorResult->SetArrayField(TEXT("references"), ErrorStats.References);
		ErrorResult->SetArrayField(TEXT("warnings"), ErrorStats.Warnings);
		ErrorResult->SetNumberField(TEXT("checked_package_count"), ErrorStats.CheckedPackageCount);
		ErrorResult->SetNumberField(TEXT("checked_object_count"), ErrorStats.CheckedObjectCount);
		ErrorResult->SetNumberField(TEXT("candidate_count"), ErrorStats.CandidateCount);
		ErrorResult->SetNumberField(TEXT("applied_count"), ErrorStats.AppliedCount);
		ErrorResult->SetNumberField(TEXT("warning_count"), ErrorStats.Warnings.Num());
		ErrorResult->SetNumberField(TEXT("changed_package_count"), ErrorStats.ChangedPackages.Num());
		ErrorResult->SetBoolField(TEXT("truncated"), ErrorStats.bTruncated);
		ErrorResult->SetStringField(TEXT("status"), Status);
		return ErrorResult;
	};

	if (!Options.Mutation.bDryRun && Options.Mutation.bStrict)
	{
		FReferenceFixupOptions PreflightOptions = Options;
		PreflightOptions.Mutation.bDryRun = true;
		PreflightOptions.Mutation.bSave = false;

		FReferenceFixupStats PreflightStats;
		PreflightStats.bTruncated = bTruncated;
		PreflightStats.bHasBlockingErrors = bTruncated;
		if (!bTruncated)
		{
			VisitPackages(PreflightOptions, PreflightStats);
		}

		if (PreflightStats.bHasBlockingErrors)
		{
			return FMonolithActionResult::Error(
				TEXT("fixup_copied_references strict preflight found blocking reference issues"),
				ErrInvalidParams)
				.WithErrorData(MakeFixupError(PreflightStats, TEXT("preflight_failed")));
		}
	}

	FReferenceFixupStats Stats;
	Stats.bTruncated = bTruncated;
	VisitPackages(Options, Stats);

	if (Stats.bHasBlockingErrors && !Options.Mutation.bDryRun && Options.Mutation.bStrict)
	{
		return FMonolithActionResult::Error(TEXT("fixup_copied_references found blocking reference issues"), ErrInvalidParams)
			.WithErrorData(MakeFixupError(Stats, TEXT("apply_failed")));
	}

	TArray<TSharedPtr<FJsonValue>> SavedRows;
	if (!Options.Mutation.bDryRun && Options.Mutation.bSave)
	{
		for (const FString& ChangedPackageName : Stats.ChangedPackages)
		{
			UPackage* Package = FindPackage(nullptr, *ChangedPackageName);
			if (!Package)
			{
				AddWarning(Stats, ChangedPackageName, TEXT("Changed package could not be found for save"));
				continue;
			}

			FString SavedFilename;
			FString SaveError;
			if (!SavePackageIfRequested(Package, true, SavedFilename, SaveError))
			{
				return FMonolithActionResult::Error(SaveError);
			}

			TSharedPtr<FJsonObject> SavedRow = MakeShared<FJsonObject>();
			SavedRow->SetStringField(TEXT("package_path"), ChangedPackageName);
			SavedRow->SetStringField(TEXT("filename"), SavedFilename);
			SavedRows.Add(MakeShared<FJsonValueObject>(SavedRow));
		}
	}

	TArray<TSharedPtr<FJsonValue>> RemapRows;
	for (const FRootRemap& Remap : Remaps)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("source_root"), Remap.SourceRoot);
		Row->SetStringField(TEXT("destination_root"), Remap.DestinationRoot);
		RemapRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> ChangedPackageRows;
	for (const FString& ChangedPackageName : Stats.ChangedPackages)
	{
		ChangedPackageRows.Add(MakeShared<FJsonValueString>(ChangedPackageName));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("fixup_copied_references"));
	Result->SetBoolField(TEXT("dry_run"), Options.Mutation.bDryRun);
	Result->SetBoolField(TEXT("confirmed"), Options.Mutation.bConfirm);
	Result->SetBoolField(TEXT("save"), Options.Mutation.bSave);
	Result->SetBoolField(TEXT("strict"), Options.Mutation.bStrict);
	Result->SetBoolField(TEXT("require_targets"), Options.bRequireTargets);
	Result->SetBoolField(TEXT("ok"), !Stats.bHasBlockingErrors);
	Result->SetBoolField(TEXT("truncated"), Stats.bTruncated);
	Result->SetStringField(TEXT("status"), Options.Mutation.bDryRun ? TEXT("dry_run") : TEXT("success"));
	Result->SetArrayField(TEXT("root_remaps"), RemapRows);
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("checked_packages"), StringsToJson(PackagePaths));
	Result->SetArrayField(TEXT("references"), Stats.References);
	Result->SetArrayField(TEXT("warnings"), Stats.Warnings);
	Result->SetArrayField(TEXT("changed_packages"), ChangedPackageRows);
	Result->SetArrayField(TEXT("saved_packages"), SavedRows);
	Result->SetNumberField(TEXT("checked_package_count"), Stats.CheckedPackageCount);
	Result->SetNumberField(TEXT("checked_object_count"), Stats.CheckedObjectCount);
	Result->SetNumberField(TEXT("candidate_count"), Stats.CandidateCount);
	Result->SetNumberField(TEXT("applied_count"), Stats.AppliedCount);
	Result->SetNumberField(TEXT("warning_count"), Stats.Warnings.Num());
	Result->SetNumberField(TEXT("changed_package_count"), Stats.ChangedPackages.Num());
	Result->SetNumberField(TEXT("saved_count"), SavedRows.Num());
	Result->SetStringField(TEXT("next_recommended_action"), TEXT("asset.validate_dependency_closure"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithAssetPackageGraphActions::ValidateDependencyClosure(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> DestinationRoots;
	TArray<FString> PackagePaths;
	TArray<FString> AllowedExternalRoots;
	TArray<FString> LegacySourceRoots;
	TArray<EDependencyKind> DependencyKinds;
	FString Error;

	if (!ReadStringArrayParam(Params, TEXT("destination_roots"), true, DestinationRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("package_paths"), false, PackagePaths, Error)
		|| !ReadStringArrayParam(Params, TEXT("allowed_external_roots"), false, AllowedExternalRoots, Error)
		|| !ReadStringArrayParam(Params, TEXT("legacy_source_roots"), false, LegacySourceRoots, Error)
		|| !ReadDependencyKinds(Params, true, true, DependencyKinds, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("params"), Error));
	}

	int32 MaxPackages = 1000;
	if (!ReadIntParam(Params, TEXT("max_packages"), MaxPackages, Error))
	{
		return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("max_packages"), Error));
	}

	for (const FString& DestinationRoot : DestinationRoots)
	{
		if (!IsValidPackageOrRoot(DestinationRoot))
		{
			Error = FString::Printf(TEXT("Invalid destination root '%s'"), *DestinationRoot);
			return FMonolithActionResult::Error(Error, ErrInvalidParams).WithErrorData(ErrorData(TEXT("destination_roots"), Error));
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	if (PackagePaths.Num() == 0)
	{
		PackagePaths = ScanPackagesUnderRoots(AssetRegistry, DestinationRoots);
	}

	PackagePaths.Sort();
	const bool bTruncated = PackagePaths.Num() > MaxPackages;
	if (PackagePaths.Num() > MaxPackages)
	{
		PackagePaths.SetNum(MaxPackages);
	}

	bool bOk = true;
	int32 EdgeCount = 0;
	TArray<TSharedPtr<FJsonValue>> Violations;

	for (const FString& PackagePath : PackagePaths)
	{
		if (!IsUnderAnyRoot(PackagePath, DestinationRoots))
		{
			TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
			Violation->SetStringField(TEXT("source_package"), PackagePath);
			Violation->SetStringField(TEXT("target_package"), FString());
			Violation->SetStringField(TEXT("kind"), TEXT("input"));
			Violation->SetStringField(TEXT("reason"), TEXT("package_outside_destination_roots"));
			Violations.Add(MakeShared<FJsonValueObject>(Violation));
			bOk = false;
			continue;
		}

		TArray<TPair<FString, EDependencyKind>> Dependencies;
		AppendDependencies(AssetRegistry, PackagePath, DependencyKinds, Dependencies);
		for (const TPair<FString, EDependencyKind>& Dependency : Dependencies)
		{
			++EdgeCount;
			const bool bInsideDestination = IsUnderAnyRoot(Dependency.Key, DestinationRoots);
			const bool bAllowedExternal = IsUnderAnyRoot(Dependency.Key, AllowedExternalRoots);
			const bool bLegacySource = IsUnderAnyRoot(Dependency.Key, LegacySourceRoots);
			if (!bInsideDestination && (bLegacySource || !bAllowedExternal))
			{
				TSharedPtr<FJsonObject> Violation = MakeShared<FJsonObject>();
				Violation->SetStringField(TEXT("source_package"), PackagePath);
				Violation->SetStringField(TEXT("target_package"), Dependency.Key);
				Violation->SetStringField(TEXT("kind"), DependencyKindToString(Dependency.Value));
				Violation->SetStringField(TEXT("reason"), bLegacySource ? TEXT("legacy_source_root_dependency") : TEXT("disallowed_external_dependency"));
				Violations.Add(MakeShared<FJsonValueObject>(Violation));
				bOk = false;
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("namespace"), TEXT("asset"));
	Result->SetStringField(TEXT("action"), TEXT("validate_dependency_closure"));
	Result->SetBoolField(TEXT("read_only"), true);
	// A truncated scan never examined part of the graph, so a clean prefix is not
	// proof of closure. Reporting ok=true here let copy_fixup_validate mark its
	// closure phase and the whole workflow successful over an unchecked tail.
	Result->SetBoolField(TEXT("ok"), bOk && !bTruncated);
	Result->SetBoolField(TEXT("closure_proven"), bOk && !bTruncated);
	if (bTruncated)
	{
		Result->SetStringField(
			TEXT("incomplete_reason"),
			TEXT("package scan reached max_packages; closure cannot be certified over an unchecked tail"));
	}
	Result->SetArrayField(TEXT("destination_roots"), StringsToJson(DestinationRoots));
	Result->SetArrayField(TEXT("allowed_external_roots"), StringsToJson(AllowedExternalRoots));
	Result->SetArrayField(TEXT("legacy_source_roots"), StringsToJson(LegacySourceRoots));
	Result->SetArrayField(TEXT("checked_packages"), StringsToJson(PackagePaths));
	Result->SetArrayField(TEXT("violations"), Violations);
	Result->SetNumberField(TEXT("checked_package_count"), PackagePaths.Num());
	Result->SetNumberField(TEXT("dependency_edge_count"), EdgeCount);
	Result->SetNumberField(TEXT("violation_count"), Violations.Num());
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	return FMonolithActionResult::Success(Result);
}
