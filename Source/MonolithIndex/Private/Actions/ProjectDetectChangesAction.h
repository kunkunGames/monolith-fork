#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.detect_changes — changed asset impact and review priority triage. */
class FProjectDetectChangesAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("detect_changes"); }
	static FString GetDescription() { return TEXT("Map changed asset paths to indexed assets, direct impact, and risk-ranked review priorities"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
