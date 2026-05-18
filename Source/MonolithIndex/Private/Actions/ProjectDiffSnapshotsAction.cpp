#include "Actions/ProjectDiffSnapshotsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectDiffSnapshotsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::DiffSnapshots(*Db,
		FMonolithIndexReview::PStr(Params, TEXT("before")),
		FMonolithIndexReview::PStr(Params, TEXT("after"), TEXT("current")),
		FMonolithIndexReview::PInt(Params, TEXT("limit"), 100)));
}

TSharedPtr<FJsonObject> FProjectDiffSnapshotsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("before"), TEXT("string"), TEXT("Snapshot label/id to compare from"))
		.Optional(TEXT("after"), TEXT("string"), TEXT("Snapshot label/id to compare to; defaults to current projection"), TEXT("current"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max new/removed node or edge samples per array"), TEXT("100"))
		.Build();
}
