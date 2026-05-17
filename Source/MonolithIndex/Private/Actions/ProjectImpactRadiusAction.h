#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.impact_radius — CRG-inspired bounded dependency traversal. */
class FProjectImpactRadiusAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("impact_radius"); }
	static FString GetDescription() { return TEXT("Bounded BFS over the asset dependency graph: who is impacted within N hops (in/out/both)"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
