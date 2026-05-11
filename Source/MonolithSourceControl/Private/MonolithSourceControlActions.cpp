#include "MonolithSourceControlActions.h"

#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"

namespace
{
	const TCHAR* CommandResultToString(ECommandResult::Type Result)
	{
		switch (Result)
		{
		case ECommandResult::Succeeded:
			return TEXT("succeeded");
		case ECommandResult::Failed:
			return TEXT("failed");
		case ECommandResult::Cancelled:
			return TEXT("cancelled");
		default:
			return TEXT("unknown");
		}
	}

	TSharedPtr<FJsonObject> ProviderToJson(ISourceControlProvider& Provider)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Provider.GetName().ToString());
		Obj->SetBoolField(TEXT("enabled"), Provider.IsEnabled());
		Obj->SetBoolField(TEXT("available"), Provider.IsAvailable());
		Obj->SetStringField(TEXT("status_text"), Provider.GetStatusText().ToString());
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeUnavailableResult(ISourceControlProvider& Provider, const FString& Message)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), false);
		Result->SetBoolField(TEXT("available"), false);
		Result->SetStringField(TEXT("message"), Message);
		Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
		return Result;
	}

	bool NormalizeSourceControlPath(const FString& Input, FString& OutFile, FString& OutError)
	{
		FString Path = Input.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("empty path");
			return false;
		}

		if (Path.StartsWith(TEXT("/")))
		{
			FString PackageName = Path.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Path)
				: Path;

			if (FPackageName::IsValidLongPackageName(PackageName, false))
			{
				if (FPackageName::DoesPackageExist(PackageName, &OutFile))
				{
					OutFile = FPaths::ConvertRelativePathToFull(OutFile);
					FPaths::NormalizeFilename(OutFile);
					return true;
				}

				if (FPackageName::TryConvertLongPackageNameToFilename(
					PackageName,
					OutFile,
					FPackageName::GetAssetPackageExtension()))
				{
					OutFile = FPaths::ConvertRelativePathToFull(OutFile);
					FPaths::NormalizeFilename(OutFile);
					return true;
				}

				OutError = FString::Printf(TEXT("could not resolve package path: %s"), *Input);
				return false;
			}

			OutFile = FPaths::ConvertRelativePathToFull(Path);
			FPaths::NormalizeFilename(OutFile);
			return true;
		}

		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::Combine(FPaths::ProjectDir(), Path);
		}

		OutFile = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(OutFile);
		return true;
	}

	bool ReadPathArray(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutInputs, TArray<FString>& OutFiles, TArray<TSharedPtr<FJsonValue>>& OutRows, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("paths"), PathValues) || !PathValues || PathValues->Num() == 0)
		{
			OutError = TEXT("Required parameter: paths (non-empty string array)");
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PathValues)
		{
			FString Input;
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			if (!Value.IsValid() || !Value->TryGetString(Input) || Input.IsEmpty())
			{
				Row->SetBoolField(TEXT("valid"), false);
				Row->SetStringField(TEXT("error"), TEXT("path entry is not a non-empty string"));
				OutRows.Add(MakeShared<FJsonValueObject>(Row));
				continue;
			}

			FString File;
			FString Error;
			Row->SetStringField(TEXT("input"), Input);
			if (NormalizeSourceControlPath(Input, File, Error))
			{
				Row->SetBoolField(TEXT("valid"), true);
				Row->SetStringField(TEXT("file"), File);
				OutInputs.Add(Input);
				OutFiles.Add(File);
			}
			else
			{
				Row->SetBoolField(TEXT("valid"), false);
				Row->SetStringField(TEXT("error"), Error);
			}
			OutRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		if (OutFiles.Num() == 0)
		{
			OutError = TEXT("No valid paths were supplied.");
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> StateToJson(const FString& File, const FSourceControlStatePtr& State)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("file"), File);
		Obj->SetBoolField(TEXT("state_known"), State.IsValid());
		if (!State.IsValid())
		{
			return Obj;
		}

		Obj->SetStringField(TEXT("display_name"), State->GetDisplayName().ToString());
		Obj->SetStringField(TEXT("tooltip"), State->GetDisplayTooltip().ToString());
		Obj->SetBoolField(TEXT("source_controlled"), State->IsSourceControlled());
		Obj->SetBoolField(TEXT("current"), State->IsCurrent());
		Obj->SetBoolField(TEXT("checked_out"), State->IsCheckedOut());
		Obj->SetBoolField(TEXT("added"), State->IsAdded());
		Obj->SetBoolField(TEXT("deleted"), State->IsDeleted());
		Obj->SetBoolField(TEXT("ignored"), State->IsIgnored());
		Obj->SetBoolField(TEXT("modified"), State->IsModified());
		Obj->SetBoolField(TEXT("conflicted"), State->IsConflicted());
		Obj->SetBoolField(TEXT("can_checkout"), State->CanCheckout());
		Obj->SetBoolField(TEXT("can_add"), State->CanAdd());
		Obj->SetBoolField(TEXT("can_revert"), State->CanRevert());
		Obj->SetBoolField(TEXT("can_edit"), State->CanEdit());
		Obj->SetBoolField(TEXT("can_delete"), State->CanDelete());

		FString OtherUser;
		if (State->IsCheckedOutOther(&OtherUser))
		{
			Obj->SetBoolField(TEXT("checked_out_other"), true);
			Obj->SetStringField(TEXT("checked_out_other_user"), OtherUser);
		}
		else
		{
			Obj->SetBoolField(TEXT("checked_out_other"), false);
		}

		TOptional<FText> StatusText = State->GetStatusText();
		if (StatusText.IsSet())
		{
			Obj->SetStringField(TEXT("status_text"), StatusText.GetValue().ToString());
		}

		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> GetStateRows(ISourceControlProvider& Provider, const TArray<FString>& Files)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Files.Num());
		for (const FString& File : Files)
		{
			Rows.Add(MakeShared<FJsonValueObject>(
				StateToJson(File, Provider.GetState(File, EStateCacheUsage::ForceUpdate))));
		}
		return Rows;
	}

	void AddOperationMessages(const FSourceControlOperationRef& Operation, TSharedPtr<FJsonObject>& Result)
	{
		const FSourceControlResultInfo& Info = Operation->GetResultInfo();

		TArray<TSharedPtr<FJsonValue>> ErrorRows;
		for (const FText& Error : Info.ErrorMessages)
		{
			ErrorRows.Add(MakeShared<FJsonValueString>(Error.ToString()));
		}
		Result->SetArrayField(TEXT("errors"), ErrorRows);

		TArray<TSharedPtr<FJsonValue>> InfoRows;
		for (const FText& Message : Info.InfoMessages)
		{
			InfoRows.Add(MakeShared<FJsonValueString>(Message.ToString()));
		}
		Result->SetArrayField(TEXT("messages"), InfoRows);
	}

	FMonolithActionResult ExecuteFileOperation(
		const TSharedPtr<FJsonObject>& Params,
		const FString& OperationName,
		const FSourceControlOperationRef& Operation,
		bool bRequiresConfirm)
	{
		TArray<FString> Inputs;
		TArray<FString> Files;
		TArray<TSharedPtr<FJsonValue>> PathRows;
		FString Error;
		if (!ReadPathArray(Params, Inputs, Files, PathRows, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		bool bDryRun = false;
		bool bConfirm = false;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
			Params->TryGetBoolField(TEXT("confirm"), bConfirm);
		}

		ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
		if (!Provider.IsEnabled() || !Provider.IsAvailable())
		{
			return FMonolithActionResult::Success(MakeUnavailableResult(
				Provider,
				TEXT("Source control provider is disabled or unavailable.")));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("operation"), OperationName);
		Result->SetBoolField(TEXT("dry_run"), bDryRun);
		Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
		Result->SetArrayField(TEXT("paths"), PathRows);

		if (bRequiresConfirm && !bDryRun && !bConfirm)
		{
			Result->SetBoolField(TEXT("ok"), false);
			Result->SetStringField(TEXT("message"), TEXT("This operation requires confirm=true or dry_run=true."));
			return FMonolithActionResult::Success(Result);
		}

		if (bDryRun)
		{
			Result->SetBoolField(TEXT("ok"), true);
			Result->SetArrayField(TEXT("states"), GetStateRows(Provider, Files));
			return FMonolithActionResult::Success(Result);
		}

		const ECommandResult::Type CommandResult = Provider.Execute(Operation, Files, EConcurrency::Synchronous);
		Result->SetBoolField(TEXT("ok"), CommandResult == ECommandResult::Succeeded);
		Result->SetStringField(TEXT("command_result"), CommandResultToString(CommandResult));
		AddOperationMessages(Operation, Result);
		Result->SetArrayField(TEXT("states"), GetStateRows(Provider, Files));
		return FMonolithActionResult::Success(Result);
	}
}

void FMonolithSourceControlActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("source_control"), TEXT("get_capabilities"),
		TEXT("Return the active Unreal source-control provider and Phase 1 Monolith action capabilities."),
		FMonolithActionHandler::CreateStatic(&HandleGetCapabilities),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("source_control"), TEXT("get_status"),
		TEXT("Return source-control status for filesystem or /Game package paths."),
		FMonolithActionHandler::CreateStatic(&HandleGetStatus),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout"),
		TEXT("Check out files through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleCheckout),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("add"),
		TEXT("Mark files for add through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleAdd),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout_or_add"),
		TEXT("Prepare files for mutation by checking out existing source-controlled files or adding local files."),
		FMonolithActionHandler::CreateStatic(&HandleCheckoutOrAdd),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview actions without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert"),
		TEXT("Revert files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevert),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert_unchanged"),
		TEXT("Revert unchanged files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevertUnchanged),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert unchanged"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithSourceControlActions::HandleGetCapabilities(const TSharedPtr<FJsonObject>& /*Params*/)
{
	ISourceControlModule& Module = ISourceControlModule::Get();
	ISourceControlProvider& Provider = Module.GetProvider();

	TArray<FName> ProviderNames;
	Module.GetProviderNames(ProviderNames);
	TArray<TSharedPtr<FJsonValue>> ProvidersJson;
	for (const FName& Name : ProviderNames)
	{
		ProvidersJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
	}

	TArray<TSharedPtr<FJsonValue>> ActionsJson;
	for (const FString& Name : {
		TEXT("get_capabilities"),
		TEXT("get_status"),
		TEXT("checkout"),
		TEXT("add"),
		TEXT("checkout_or_add"),
		TEXT("revert"),
		TEXT("revert_unchanged") })
	{
		ActionsJson.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
	Result->SetBoolField(TEXT("enabled"), Module.IsEnabled());
	Result->SetArrayField(TEXT("available_providers"), ProvidersJson);
	Result->SetArrayField(TEXT("phase1_actions"), ActionsJson);
	Result->SetBoolField(TEXT("supports_changelists"), false);
	Result->SetBoolField(TEXT("supports_shelving"), false);
	Result->SetStringField(TEXT("note"), TEXT("Phase 1 covers provider status and safe file prepare/revert actions only."));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleGetStatus(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Inputs;
	TArray<FString> Files;
	TArray<TSharedPtr<FJsonValue>> PathRows;
	FString Error;
	if (!ReadPathArray(Params, Inputs, Files, PathRows, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
	Result->SetArrayField(TEXT("paths"), PathRows);
	Result->SetBoolField(TEXT("available"), Provider.IsEnabled() && Provider.IsAvailable());
	if (Provider.IsEnabled() && Provider.IsAvailable())
	{
		Result->SetArrayField(TEXT("states"), GetStateRows(Provider, Files));
	}
	else
	{
		Result->SetStringField(TEXT("message"), TEXT("Source control provider is disabled or unavailable."));
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleCheckout(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("checkout"), ISourceControlOperation::Create<FCheckOut>(), false);
}

FMonolithActionResult FMonolithSourceControlActions::HandleAdd(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("add"), ISourceControlOperation::Create<FMarkForAdd>(), false);
}

FMonolithActionResult FMonolithSourceControlActions::HandleCheckoutOrAdd(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> Inputs;
	TArray<FString> Files;
	TArray<TSharedPtr<FJsonValue>> PathRows;
	FString Error;
	if (!ReadPathArray(Params, Inputs, Files, PathRows, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bDryRun = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	if (!Provider.IsEnabled() || !Provider.IsAvailable())
	{
		return FMonolithActionResult::Success(MakeUnavailableResult(
			Provider,
			TEXT("Source control provider is disabled or unavailable.")));
	}

	TArray<FString> FilesToCheckout;
	TArray<FString> FilesToAdd;
	TArray<TSharedPtr<FJsonValue>> Decisions;
	for (const FString& File : Files)
	{
		FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::ForceUpdate);
		TSharedPtr<FJsonObject> Decision = StateToJson(File, State);

		if (State.IsValid() && (State->IsCheckedOut() || State->IsAdded()))
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("skip"));
			Decision->SetStringField(TEXT("reason"), TEXT("already checked out or added"));
		}
		else if (!State.IsValid() || !State->IsSourceControlled() || State->CanAdd())
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("add"));
			FilesToAdd.Add(File);
		}
		else if (State->CanCheckout())
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("checkout"));
			FilesToCheckout.Add(File);
		}
		else
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("skip"));
			Decision->SetStringField(TEXT("reason"), TEXT("provider state cannot be added or checked out"));
		}

		Decisions.Add(MakeShared<FJsonValueObject>(Decision));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("operation"), TEXT("checkout_or_add"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
	Result->SetArrayField(TEXT("paths"), PathRows);
	Result->SetArrayField(TEXT("decisions"), Decisions);
	Result->SetNumberField(TEXT("checkout_count"), FilesToCheckout.Num());
	Result->SetNumberField(TEXT("add_count"), FilesToAdd.Num());

	if (bDryRun)
	{
		Result->SetBoolField(TEXT("ok"), true);
		return FMonolithActionResult::Success(Result);
	}

	bool bOk = true;
	if (FilesToCheckout.Num() > 0)
	{
		FSourceControlOperationRef Operation = ISourceControlOperation::Create<FCheckOut>();
		const ECommandResult::Type CommandResult = Provider.Execute(Operation, FilesToCheckout, EConcurrency::Synchronous);
		Result->SetStringField(TEXT("checkout_result"), CommandResultToString(CommandResult));
		bOk = bOk && CommandResult == ECommandResult::Succeeded;
	}
	if (FilesToAdd.Num() > 0)
	{
		FSourceControlOperationRef Operation = ISourceControlOperation::Create<FMarkForAdd>();
		const ECommandResult::Type CommandResult = Provider.Execute(Operation, FilesToAdd, EConcurrency::Synchronous);
		Result->SetStringField(TEXT("add_result"), CommandResultToString(CommandResult));
		bOk = bOk && CommandResult == ECommandResult::Succeeded;
	}

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("states"), GetStateRows(Provider, Files));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleRevert(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("revert"), ISourceControlOperation::Create<FRevert>(), true);
}

FMonolithActionResult FMonolithSourceControlActions::HandleRevertUnchanged(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("revert_unchanged"), ISourceControlOperation::Create<FRevertUnchanged>(), true);
}
