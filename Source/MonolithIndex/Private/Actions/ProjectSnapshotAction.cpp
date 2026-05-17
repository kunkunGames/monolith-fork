#include "Actions/ProjectSnapshotAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectSnapshotAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	const bool bExecute = FMonolithIndexReview::PBool(Params, TEXT("execute"), false);
	if (bExecute && Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(
			TEXT("Indexing is in progress; retry snapshot(execute=true) once it completes"), -32000)
			.WithHint(TEXT("Use project.snapshot (dry-run) meanwhile, or project.health"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::Snapshot(*Db,
		FMonolithIndexReview::PStr(Params, TEXT("label")),
		bExecute));
}

TSharedPtr<FJsonObject> FProjectSnapshotAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("label"), TEXT("string"), TEXT("Snapshot label; defaults to project-<utc_ticks>"))
		.Optional(TEXT("execute"), TEXT("bool"), TEXT("Store the snapshot (sole write gate). Default dry-run"), TEXT("false"))
		.Build();
}
