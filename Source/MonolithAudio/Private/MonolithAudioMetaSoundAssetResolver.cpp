#include "MonolithAudioMetaSoundAssetResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithAudio::MetaSoundAssetResolver
{
	bool ResolveAssetPath(
		const FString& AssetPath,
		FResolvedAssetPath& OutResolved,
		FString& OutError)
	{
		OutResolved = {};
		OutError.Reset();

		FString Normalized = AssetPath.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Normalized.Contains(TEXT("'")))
		{
			Normalized = FPackageName::ExportTextPathToObjectPath(Normalized);
		}

		int32 LastSlash = INDEX_NONE;
		if (!Normalized.StartsWith(TEXT("/"))
			|| !Normalized.FindLastChar(TEXT('/'), LastSlash)
			|| LastSlash <= 0
			|| LastSlash == Normalized.Len() - 1)
		{
			OutError = FString::Printf(
				TEXT("Invalid MetaSound asset path '%s'; expected /Root/Folder/Asset or /Root/Folder/Asset.Asset"),
				*AssetPath);
			return false;
		}

		int32 LastDot = INDEX_NONE;
		Normalized.FindLastChar(TEXT('.'), LastDot);
		if (LastDot > LastSlash)
		{
			OutResolved.PackagePath = Normalized.Left(LastDot);
			OutResolved.AssetName = Normalized.Mid(LastDot + 1);
		}
		else
		{
			OutResolved.PackagePath = Normalized;
			OutResolved.AssetName = Normalized.Mid(LastSlash + 1);
		}

		if (!FPackageName::IsValidLongPackageName(OutResolved.PackagePath, false)
			|| OutResolved.AssetName.IsEmpty()
			|| OutResolved.AssetName.Contains(TEXT("/"))
			|| OutResolved.AssetName.Contains(TEXT(".")))
		{
			OutError = FString::Printf(TEXT("Invalid MetaSound asset path '%s'"), *AssetPath);
			OutResolved = {};
			return false;
		}

		OutResolved.ObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*OutResolved.PackagePath,
			*OutResolved.AssetName);
		return true;
	}

	UObject* LoadMetaSoundAsset(const FString& AssetPath, FString& OutError)
	{
		FResolvedAssetPath Resolved;
		if (!ResolveAssetPath(AssetPath, Resolved, OutError))
		{
			return nullptr;
		}

		if (UObject* Existing = StaticFindObject(
			UObject::StaticClass(),
			nullptr,
			*Resolved.ObjectPath))
		{
			return Existing;
		}

		IAssetRegistry& AssetRegistry = IAssetRegistry::GetChecked();
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(Resolved.ObjectPath));
		if (!AssetData.IsValid())
		{
			TArray<FAssetData> PackageAssets;
			AssetRegistry.GetAssetsByPackageName(
				FName(*Resolved.PackagePath),
				PackageAssets,
				/*bIncludeOnlyOnDiskAssets=*/ false);
			for (const FAssetData& Candidate : PackageAssets)
			{
				if (Candidate.AssetName == FName(*Resolved.AssetName))
				{
					AssetData = Candidate;
					break;
				}
			}
		}

		if (AssetData.IsValid())
		{
			if (UObject* Loaded = AssetData.GetAsset())
			{
				return Loaded;
			}
		}

		if (UObject* Loaded = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*Resolved.ObjectPath))
		{
			return Loaded;
		}

		OutError = FString::Printf(
			TEXT("MetaSound asset not found at '%s' (canonical object path '%s')"),
			*AssetPath,
			*Resolved.ObjectPath);
		return nullptr;
	}
}
