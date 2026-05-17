#include "Actions/ProjectReviewHotspotsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectReviewHotspotsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::ReviewHotspots(*Db,
		FMonolithIndexReview::PStr(Params, TEXT("kind"), TEXT("all")),
		FMonolithIndexReview::PInt(Params, TEXT("limit"), 50),
		FMonolithIndexReview::PInt(Params, TEXT("min_lines"), 100),
		FMonolithIndexReview::PBool(Params, TEXT("include_questions"), true)));
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
