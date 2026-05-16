#include "Actions/ProjectRepairCrgCacheAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithIndexReview.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

FMonolithActionResult FProjectRepairCrgCacheAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	UMonolithIndexSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>() : nullptr;
	FMonolithIndexDatabase* Db = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Db)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem/database not available"));
	}

	const bool bExecute = FMonolithIndexReview::PBool(Params, TEXT("execute"), false);
	const FString Scope = FMonolithIndexReview::PStr(Params, TEXT("scope"), TEXT("all"));
	if (Scope != TEXT("all"))
	{
		return FMonolithActionResult::Error(TEXT("Unsupported scope for repair_crg_cache (expected 'all')"), -32602);
	}
	if (bExecute && Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(
			TEXT("Indexing is in progress; retry repair_crg_cache(execute=true) once it completes"), -32000)
			.WithHint(TEXT("Use project.repair_crg_cache (dry_run) meanwhile, or project.health"));
	}

	return FMonolithActionResult::Success(FMonolithIndexReview::RepairCrgCache(*Db, bExecute));
}

TSharedPtr<FJsonObject> FProjectRepairCrgCacheAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("scope"), TEXT("string"), TEXT("Only 'all' is supported in this version"), TEXT("all"))
		.Optional(TEXT("execute"), TEXT("bool"), TEXT("Apply the rebuild (sole write gate). Default dry-run"), TEXT("false"))
		.Build();
}
