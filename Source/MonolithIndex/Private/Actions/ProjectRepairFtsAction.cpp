#include "Actions/ProjectRepairFtsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

namespace
{
bool IsSupportedRepairTarget(const FString& Target)
{
	return Target == TEXT("all")
		|| Target == TEXT("assets")
		|| Target == TEXT("nodes");
}
}

FMonolithActionResult FProjectRepairFtsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Target = TEXT("all");
	if (Params.IsValid() && Params->HasField(TEXT("target"))
		&& !Params->TryGetStringField(TEXT("target"), Target))
	{
		return FMonolithActionResult::Error(TEXT("'target' must be a string"), -32602);
	}
	Target = Target.TrimStartAndEnd().ToLower();
	if (!IsSupportedRepairTarget(Target))
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Unknown FTS target '%s'; expected all, assets, or nodes"),
				*Target),
			-32602);
	}

	bool bExecute = false;
	if (Params.IsValid() && Params->HasField(TEXT("execute"))
		&& !Params->TryGetBoolField(TEXT("execute"), bExecute))
	{
		return FMonolithActionResult::Error(TEXT("'execute' must be a boolean"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>()
		: nullptr;
	FMonolithIndexDatabase* Database = Subsystem ? Subsystem->GetDatabase() : nullptr;
	if (!Database)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}
	if (bExecute && Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(
			TEXT("Cannot rebuild FTS indexes while project indexing is in progress"));
	}

	TSharedPtr<FJsonObject> Report;
	FString Error;
	if (!bExecute)
	{
		if (!Database->RepairFullTextIndexes(Target, false, Report, Error))
		{
			return FMonolithActionResult::Error(Error);
		}
		return FMonolithActionResult::Success(Report);
	}

	if (!Database->BeginTransaction())
	{
		return FMonolithActionResult::Error(TEXT("Failed to begin FTS repair transaction"));
	}
	if (!Database->RepairFullTextIndexes(Target, true, Report, Error)
		|| !Database->CommitTransaction())
	{
		Database->RollbackTransaction();
		return FMonolithActionResult::Error(
			Error.IsEmpty() ? TEXT("Failed to commit FTS repair transaction") : Error);
	}
	return FMonolithActionResult::Success(Report);
}

TSharedPtr<FJsonObject> FProjectRepairFtsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(
			TEXT("target"),
			TEXT("string"),
			TEXT("all, assets, or nodes"),
			TEXT("all"))
		.Optional(
			TEXT("execute"),
			TEXT("boolean"),
			TEXT("False reports the rebuild plan; true performs it"),
			TEXT("false"))
		.Build();
}
