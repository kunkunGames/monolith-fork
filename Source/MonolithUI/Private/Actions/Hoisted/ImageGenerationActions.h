// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

namespace MonolithUI
{
	struct FImageGenerationActions
	{
		static void Register(FMonolithToolRegistry& Registry);

		static FMonolithActionResult HandleListImageModels(const TSharedPtr<FJsonObject>& Params);
		static FMonolithActionResult HandleGetImageGenerationDefaults(const TSharedPtr<FJsonObject>& Params);
		static FMonolithActionResult HandleGenerateImage(const TSharedPtr<FJsonObject>& Params);
		static FMonolithActionResult HandleImportGeneratedImage(const TSharedPtr<FJsonObject>& Params);
		static FMonolithActionResult HandleGetGeneratedAssetProvenance(const TSharedPtr<FJsonObject>& Params);
	};
}
