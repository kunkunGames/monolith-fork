#include "Actions/ProjectRiskScoreAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectRiskScoreAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Seed;
	if (Params->HasField(TEXT("asset_path")))
	{
		if (!Params->TryGetStringField(TEXT("asset_path"), Seed))
		{
			return FMonolithActionResult::Error(TEXT("'asset_path' parameter must be a string"), -32602);
		}
		if (Seed.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'asset_path' parameter cannot be empty"), -32602);
		}
	}
	else if (Params->HasField(TEXT("seed")))
	{
		if (!Params->TryGetStringField(TEXT("seed"), Seed))
		{
			return FMonolithActionResult::Error(TEXT("'seed' parameter must be a string"), -32602);
		}
		if (Seed.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'seed' parameter cannot be empty"), -32602);
		}
	}

	int32 Limit = 20;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	// Preserve the <= 0 default sentinel. RiskScore is the single owner of
	// defaulting and the effective 1..200 result cap.

	FString MinTier = TEXT("low");
	if (Params->HasField(TEXT("min_tier")))
	{
		if (!Params->TryGetStringField(TEXT("min_tier"), MinTier))
		{
			return FMonolithActionResult::Error(TEXT("'min_tier' parameter must be a string"), -32602);
		}
		if (MinTier.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("'min_tier' parameter cannot be empty"), -32602);
		}
	}

	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	TSharedPtr<FJsonObject> Result = FMonolithIndexReview::RiskScore(*Db, Seed, Limit, MinTier);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectRiskScoreAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Seed asset; omit to rank top fan-in assets"))
		.Optional(TEXT("seed"), TEXT("string"), TEXT("Alias of asset_path"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max scored assets when no seed"), TEXT("20"))
		.Optional(TEXT("min_tier"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
		.Build();
}
