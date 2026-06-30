#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

class FJsonObject;

class FMonolithBuildArtifactActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult ResolveUnrealEngine(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult RunBuildCookRun(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult PackageBuildOutputs(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult MirrorScreenshotEvidence(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult DiscordScreenshotEvidence(const TSharedPtr<FJsonObject>& Params);
};
