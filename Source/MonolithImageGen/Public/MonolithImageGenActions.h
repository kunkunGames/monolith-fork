// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Generated-image provider, import, and provenance actions.
 */
class MONOLITHIMAGEGEN_API FMonolithImageGenActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult HandleListImageModels(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetImageGenerationDefaults(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGenerateImage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportGeneratedImage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGeneratedAssetProvenance(const TSharedPtr<FJsonObject>& Params);
};
