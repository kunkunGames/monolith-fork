#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "UObject/Package.h"

FString FMonolithAssetUtils::ResolveAssetPath(const FString& InPath)
{
	FString Path = InPath;
	Path.TrimStartAndEndInline();

	if (Path.IsEmpty())
	{
		return Path;
	}

	// Accept Unreal copy-reference strings such as Texture2D'/Game/Foo.Foo'.
	Path = FPackageName::ExportTextPathToObjectPath(Path);

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
	// terminal except typed redirectors, which may point to the requested class.
	// Do NOT fall through to disk for other mismatches: that would silently load
	// wrong-class objects at the same path and can return stale RF_Standalone ghosts.
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

FMonolithAssetUtils::FAssetCandidateKey FMonolithAssetUtils::ParseAssetCandidateInput(const FString& Input)
{
	FAssetCandidateKey Key;

	FString Norm = Input;
	Norm.TrimStartAndEndInline();
	if (Norm.IsEmpty())
	{
		return Key;
	}

	// Unify path separators. After this, the only colon left should be a
	// drive letter (D:/...) or a SubObject delimiter; we handle both below.
	Norm.ReplaceInline(TEXT("\\"), TEXT("/"));

	// Strip Windows drive letter prefix ("D:/foo" → "/foo"). Without this,
	// the next steps treat "D:" as a path segment.
	if (Norm.Len() >= 2 && Norm[1] == TEXT(':') && FChar::IsAlpha(Norm[0]))
	{
		Norm = Norm.Mid(2);
	}

	// Strip ":SubObject" suffix. Drive-letter colon is already handled above.
	int32 ColonIdx = INDEX_NONE;
	if (Norm.FindChar(TEXT(':'), ColonIdx))
	{
		Norm = Norm.Left(ColonIdx);
	}

	// Strip .uasset / .umap (case-insensitive).
	if (Norm.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase))
	{
		Norm.LeftChopInline(7);
	}
	else if (Norm.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
	{
		Norm.LeftChopInline(5);
	}

	// Filesystem prefix up to "/Content/" → rewrite to "/Game/".
	// Handles e.g. "D:\LyraStarterGame\Content\AI\..." (already de-Windowsed)
	// and "/some/path/Content/Subdir/...".
	{
		const int32 ContentIdx = Norm.Find(TEXT("/Content/"), ESearchCase::IgnoreCase);
		if (ContentIdx != INDEX_NONE)
		{
			Norm = TEXT("/Game/") + Norm.Mid(ContentIdx + 9);
		}
	}

	// Split into non-empty path segments.
	TArray<FString> Segments;
	Norm.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);
	if (Segments.Num() == 0)
	{
		return Key;
	}

	// The last segment may be in "AssetName" form, "AssetName.AssetName" form,
	// or "PackageName.AssetName" form (FSoftObjectPath). Take the part after
	// the last '.' as the short name in all those cases.
	FString Last = Segments.Last();
	int32 LastDot = INDEX_NONE;
	if (Last.FindLastChar(TEXT('.'), LastDot))
	{
		Last = Last.Mid(LastDot + 1);
	}
	if (Last.IsEmpty())
	{
		return Key;
	}

	Key.ShortName = MoveTemp(Last);

	// Remaining segments become path hints (used to disambiguate candidates).
	for (int32 i = 0; i < Segments.Num() - 1; ++i)
	{
		Key.PathHints.Add(Segments[i]);
	}
	return Key;
}

TArray<FString> FMonolithAssetUtils::FindAssetCandidates(const FString& Input, int32 MaxResults)
{
	TArray<FString> Results;
	if (Input.IsEmpty() || MaxResults <= 0)
	{
		return Results;
	}

	const FAssetCandidateKey Key = ParseAssetCandidateInput(Input);
	if (Key.ShortName.IsEmpty())
	{
		return Results;
	}

	IAssetRegistry* AR = IAssetRegistry::Get();
	if (!AR)
	{
		return Results;
	}

	// AssetRegistry has no direct AssetName index — scan with scoring.
	// On a Lyra-sized project (~10k assets) this is well under 10ms; the call
	// only fires on the error path, so it's acceptable.
	const FName TargetName(*Key.ShortName);
	TArray<FAssetData> AllAssets;
	AR->GetAllAssets(AllAssets, /*bIncludeOnlyOnDiskAssets=*/false);

	struct FScoredCandidate { FString Path; int32 Score; };
	TArray<FScoredCandidate> Scored;
	Scored.Reserve(8);

	for (const FAssetData& Data : AllAssets)
	{
		if (Data.AssetName != TargetName)
		{
			continue;
		}

		const FString ObjectPath = Data.GetSoftObjectPath().ToString();

		// Score: how many path hints appear in the candidate's object path.
		// More hint matches → user's mistyped path was closer to this candidate.
		int32 Score = 0;
		for (const FString& Hint : Key.PathHints)
		{
			if (Hint.IsEmpty())
			{
				continue;
			}
			if (ObjectPath.Contains(Hint, ESearchCase::IgnoreCase))
			{
				Score += 10;
			}
		}

		// Tiny tiebreaker: shorter object paths beat longer ones at the same
		// score (prefer "/Game/AI/PunchBot/BB_PunchBot" over a deeper match).
		Score -= ObjectPath.Len() / 64;

		Scored.Add({ObjectPath, Score});
	}

	Scored.Sort([](const FScoredCandidate& L, const FScoredCandidate& R)
	{
		return L.Score > R.Score;
	});

	const int32 Count = FMath::Min(MaxResults, Scored.Num());
	Results.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		Results.Add(Scored[i].Path);
	}
	return Results;
}
