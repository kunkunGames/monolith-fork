#include "MonolithSourceAvailability.h"

#include "Dom/JsonObject.h"

namespace
{
	TSharedPtr<FJsonObject> MakeStatusErrorData(
		const FMonolithSourceDatabaseStatus& Status,
		const FString& FailureCause)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("failure_cause"), FailureCause);
		ErrorData->SetStringField(TEXT("database_state"), Status.State);
		ErrorData->SetStringField(TEXT("database_path"), Status.DatabasePath);
		ErrorData->SetBoolField(TEXT("database_exists"), Status.bDatabaseExists);
		ErrorData->SetBoolField(TEXT("database_open"), Status.bDatabaseOpen);
		ErrorData->SetBoolField(TEXT("indexing"), Status.bIndexing);
		ErrorData->SetBoolField(
			TEXT("requires_successful_reindex"),
			Status.bRequiresSuccessfulReindex);
		ErrorData->SetNumberField(
			TEXT("last_files_processed"),
			Status.LastFilesProcessed);
		ErrorData->SetNumberField(
			TEXT("last_symbols_extracted"),
			Status.LastSymbolsExtracted);
		ErrorData->SetNumberField(TEXT("last_errors"), Status.LastErrors);
		if (!Status.LastIndexContext.IsEmpty())
		{
			ErrorData->SetStringField(
				TEXT("last_index_context"),
				Status.LastIndexContext);
		}
		if (!Status.LastFailureStage.IsEmpty())
		{
			ErrorData->SetStringField(
				TEXT("last_failure_stage"),
				Status.LastFailureStage);
		}
		if (!Status.LastFailureDetail.IsEmpty())
		{
			ErrorData->SetStringField(
				TEXT("last_failure_detail"),
				Status.LastFailureDetail);
		}
		return ErrorData;
	}

	FString DatabaseFailureCause(const FMonolithSourceDatabaseStatus& Status)
	{
		return FString::Printf(
			TEXT("source_index_%s"),
			Status.State.IsEmpty() ? TEXT("unavailable") : *Status.State);
	}
}

FMonolithActionResult MonolithSourceAvailability::MakeDatabaseUnavailableError(
	const FMonolithSourceDatabaseStatus& Status)
{
	FString Message;
	FString Hint;
	TArray<FString> RelatedActions;

	if (Status.State == TEXT("indexing"))
	{
		Message =
			TEXT("Source indexing is in progress; EngineSource.db reads are unavailable until it completes.");
		Hint =
			TEXT("Wait for the active index run to finish, then run source.health with include_deep_checks=true before retrying.");
		RelatedActions = { TEXT("source.health") };
	}
	else if (Status.State == TEXT("reindex_required"))
	{
		Message = Status.LastFailureStage.IsEmpty()
			? TEXT("The last source index run failed; EngineSource.db remains closed until a full reindex succeeds.")
			: FString::Printf(
				TEXT("The last source index run failed at '%s'; EngineSource.db remains closed until a full reindex succeeds."),
				*Status.LastFailureStage);
		Hint =
			TEXT("Run source.trigger_reindex for a clean rebuild, wait for completion, then run source.health with include_deep_checks=true.");
		RelatedActions = { TEXT("source.trigger_reindex"), TEXT("source.health") };
	}
	else if (Status.State == TEXT("missing"))
	{
		Message = TEXT("EngineSource.db does not exist.");
		Hint =
			TEXT("Run source.trigger_reindex to create the full engine and project source index, then run source.health.");
		RelatedActions = { TEXT("source.trigger_reindex"), TEXT("source.health") };
	}
	else if (Status.State == TEXT("open_failed"))
	{
		Message = TEXT("EngineSource.db exists but could not be opened.");
		Hint =
			TEXT("Inspect error.data.last_failure_detail, then run source.trigger_reindex for a clean rebuild and verify with source.health.");
		RelatedActions = { TEXT("source.trigger_reindex"), TEXT("source.health") };
	}
	else
	{
		Message = TEXT("The source index subsystem or EngineSource.db is unavailable.");
		Hint =
			TEXT("Use a live editor, inspect the structured database state, and run source.trigger_reindex if the database is missing or invalid.");
		RelatedActions = { TEXT("source.trigger_reindex"), TEXT("source.health") };
	}

	return FMonolithActionResult::Error(Message, -32000)
		.WithErrorData(MakeStatusErrorData(Status, DatabaseFailureCause(Status)))
		.WithHint(Hint)
		.WithRelatedActions(RelatedActions);
}

FMonolithActionResult MonolithSourceAvailability::MakeIndexRequestError(
	const FMonolithSourceDatabaseStatus& Status,
	const FString& RequestedMode)
{
	const FString FailureStage = Status.LastFailureStage.IsEmpty()
		? TEXT("request_rejected")
		: Status.LastFailureStage;
	const FString FailureCause = FString::Printf(
		TEXT("source_%s_index_%s"),
		*RequestedMode,
		*FailureStage);
	TSharedPtr<FJsonObject> ErrorData =
		MakeStatusErrorData(Status, FailureCause);
	ErrorData->SetStringField(TEXT("requested_mode"), RequestedMode);

	FString Hint;
	TArray<FString> RelatedActions;
	if (FailureStage == TEXT("indexing_disabled"))
	{
		Hint =
			TEXT("Run Monolith.StartIndexing in the editor console, then retry the source indexing action.");
	}
	else if (FailureStage == TEXT("database_missing")
		&& RequestedMode == TEXT("project"))
	{
		Hint =
			TEXT("Run source.trigger_reindex once to create the full database before requesting an incremental project reindex.");
		RelatedActions = { TEXT("source.trigger_reindex") };
	}
	else
	{
		Hint =
			TEXT("Inspect error.data.last_failure_detail and the editor log, then retry only after correcting the reported failure.");
		RelatedActions = { TEXT("source.health") };
	}

	return FMonolithActionResult::Error(
		FString::Printf(
			TEXT("The %s source index request failed at '%s'."),
			*RequestedMode,
			*FailureStage),
		-32000)
		.WithErrorData(ErrorData)
		.WithHint(Hint)
		.WithRelatedActions(RelatedActions);
}
