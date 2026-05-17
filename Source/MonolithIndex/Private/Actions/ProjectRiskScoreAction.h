#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.risk_score — query-time risk scoring with decomposed reasons. */
class FProjectRiskScoreAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("risk_score"); }
	static FString GetDescription() { return TEXT("Score asset change risk (fan-in, hard deps, class weight, graph density) with reasons"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
