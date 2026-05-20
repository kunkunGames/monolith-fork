#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/**
 * Composition and encounter review actions for level design.
 */
class MONOLITHLEVELDESIGN_API FMonolithLevelDesignQualityActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

private:
	static FMonolithActionResult AnalyzeFraming(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult EvaluateMonsterReveal(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult AnalyzeCoOpBalance(const TSharedPtr<FJsonObject>& Params);
};
