#include "MonolithSourceControlActions.h"

#include "MonolithParamSchema.h"
#include "MonolithSourceControlUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
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
		return FMonolithSourceControlUtils::NormalizePathForSourceControl(Input, OutFile, OutError);
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

	FString QuoteP4Arg(const FString& Arg)
	{
		FString Escaped = Arg;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return TEXT("\"") + Escaped + TEXT("\"");
	}

	bool RunP4(const FString& Args, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
	{
		OutStdOut.Reset();
		OutStdErr.Reset();
		OutReturnCode = INDEX_NONE;
		return FPlatformProcess::ExecProcess(TEXT("p4"), *Args, &OutReturnCode, &OutStdOut, &OutStdErr);
	}

	TArray<TMap<FString, FString>> ParseTaggedRecords(const FString& Output)
	{
		TArray<TMap<FString, FString>> Records;
		TMap<FString, FString> Current;
		TArray<FString> Lines;
		Output.ParseIntoArrayLines(Lines, false);
		for (const FString& RawLine : Lines)
		{
			FString Line = RawLine;
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty())
			{
				if (Current.Num() > 0)
				{
					Records.Add(MoveTemp(Current));
					Current.Reset();
				}
				continue;
			}
			if (!Line.StartsWith(TEXT("... ")))
			{
				continue;
			}

			FString Remainder = Line.RightChop(4);
			FString Key;
			FString Value;
			if (!Remainder.Split(TEXT(" "), &Key, &Value))
			{
				continue;
			}
			if (Key == TEXT("depotFile") && Current.Contains(TEXT("depotFile")))
			{
				Records.Add(MoveTemp(Current));
				Current.Reset();
			}
			Current.Add(Key, Value);
		}
		if (Current.Num() > 0)
		{
			Records.Add(MoveTemp(Current));
		}
		return Records;
	}

	FString RecordValue(const TMap<FString, FString>& Record, const TCHAR* Key)
	{
		if (const FString* Value = Record.Find(Key))
		{
			return *Value;
		}
		return FString();
	}

	bool DepotPathToLocalPath(const FString& DepotPath, FString& OutLocalPath, FString& OutError)
	{
		FString StdOut;
		FString StdErr;
		int32 ReturnCode = 0;
		if (!RunP4(TEXT("-ztag where ") + QuoteP4Arg(DepotPath), StdOut, StdErr, ReturnCode))
		{
			OutError = TEXT("Failed to execute p4 where.");
			return false;
		}
		if (ReturnCode != 0)
		{
			OutError = StdErr.IsEmpty() ? StdOut : StdErr;
			return false;
		}
		const TArray<TMap<FString, FString>> Records = ParseTaggedRecords(StdOut);
		if (Records.Num() == 0)
		{
			OutError = FString::Printf(TEXT("p4 where returned no records for '%s'."), *DepotPath);
			return false;
		}
		OutLocalPath = RecordValue(Records[0], TEXT("path"));
		if (OutLocalPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("p4 where returned no local path for '%s'."), *DepotPath);
			return false;
		}
		FPaths::NormalizeFilename(OutLocalPath);
		return true;
	}

	FString LocalPathToPackagePath(FString LocalPath)
	{
		FPaths::NormalizeFilename(LocalPath);
		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(LocalPath, PackageName))
		{
			return PackageName;
		}
		return FString();
	}

	TSharedPtr<FJsonObject> OpenedRecordToJson(const TMap<FString, FString>& Record, bool bResolvePackages)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		const FString DepotFile = RecordValue(Record, TEXT("depotFile"));
		const FString ClientFile = RecordValue(Record, TEXT("clientFile"));
		Row->SetStringField(TEXT("depot_file"), DepotFile);
		Row->SetStringField(TEXT("client_file"), ClientFile);
		Row->SetStringField(TEXT("action"), RecordValue(Record, TEXT("action")));
		Row->SetStringField(TEXT("change"), RecordValue(Record, TEXT("change")));
		Row->SetStringField(TEXT("type"), RecordValue(Record, TEXT("type")));
		Row->SetStringField(TEXT("user"), RecordValue(Record, TEXT("user")));
		Row->SetStringField(TEXT("client"), RecordValue(Record, TEXT("client")));
		Row->SetStringField(TEXT("rev"), RecordValue(Record, TEXT("rev")));
		Row->SetStringField(TEXT("have_rev"), RecordValue(Record, TEXT("haveRev")));

		if (bResolvePackages && !DepotFile.IsEmpty())
		{
			FString LocalPath;
			FString Error;
			if (DepotPathToLocalPath(DepotFile, LocalPath, Error))
			{
				Row->SetStringField(TEXT("local_path"), LocalPath);
				const FString PackagePath = LocalPathToPackagePath(LocalPath);
				Row->SetStringField(TEXT("package_path"), PackagePath);
				Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
			}
			else
			{
				Row->SetStringField(TEXT("where_error"), Error);
				Row->SetBoolField(TEXT("is_package"), false);
			}
		}

		return Row;
	}
}

void FMonolithSourceControlActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(TEXT("source_control"), TEXT("get_capabilities"),
		TEXT("Return the active Unreal source-control provider and Phase 1 Monolith action capabilities."),
		FMonolithActionHandler::CreateStatic(&HandleGetCapabilities),
		FParamSchemaBuilder()
			.EnableValidation()
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("get_status"),
		TEXT("Return source-control status for filesystem or /Game package paths."),
		FMonolithActionHandler::CreateStatic(&HandleGetStatus),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout"),
		TEXT("Check out files through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleCheckout),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("add"),
		TEXT("Mark files for add through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleAdd),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout_or_add"),
		TEXT("Prepare files for mutation by checking out existing source-controlled files or adding local files."),
		FMonolithActionHandler::CreateStatic(&HandleCheckoutOrAdd),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview actions without executing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("delete"),
		TEXT("Mark files for delete through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleDelete),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute source-control delete"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("mark_for_delete"),
		TEXT("Mark files for delete through the active Unreal source-control provider. Alias of delete with an explicit source-control name. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleMarkForDelete),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute source-control mark for delete"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert"),
		TEXT("Revert files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevert),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert_unchanged"),
		TEXT("Revert unchanged files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevertUnchanged),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Filesystem paths or /Game package/object paths"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing"), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert unchanged"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("list_opened"),
		TEXT("List Perforce opened files through `p4 -ztag opened`, optionally scoped to a changelist, and map opened depot files back to local and Unreal package paths."),
		FMonolithActionHandler::CreateStatic(&HandleListOpened),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("changelist"), TEXT("string"), TEXT("Perforce changelist number or 'default'. Omit for all opened files."))
			.Optional(TEXT("resolve_packages"), TEXT("bool"), TEXT("Run p4 where for each opened file and emit local_path/package_path. Default true."), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum opened records to return. Default 200, max 2000."), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("map_depot_paths"),
		TEXT("Map Perforce depot/client/local paths to local filesystem and Unreal long package paths. Depot paths are resolved with `p4 -ztag where`; local paths are converted with FPackageName mount points."),
		FMonolithActionHandler::CreateStatic(&HandleMapDepotPaths),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("paths"), TEXT("array"), TEXT("Depot, client, local filesystem, or /Game package/object paths to map"))
			.Build());

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("get_status"),
		{ TEXT("perforce"), TEXT("p4"), TEXT("checked out by"), TEXT("depot revision"), TEXT("changelist"), TEXT("pending changes") },
		{ TEXT("file_status"), TEXT("p4_status"), TEXT("sc_status"), TEXT("who_has_checked_out") },
		{ TEXT("is this asset checked out in perforce"), TEXT("show source control status for these files"), TEXT("who has this file checked out") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("checkout"),
		{ TEXT("perforce"), TEXT("p4 edit"), TEXT("open for edit"), TEXT("prepare for edit"), TEXT("make writable"), TEXT("lock file") },
		{ TEXT("check_out"), TEXT("p4_edit"), TEXT("open_for_edit"), TEXT("edit_file") },
		{ TEXT("check out this asset before editing"), TEXT("open these files for edit in perforce"), TEXT("make this ini writable in source control") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("add"),
		{ TEXT("perforce"), TEXT("p4 add"), TEXT("add to depot"), TEXT("track new file"), TEXT("stage new file"), TEXT("submit new asset") },
		{ TEXT("mark_for_add"), TEXT("p4_add"), TEXT("add_to_source_control"), TEXT("add_file") },
		{ TEXT("mark this new asset for add"), TEXT("add these new files to perforce"), TEXT("start tracking this file in source control") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("checkout_or_add"),
		{ TEXT("perforce"), TEXT("prepare for edit"), TEXT("open or add"), TEXT("make writable"), TEXT("ensure tracked"), TEXT("edit existing or add new") },
		{ TEXT("checkout_or_add"), TEXT("prepare_for_mutation"), TEXT("edit_or_add"), TEXT("ensure_writable") },
		{ TEXT("prepare these files for mutation in source control"), TEXT("check out if tracked otherwise add this file"), TEXT("make sure these assets are writable before saving") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("mark_for_delete"),
		{ TEXT("perforce"), TEXT("p4 delete"), TEXT("remove from depot"), TEXT("open for delete"), TEXT("stage deletion"), TEXT("untrack file") },
		{ TEXT("delete"), TEXT("p4_delete"), TEXT("remove_from_source_control"), TEXT("delete_file") },
		{ TEXT("mark this asset for delete in perforce"), TEXT("remove these files from source control"), TEXT("open these files for delete in p4") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("revert"),
		{ TEXT("perforce"), TEXT("p4 revert"), TEXT("discard changes"), TEXT("undo checkout"), TEXT("restore file"), TEXT("roll back edits") },
		{ TEXT("discard"), TEXT("p4_revert"), TEXT("undo_changes"), TEXT("restore_file") },
		{ TEXT("revert my changes to this file"), TEXT("discard the checkout on these assets"), TEXT("undo edits and restore from depot") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("list_opened"),
		{ TEXT("perforce opened"), TEXT("p4 opened"), TEXT("changelist"), TEXT("opened packages"), TEXT("package path") },
		{ TEXT("p4_opened"), TEXT("opened_files"), TEXT("opened_changelist") },
		{ TEXT("list opened files in changelist 1001"), TEXT("map opened uassets to package paths") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("source_control"), TEXT("map_depot_paths"),
		{ TEXT("p4 where"), TEXT("depot path"), TEXT("client path"), TEXT("local path"), TEXT("package path") },
		{ TEXT("p4_where"), TEXT("map_to_package_path"), TEXT("depot_to_package") },
		{ TEXT("map //depot file paths to /Game package names"), TEXT("convert opened depot files to Unreal package paths") });
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
		TEXT("delete"),
		TEXT("mark_for_delete"),
		TEXT("revert"),
		TEXT("revert_unchanged"),
		TEXT("list_opened"),
		TEXT("map_depot_paths") })
	{
		ActionsJson.Add(MakeShared<FJsonValueString>(Name));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
	Result->SetBoolField(TEXT("enabled"), Module.IsEnabled());
	Result->SetArrayField(TEXT("available_providers"), ProvidersJson);
	Result->SetArrayField(TEXT("phase1_actions"), ActionsJson);
	Result->SetBoolField(TEXT("supports_changelists"), true);
	Result->SetBoolField(TEXT("supports_shelving"), false);
	Result->SetStringField(TEXT("note"), TEXT("Provider actions cover safe file prepare/delete/revert. P4 opened/changelist inspection is exposed through read-only p4 -ztag wrappers."));
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

FMonolithActionResult FMonolithSourceControlActions::HandleListOpened(const TSharedPtr<FJsonObject>& Params)
{
	FString Changelist;
	bool bResolvePackages = true;
	double LimitValue = 200.0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("changelist"), Changelist);
		Params->TryGetBoolField(TEXT("resolve_packages"), bResolvePackages);
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
	}
	Changelist.TrimStartAndEndInline();
	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 2000);

	FString Args = TEXT("-ztag opened");
	if (!Changelist.IsEmpty())
	{
		Args += TEXT(" -c ") + QuoteP4Arg(Changelist);
	}

	FString StdOut;
	FString StdErr;
	int32 ReturnCode = 0;
	if (!RunP4(Args, StdOut, StdErr, ReturnCode))
	{
		return FMonolithActionResult::Error(TEXT("Failed to execute p4 opened."), -32603);
	}

	const bool bNoOpenedFiles = ReturnCode != 0
		&& (StdOut.Contains(TEXT("file(s) not opened"), ESearchCase::IgnoreCase)
			|| StdErr.Contains(TEXT("file(s) not opened"), ESearchCase::IgnoreCase));
	if (ReturnCode != 0 && !bNoOpenedFiles)
	{
		return FMonolithActionResult::Error(StdErr.IsEmpty() ? StdOut : StdErr, -32603);
	}

	const TArray<TMap<FString, FString>> Records = bNoOpenedFiles
		? TArray<TMap<FString, FString>>()
		: ParseTaggedRecords(StdOut);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (int32 Index = 0; Index < Records.Num() && Rows.Num() < Limit; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(OpenedRecordToJson(Records[Index], bResolvePackages)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("changelist"), Changelist);
	Result->SetBoolField(TEXT("resolve_packages"), bResolvePackages);
	Result->SetNumberField(TEXT("count"), Records.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetBoolField(TEXT("truncated"), Records.Num() > Rows.Num());
	Result->SetArrayField(TEXT("opened"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleMapDepotPaths(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("paths"), Values) || !Values || Values->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: paths (non-empty string array)"), -32602);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		FString Input;
		if (!Value.IsValid() || !Value->TryGetString(Input) || Input.IsEmpty())
		{
			Row->SetBoolField(TEXT("valid"), false);
			Row->SetStringField(TEXT("error"), TEXT("path entry is not a non-empty string"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		Input.TrimStartAndEndInline();
		Row->SetStringField(TEXT("input"), Input);

		FString LocalPath;
		FString PackagePath;
		if (Input.StartsWith(TEXT("//")))
		{
			FString Error;
			if (DepotPathToLocalPath(Input, LocalPath, Error))
			{
				PackagePath = LocalPathToPackagePath(LocalPath);
				Row->SetBoolField(TEXT("valid"), true);
				Row->SetStringField(TEXT("local_path"), LocalPath);
				Row->SetStringField(TEXT("package_path"), PackagePath);
				Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
			}
			else
			{
				Row->SetBoolField(TEXT("valid"), false);
				Row->SetStringField(TEXT("error"), Error);
			}
		}
		else if (Input.StartsWith(TEXT("/")))
		{
			FString PackageName = Input.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Input)
				: Input;
			Row->SetBoolField(TEXT("valid"), true);
			Row->SetStringField(TEXT("package_path"), PackageName);
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename))
			{
				FPaths::NormalizeFilename(Filename);
				Row->SetStringField(TEXT("local_path_no_extension"), Filename);
			}
			Row->SetBoolField(TEXT("is_package"), true);
		}
		else
		{
			LocalPath = FPaths::ConvertRelativePathToFull(Input);
			FPaths::NormalizeFilename(LocalPath);
			PackagePath = LocalPathToPackagePath(LocalPath);
			Row->SetBoolField(TEXT("valid"), true);
			Row->SetStringField(TEXT("local_path"), LocalPath);
			Row->SetStringField(TEXT("package_path"), PackagePath);
			Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
		}

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("paths"), Rows);
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

	FMonolithSourceControlPrepareOptions Options;
	Options.bDryRun = bDryRun;
	Options.bUnavailableIsSuccess = false;
	Options.bAddMissingFiles = true;

	TSharedPtr<FJsonObject> Result = FMonolithSourceControlUtils::CheckoutOrAddFiles(Inputs, Options);
	if (Result.IsValid())
	{
		Result->SetArrayField(TEXT("paths"), PathRows);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleDelete(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("delete"), ISourceControlOperation::Create<FDelete>(), true);
}

FMonolithActionResult FMonolithSourceControlActions::HandleMarkForDelete(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("mark_for_delete"), ISourceControlOperation::Create<FDelete>(), true);
}

FMonolithActionResult FMonolithSourceControlActions::HandleRevert(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("revert"), ISourceControlOperation::Create<FRevert>(), true);
}

FMonolithActionResult FMonolithSourceControlActions::HandleRevertUnchanged(const TSharedPtr<FJsonObject>& Params)
{
	return ExecuteFileOperation(Params, TEXT("revert_unchanged"), ISourceControlOperation::Create<FRevertUnchanged>(), true);
}
