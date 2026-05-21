#include "Actions/ProjectRepairFtsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectRepairFtsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	const FString Target = FMonolithIndexReview::PStr(Params, TEXT("target"), TEXT("all"));
	const bool bExecute = FMonolithIndexReview::PBool(Params, TEXT("execute"), false);

	// execute=true is the sole write gate; refuse while a reindex is in flight so
	// the rebuild does not race the indexer's writes.
	if (bExecute && Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(
			TEXT("Indexing is in progress; retry repair_fts(execute=true) once it completes"), -32000)
			.WithHint(TEXT("Use project.repair_fts (dry_run) meanwhile, or project.health"));
	}

	TSharedPtr<FJsonObject> Result = FMonolithIndexReview::RepairFts(*Db, Target, bExecute);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectRepairFtsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("target"), TEXT("string"), TEXT("all|assets|nodes|variables|parameters|datatable_rows|actors|asset_search_values"), TEXT("all"))
		.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply the rebuild (sole write gate). Default dry-run"), TEXT("false"))
		.Build();
}
