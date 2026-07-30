#pragma once

#include "CoreMinimal.h"

class UPackage;

namespace MonolithAINavigationPackages
{
	/**
	 * Resolve the canonical on-disk filename for a navigation-owned package.
	 * Map packages must retain the .umap extension; ordinary content packages
	 * use .uasset.
	 */
	FString ResolveSaveFilename(const UPackage* Package);
}
