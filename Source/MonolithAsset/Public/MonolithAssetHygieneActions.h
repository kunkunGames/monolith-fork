#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Asset namespace hygiene actions.
 */
class MONOLITHASSET_API FMonolithAssetHygieneActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);
	static void RegisterValidateNamingConventions(FMonolithToolRegistry& Registry);
	static void RegisterBatchRenameAssets(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult ValidateNamingConventions(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BatchRenameAssets(const TSharedPtr<FJsonObject>& Params);
};
