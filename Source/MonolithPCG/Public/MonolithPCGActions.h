#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;
struct FSoftObjectPath;

class FMonolithPCGActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetStatus(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListGraphAssets(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult GetGraphAsset(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult ListComponents(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RemapGraphReferences(const TSharedPtr<FJsonObject>& Params);

#if WITH_DEV_AUTOMATION_TESTS
	static bool RemapSoftObjectPathForTest(
		const FSoftObjectPath& SourcePath,
		const TMap<FString, FString>& RootRemaps,
		FSoftObjectPath& OutPath);
#endif
};
