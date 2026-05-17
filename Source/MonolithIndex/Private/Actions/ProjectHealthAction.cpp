#include "Actions/ProjectHealthAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectHealthAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	const bool bIncludeCounts = FMonolithIndexReview::PBool(Params, TEXT("include_counts"), true);
	TSharedPtr<FJsonObject> Result = FMonolithIndexReview::Health(*Db, bIncludeCounts);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectHealthAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("include_counts"), TEXT("bool"), TEXT("Include row-count summary"), TEXT("true"))
		.Build();
}
