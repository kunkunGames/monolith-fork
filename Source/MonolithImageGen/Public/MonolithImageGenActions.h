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

#if WITH_DEV_AUTOMATION_TESTS
	static FString TestBuildIma2RetrySignature(const TSharedPtr<FJsonObject>& Params);
	static void TestRecordIma2RateLimitCooldown(const FString& RetrySignature, double RetryAfterSeconds);
	static void TestResetIma2RateLimitCooldowns();
#endif

private:
	static FMonolithActionResult HandleListImageModels(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetImageGenerationDefaults(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGenerateImage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGenerateImageViaIma2(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleImportGeneratedImage(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult HandleGetGeneratedAssetProvenance(const TSharedPtr<FJsonObject>& Params);
};
