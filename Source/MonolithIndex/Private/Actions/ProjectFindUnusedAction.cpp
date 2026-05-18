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

	return FMonolithActionResult::Success(FMonolithIndexReview::FindUnused(*Db,
		FMonolithIndexReview::PStr(Params, TEXT("kind"), TEXT("all")),
		FMonolithIndexReview::PInt(Params, TEXT("limit"), 100),
		FMonolithIndexReview::PStr(Params, TEXT("min_confidence"), TEXT("low"))));
}

TSharedPtr<FJsonObject> FProjectFindUnusedAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("kind"), TEXT("string"), TEXT("Asset class filter or all"), TEXT("all"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max candidates"), TEXT("100"))
		.Optional(TEXT("min_confidence"), TEXT("string"), TEXT("low|medium|high filter"), TEXT("low"))
		.Build();
}
