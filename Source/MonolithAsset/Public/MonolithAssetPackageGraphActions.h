#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithAssetPackageGraphActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult RegisterContentMountPoints(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PlanPackageGraphCopy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CopyPackageGraphWithRemap(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult CopyPackageGraphWithStrategy(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FixupCopiedReferences(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateDependencyClosure(const TSharedPtr<FJsonObject>& Params);
};
