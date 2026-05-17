#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.review_hotspots — global top fan/risk/size review queue. */
class FProjectReviewHotspotsAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("review_hotspots"); }
	static FString GetDescription() { return TEXT("Rank global project review hotspots by fan-in, fan-out, risk, graph size, or all signals"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
