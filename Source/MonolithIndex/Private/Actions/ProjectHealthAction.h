#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.health — read-only schema/FTS/integrity diagnostics. */
class FProjectHealthAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("health"); }
	static FString GetDescription() { return TEXT("Read-only ProjectIndex diagnostics: schema v2, triggers, FTS parity, orphans, journal mode"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
