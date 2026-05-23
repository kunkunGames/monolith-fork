#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FMonolithSpriteActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateAssetSpec(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateGuides(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildCandidatePlan(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PrepareImageGenRequests(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RunGenerationBatch(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ValidateSheet(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ExportMetadata(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult BuildPreviewContactSheet(const TSharedPtr<FJsonObject>& Params);
};
