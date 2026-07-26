#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** Read-only access to the live IConsoleManager variable registry. */
class FMonolithCVarActions
{
public:
	static void RegisterActions(FMonolithToolRegistry& Registry);

	static FMonolithActionResult GetCVar(const TSharedPtr<FJsonObject>& Params);
	static FMonolithActionResult FindCVars(const TSharedPtr<FJsonObject>& Params);
};
