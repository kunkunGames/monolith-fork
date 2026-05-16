#include "Actions/ProjectRiskIndexAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectRiskIndexAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	const FString Seed = FMonolithIndexReview::PStr(Params, TEXT("asset_path"),
		FMonolithIndexReview::PStr(Params, TEXT("seed")));
	const int32 Limit = FMonolithIndexReview::PInt(Params, TEXT("limit"), 20);
	const FString MinTier = FMonolithIndexReview::PStr(Params, TEXT("min_tier"), TEXT("low"));

	TSharedPtr<FJsonObject> Result = FMonolithIndexReview::RiskIndex(*Db, Seed, Limit, MinTier);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectRiskIndexAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("asset_path"), TEXT("string"), TEXT("Seed asset; omit to rank top fan-in assets"))
		.Optional(TEXT("seed"), TEXT("string"), TEXT("Alias of asset_path"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max scored assets when no seed"), TEXT("20"))
		.Optional(TEXT("min_tier"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
		.Build();
}
