#pragma once

#include "CoreMinimal.h"

class UObject;

namespace MonolithAudio::MetaSoundAssetResolver
{
	struct FResolvedAssetPath
	{
		FString PackagePath;
		FString ObjectPath;
		FString AssetName;
	};

	/** Resolve package-only, object, or export-text syntax to one canonical MetaSound object path. */
	bool ResolveAssetPath(
		const FString& AssetPath,
		FResolvedAssetPath& OutResolved,
		FString& OutError);

	/** Load the exact UObject without depending on transient AssetRegistry object-path aliases. */
	UObject* LoadMetaSoundAsset(const FString& AssetPath, FString& OutError);
}
