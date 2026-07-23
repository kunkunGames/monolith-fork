#include "Actions/ProjectReviewHotspotsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectReviewHotspotsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Kind = TEXT("all");
	if (Params->HasField(TEXT("kind")) && !Params->TryGetStringField(TEXT("kind"), Kind))
	{
		return FMonolithActionResult::Error(TEXT("'kind' parameter must be a string"), -32602);
	}

	int32 Limit = 50;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	// Preserve the <= 0 default sentinel. ReviewHotspots is the single owner of
	// defaulting and the effective 1..200 result cap.

	int32 MinLines = 100;
	if (Params->HasField(TEXT("min_lines")))
	{
		double MinLinesValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("min_lines"), MinLinesValue))
		{
			return FMonolithActionResult::Error(TEXT("'min_lines' parameter must be a number"), -32602);
		}
		MinLines = static_cast<int32>(MinLinesValue);
	}
	// Preserve the <= 0 default sentinel. ReviewHotspots owns the effective floor.

	bool bIncludeQuestions = true;
	if (Params->HasField(TEXT("include_questions")))
	{
		if (!Params->TryGetBoolField(TEXT("include_questions"), bIncludeQuestions))
		{
			return FMonolithActionResult::Error(TEXT("'include_questions' parameter must be a bool"), -32602);
		}
	}

	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::ReviewHotspots(*Db,
		Kind, Limit, MinLines, bIncludeQuestions));
}

TSharedPtr<FJsonObject> FProjectReviewHotspotsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("kind"), TEXT("string"), TEXT("fan_in|fan_out|risk|large|all"), TEXT("all"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max hotspots"), TEXT("50"))
		.Optional(TEXT("min_lines"), TEXT("integer"), TEXT("Large-graph signal floor"), TEXT("100"))
		.Optional(TEXT("include_questions"), TEXT("bool"), TEXT("Add advisory review questions"), TEXT("true"))
		.Build();
}
