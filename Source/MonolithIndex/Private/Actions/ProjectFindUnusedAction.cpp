#include "Actions/ProjectFindUnusedAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectFindUnusedAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	FString Kind = TEXT("all");
	if (Params->HasField(TEXT("kind")) && !Params->TryGetStringField(TEXT("kind"), Kind))
	{
		return FMonolithActionResult::Error(TEXT("'kind' parameter must be a string"), -32602);
	}

	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->HasTypedField(TEXT("limit"), EJson::Number) || !Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	FString MinConfidence = TEXT("low");
	if (Params->HasField(TEXT("min_confidence")) && !Params->TryGetStringField(TEXT("min_confidence"), MinConfidence))
	{
		return FMonolithActionResult::Error(TEXT("'min_confidence' parameter must be a string"), -32602);
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::FindUnused(*Db, Kind, Limit, MinConfidence));
}

TSharedPtr<FJsonObject> FProjectFindUnusedAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("kind"), TEXT("string"), TEXT("Asset class filter or all"), TEXT("all"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max candidates"), TEXT("100"))
		.Optional(TEXT("min_confidence"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
		.Build();
}
