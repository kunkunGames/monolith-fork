#pragma once

#include "CoreMinimal.h"
#include "MonolithToolRegistry.h"

/** project.review_context — packaged impact + risk + details + next actions. */
class FProjectReviewContextAction
{
public:
	static FMonolithActionResult Execute(const TSharedPtr<FJsonObject>& Params);
	static FString GetName() { return TEXT("review_context"); }
	static FString GetDescription() { return TEXT("Token-efficient review package: seed + impact + risk reasons + next actions (minimal|standard)"); }
	static TSharedPtr<FJsonObject> GetSchema();
};
