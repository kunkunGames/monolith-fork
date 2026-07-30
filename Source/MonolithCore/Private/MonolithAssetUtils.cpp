#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"

bool FMonolithAssetUtils::IsProjectOwnedPackage(const FString& PackageName)
{
	if (!FPackageName::IsValidLongPackageName(PackageName, false))
	{
		return false;
	}

	FString PackageFilename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			PackageFilename,
			FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}

	PackageFilename = FPaths::ConvertRelativePathToFull(PackageFilename);
	FPaths::NormalizeFilename(PackageFilename);
	FString ProjectDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeDirectoryName(ProjectDirectory);
	return FPaths::IsUnderDirectory(PackageFilename, ProjectDirectory);
}

FString FMonolithAssetUtils::ResolveAssetPath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();

	// Normalize backslashes
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Handle /Content/ → /Game/
	if (Path.StartsWith(TEXT("/Content/")))
	{
		Path = TEXT("/Game/") + Path.Mid(9);
	}
	else if (!Path.StartsWith(TEXT("/")))
	{
		// Relative path — assume /Game/
		Path = TEXT("/Game/") + Path;
	}

	// Strip extension if present
	if (Path.EndsWith(TEXT(".uasset")) || Path.EndsWith(TEXT(".umap")))
	{
		Path = FPaths::GetBaseFilename(Path, false);
	}

	return Path;
}

UPackage* FMonolithAssetUtils::LoadPackageByPath(const FString& AssetPath)
{
	FString Resolved = ResolveAssetPath(AssetPath);
	UPackage* Package = LoadPackage(nullptr, *Resolved, LOAD_None);
	if (!Package)
	{
		UE_LOG(LogMonolith, Warning, TEXT("Failed to load package: %s"), *Resolved);
	}
	return Package;
}

UObject* FMonolithAssetUtils::LoadAssetByPath(const FString& AssetPath)
{
	// Single-arg overload preserves prior behaviour: load as UObject (no class check).
	return LoadAssetByPath(UObject::StaticClass(), AssetPath);
}

UObject* FMonolithAssetUtils::LoadAssetByPath(UClass* ExpectedClass, const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}

	UClass* LookupClass = ExpectedClass ? ExpectedClass : UObject::StaticClass();
	const bool bResolveTypedRedirectors = ExpectedClass && ExpectedClass != UObject::StaticClass();
	auto TryResolveTypedRedirector = [ExpectedClass, bResolveTypedRedirectors](UObject* Object) -> UObject*
	{
		if (!bResolveTypedRedirectors)
		{
			return nullptr;
		}

		if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Object))
		{
			UObject* Destination = Redirector->DestinationObject;
			if (Destination && Destination->IsA(ExpectedClass))
			{
				return Destination;
			}
		}

		return nullptr;
	};

	// -------------------------------------------------------------------------
	// Tier 1: Normalize. ResolveAssetPath handles /Content/->/Game/, relative
	// paths, and strips .uasset/.umap. Then split into Package.AssetName.
	// Strip optional ":SubObject" suffix for FindObject path.
	// -------------------------------------------------------------------------
	FString NormalizedFull = ResolveAssetPath(AssetPath);

	FString PackagePath = NormalizedFull;
	FString AssetName;
	int32 LastDot = INDEX_NONE;
	if (NormalizedFull.FindLastChar('.', LastDot))
	{
		PackagePath = NormalizedFull.Left(LastDot);
		AssetName = NormalizedFull.Mid(LastDot + 1);
	}
	else
	{
		// Bare /Game/Path/AssetName -> use last path segment as AssetName.
		int32 LastSlash = INDEX_NONE;
		if (NormalizedFull.FindLastChar('/', LastSlash))
		{
			AssetName = NormalizedFull.Mid(LastSlash + 1);
		}
		PackagePath = NormalizedFull;
		NormalizedFull = PackagePath + TEXT(".") + AssetName;
	}

	// Strip ":SubObject" portion (if any) from AssetName for FindObject.
	FString AssetNameNoSub = AssetName;
	int32 ColonIdx = INDEX_NONE;
	if (AssetNameNoSub.FindChar(':', ColonIdx))
	{
		AssetNameNoSub = AssetNameNoSub.Left(ColonIdx);
	}

	// -------------------------------------------------------------------------
	// Tier 2: AssetRegistry — authoritative for class match. Class mismatch is
	// terminal (do NOT fall through to disk — would silently load wrong-class
	// object at the same path). Reflects the editor's current ground truth and
	// avoids stale RF_Standalone ghosts that StaticLoadObject can return.
	// -------------------------------------------------------------------------
	if (IAssetRegistry* AR = IAssetRegistry::Get())
	{
		FAssetData Data = AR->GetAssetByObjectPath(FSoftObjectPath(NormalizedFull));
		if (Data.IsValid())
		{
			if (UObject* Loaded = Data.GetAsset())
			{
				if (!ExpectedClass || Loaded->IsA(ExpectedClass))
				{
					return Loaded;
				}
				if (UObject* Redirected = TryResolveTypedRedirector(Loaded))
				{
					return Redirected;
				}
				// Class mismatch in registry — authoritative, do NOT fall through.
				return nullptr;
			}
		}
	}

	// -------------------------------------------------------------------------
	// Tier 3: FindPackage + FindObject — catches freshly-created unsaved assets
	// in this session (already in memory, not yet on disk / not yet in registry).
	// Prefer FindPackage over LoadPackage here — load-from-disk happens in tier 4.
	// -------------------------------------------------------------------------
	if (UPackage* Pkg = FindPackage(nullptr, *PackagePath))
	{
		if (UObject* Found = FindObject<UObject>(Pkg, *AssetNameNoSub))
		{
			if (!ExpectedClass || Found->IsA(ExpectedClass))
			{
				return Found;
			}
			if (UObject* Redirected = TryResolveTypedRedirector(Found))
			{
				return Redirected;
			}
			// In-memory class mismatch is also terminal.
			return nullptr;
		}
	}

	// -------------------------------------------------------------------------
	// Tier 4: StaticLoadObject — disk fallback.
	// If class-typed call fails AND ExpectedClass != UObject, retry with UObject
	// (some package layouts only resolve via UObject), then class-check the result.
	// -------------------------------------------------------------------------
	if (UObject* DiskObj = StaticLoadObject(LookupClass, nullptr, *NormalizedFull))
	{
		if (!ExpectedClass || DiskObj->IsA(ExpectedClass))
		{
			return DiskObj;
		}
		if (UObject* Redirected = TryResolveTypedRedirector(DiskObj))
		{
			return Redirected;
		}
	}

	if (ExpectedClass && ExpectedClass != UObject::StaticClass())
	{
		if (UObject* DiskObj2 = StaticLoadObject(UObject::StaticClass(), nullptr, *NormalizedFull))
		{
			if (DiskObj2->IsA(ExpectedClass))
			{
				return DiskObj2;
			}
			if (UObject* Redirected = TryResolveTypedRedirector(DiskObj2))
			{
				return Redirected;
			}
		}
	}

	UE_LOG(LogMonolith, Warning, TEXT("Failed to load asset: %s (tried: %s)"), *AssetPath, *NormalizedFull);
	return nullptr;
}

bool FMonolithAssetUtils::TryLoadAssetByPath(
	UClass* ExpectedClass,
	const FString& AssetPath,
	UObject*& OutAsset,
	FString& OutResolvedPath,
	FString& OutError)
{
	OutAsset = nullptr;
	OutResolvedPath = ResolveAssetPath(AssetPath);
	OutError.Reset();

	if (OutResolvedPath.IsEmpty())
	{
		OutError = TEXT("Asset path must be a non-empty string");
		return false;
	}

	UClass* EffectiveClass = ExpectedClass ? ExpectedClass : UObject::StaticClass();
	OutAsset = LoadAssetByPath(EffectiveClass, OutResolvedPath);
	if (!OutAsset)
	{
		OutError = FString::Printf(
			TEXT("Failed to load asset '%s' as %s"),
			*OutResolvedPath,
			*EffectiveClass->GetName());
		return false;
	}

	return true;
}

bool FMonolithAssetUtils::AssetExists(const FString& AssetPath)
{
	FString Resolved = ResolveAssetPath(AssetPath);
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Resolved));
	return AssetData.IsValid();
}

TArray<FAssetData> FMonolithAssetUtils::GetAssetsByClass(const FTopLevelAssetPath& ClassPath, const FString& PackagePath)
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(ClassPath);
	if (!PackagePath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*PackagePath));
		Filter.bRecursivePaths = true;
	}

	TArray<FAssetData> Results;
	AssetRegistry.GetAssets(Filter, Results);
	return Results;
}

FString FMonolithAssetUtils::GetAssetName(const FString& AssetPath)
{
	return FPackageName::GetShortName(AssetPath);
}
