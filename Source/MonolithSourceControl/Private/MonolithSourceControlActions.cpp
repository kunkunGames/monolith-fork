#include "MonolithSourceControlActions.h"

#include "MonolithParamSchema.h"
#include "MonolithSourceControlP4Batch.h"
#include "MonolithSourceControlUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
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

	bool ReadPathValues(const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>& OutPathValues, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
		if (!Params.IsValid())
		{
			OutError = TEXT("Required parameter: paths (non-empty string array or string)");
			return false;
		}

		const TSharedPtr<FJsonValue>* RawPaths = Params->Values.Find(TEXT("paths"));
		if (!RawPaths || !RawPaths->IsValid())
		{
			OutError = TEXT("Required parameter: paths (non-empty string array or string)");
			return false;
		}

		if ((*RawPaths)->Type == EJson::Array)
		{
			if (!Params->TryGetArrayField(TEXT("paths"), PathValues) || !PathValues || PathValues->Num() == 0)
			{
				OutError = TEXT("Required parameter: paths (non-empty string array or string)");
				return false;
			}
			OutPathValues = *PathValues;
			return true;
		}

		if ((*RawPaths)->Type == EJson::String)
		{
			FString Path;
			if (!(*RawPaths)->TryGetString(Path) || Path.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("Required parameter: paths (non-empty string array or string)");
				return false;
			}
			OutPathValues.Add(MakeShared<FJsonValueString>(Path));
			return true;
		}

		OutError = TEXT("Required parameter: paths (non-empty string array or string)");
		return false;
	}

	bool ReadPathArray(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutInputs, TArray<FString>& OutFiles, TArray<TSharedPtr<FJsonValue>>& OutRows, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> PathValues;
		if (!ReadPathValues(Params, PathValues, OutError))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : PathValues)
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

	bool TryReadOptionalBoolField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue>* RawValue = Params->Values.Find(FieldName);
		if (!RawValue)
		{
			return true;
		}

		if (!RawValue->IsValid() || (*RawValue)->Type != EJson::Boolean)
		{
			OutError = FString::Printf(TEXT("%s must be a bool."), FieldName);
			return false;
		}

		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("Failed to read %s as a bool."), FieldName);
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
			return FMonolithActionResult::Error(Error, -32602);
		}

		bool bDryRun = false;
		bool bConfirm = false;
		if (Params.IsValid())
		{
			if (!TryReadOptionalBoolField(Params, TEXT("dry_run"), bDryRun, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
			if (!TryReadOptionalBoolField(Params, TEXT("confirm"), bConfirm, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
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

	bool RunP4(const FString& Args, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
	{
		OutStdOut.Reset();
		OutStdErr.Reset();
		OutReturnCode = INDEX_NONE;

		void* StdOutRead = nullptr;
		void* StdOutWrite = nullptr;
		void* StdErrRead = nullptr;
		void* StdErrWrite = nullptr;
		if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite))
		{
			OutStdErr = TEXT("Failed to create the p4 stdout pipe.");
			return false;
		}
		if (!FPlatformProcess::CreatePipe(StdErrRead, StdErrWrite))
		{
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			OutStdErr = TEXT("Failed to create the p4 stderr pipe.");
			return false;
		}

		uint32 ProcessId = 0;
		FProcHandle Process = FPlatformProcess::CreateProc(
			TEXT("p4"),
			*Args,
			false,
			true,
			true,
			&ProcessId,
			0,
			nullptr,
			StdOutWrite,
			nullptr,
			StdErrWrite);
		if (!Process.IsValid())
		{
			FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
			FPlatformProcess::ClosePipe(StdErrRead, StdErrWrite);
			OutStdErr = TEXT("Failed to launch p4.");
			return false;
		}

		// The child inherited its write handles during CreateProc; the parent
		// keeps only the read ends so pipe lifetime cannot mask process exit.
		FPlatformProcess::ClosePipe(nullptr, StdOutWrite);
		FPlatformProcess::ClosePipe(nullptr, StdErrWrite);
		StdOutWrite = nullptr;
		StdErrWrite = nullptr;

		MonolithSourceControlP4::FProcessPollCallbacks Callbacks;
		Callbacks.IsRunning = [&Process]()
		{
			return FPlatformProcess::IsProcRunning(Process);
		};
		Callbacks.TerminateAndWait = [&Process]()
		{
			FPlatformProcess::TerminateProc(Process, true);
			FPlatformProcess::WaitForProc(Process);
		};
		Callbacks.ReadStdOut = [StdOutRead]()
		{
			return FPlatformProcess::ReadPipe(StdOutRead);
		};
		Callbacks.ReadStdErr = [StdErrRead]()
		{
			return FPlatformProcess::ReadPipe(StdErrRead);
		};
		Callbacks.GetReturnCode = [&Process](int32& ReturnCode)
		{
			return FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
		};
		Callbacks.NowSeconds = []()
		{
			return FPlatformTime::Seconds();
		};
		Callbacks.Sleep = [](float Seconds)
		{
			FPlatformProcess::Sleep(Seconds);
		};

		const MonolithSourceControlP4::FProcessPollResult PollResult =
			MonolithSourceControlP4::PollProcessWithTimeout(
				Callbacks,
				MonolithSourceControlP4::CommandTimeoutSeconds);
		OutStdOut = PollResult.StdOut;
		OutStdErr = PollResult.StdErr;
		OutReturnCode = PollResult.ReturnCode;
		if (PollResult.bTimedOut)
		{
			if (!OutStdErr.IsEmpty() && !OutStdErr.EndsWith(TEXT("\n")))
			{
				OutStdErr += TEXT("\n");
			}
			OutStdErr += FString::Printf(
				TEXT("p4 command timed out after %.0f seconds and was terminated."),
				MonolithSourceControlP4::CommandTimeoutSeconds);
		}
		else if (!PollResult.bReturnCodeAvailable)
		{
			OutStdErr += TEXT("p4 exited without an available return code.");
		}

		FPlatformProcess::CloseProc(Process);
		FPlatformProcess::ClosePipe(StdOutRead, nullptr);
		FPlatformProcess::ClosePipe(StdErrRead, nullptr);
		return true;
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

	MonolithSourceControlP4::FDepotPathBatchResult ResolveDepotPathsWithP4(const TArray<FString>& DepotPaths)
	{
		return MonolithSourceControlP4::ResolveDepotPathsBatched(
			DepotPaths,
			[](const TArray<FString>& Batch, FString& OutStdOut, FString& OutStdErr, int32& OutReturnCode)
			{
				FString Args = TEXT("-ztag where");
				for (const FString& DepotPath : Batch)
				{
					Args += TEXT(" ") + MonolithSourceControlP4::QuoteCommandLineArgument(DepotPath);
				}
				return RunP4(Args, OutStdOut, OutStdErr, OutReturnCode);
			});
	}

	FString LocalPathToPackagePath(FString LocalPath)
	{
		FPaths::NormalizeFilename(LocalPath);
		FString CandidatePackageName;
		FString MountedPackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(LocalPath, CandidatePackageName)
			&& FMonolithSourceControlUtils::TryGetMountedPackageName(
				CandidatePackageName,
				MountedPackageName))
		{
			return MountedPackageName;
		}
		return FString();
	}

	TSharedPtr<FJsonObject> OpenedRecordToJson(
		const TMap<FString, FString>& Record,
		bool bResolvePackages,
		const MonolithSourceControlP4::FDepotPathBatchResult& MappingResult)
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
			const MonolithSourceControlP4::FDepotPathMapping* Mapping = MappingResult.Mappings.Find(DepotFile);
			if (Mapping && Mapping->IsResolved())
			{
				Row->SetStringField(TEXT("local_path"), Mapping->LocalPath);
				const FString PackagePath = LocalPathToPackagePath(Mapping->LocalPath);
				Row->SetStringField(TEXT("package_path"), PackagePath);
				Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
			}
			else
			{
				Row->SetStringField(
					TEXT("where_error"),
					Mapping ? Mapping->Error : TEXT("No batched p4 where result was produced for this depot path."));
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
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("get_status"),
		TEXT("Return source-control status for filesystem or /Game package paths."),
		FMonolithActionHandler::CreateStatic(&HandleGetStatus),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout"),
		TEXT("Check out files through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleCheckout),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("add"),
		TEXT("Mark files for add through the active Unreal source-control provider."),
		FMonolithActionHandler::CreateStatic(&HandleAdd),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("checkout_or_add"),
		TEXT("Prepare files for mutation by checking out existing source-controlled files or adding local files."),
		FMonolithActionHandler::CreateStatic(&HandleCheckoutOrAdd),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview actions without executing."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("delete"),
		TEXT("Mark files for delete through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleDelete),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute source-control delete."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("mark_for_delete"),
		TEXT("Mark files for delete through the active Unreal source-control provider. Alias of delete with an explicit source-control name. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleMarkForDelete),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute source-control mark for delete."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert"),
		TEXT("Revert files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevert),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("revert_unchanged"),
		TEXT("Revert unchanged files through the active Unreal source-control provider. Requires confirm=true unless dry_run=true."),
		FMonolithActionHandler::CreateStatic(&HandleRevertUnchanged),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("Filesystem paths or /Game package/object paths. Alias: files."), { TEXT("files") })
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview states without executing."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("bool"), TEXT("Required to execute revert unchanged."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("list_opened"),
		TEXT("List a bounded Perforce opened-file window through `p4 -ztag opened -m (limit + 1)`, optionally scoped to a decimal/default changelist, and map returned depot files back to local and Unreal package paths."),
		FMonolithActionHandler::CreateStatic(&HandleListOpened),
		FParamSchemaBuilder()
			.Optional(TEXT("changelist"), TEXT("string"), TEXT("Decimal Perforce changelist number or 'default'. Omit for all opened files."))
			.Optional(TEXT("resolve_packages"), TEXT("bool"), TEXT("Resolve opened files through bounded p4 where batches and emit local_path/package_path. Default true."), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum opened records to return. Default 200, max 5000."), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("source_control"), TEXT("map_depot_paths"),
		TEXT("Map at most 5000 Perforce depot/client/local paths to local filesystem and Unreal long package paths. Depot paths are resolved with at most 40 bounded `p4 -ztag where` commands; local paths are converted with FPackageName mount points."),
		FMonolithActionHandler::CreateStatic(&HandleMapDepotPaths),
		FParamSchemaBuilder()
			.Required(TEXT("paths"), TEXT("array|string"), TEXT("At most 5000 depot, client, local filesystem, or /Game package/object paths to map. Control characters are rejected. Alias: files."), { TEXT("files") })
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
		return FMonolithActionResult::Error(Error, -32602);
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
		if (const TSharedPtr<FJsonValue>* ChangelistValue = Params->Values.Find(TEXT("changelist")))
		{
			if (!ChangelistValue->IsValid() || (*ChangelistValue)->Type != EJson::String)
			{
				return FMonolithActionResult::Error(TEXT("changelist must be a string."), -32602);
			}
			Changelist = (*ChangelistValue)->AsString();
		}
		FString BoolError;
		if (!TryReadOptionalBoolField(Params, TEXT("resolve_packages"), bResolvePackages, BoolError))
		{
			return FMonolithActionResult::Error(BoolError, -32602);
		}
		if (const TSharedPtr<FJsonValue>* LimitValueField = Params->Values.Find(TEXT("limit")))
		{
			if (!LimitValueField->IsValid() || (*LimitValueField)->Type != EJson::Number)
			{
				return FMonolithActionResult::Error(TEXT("limit must be a number."), -32602);
			}
			LimitValue = (*LimitValueField)->AsNumber();
		}
	}
	int32 Limit = 0;
	FString LimitError;
	if (!MonolithSourceControlP4::TryValidateOpenedLimit(LimitValue, Limit, LimitError))
	{
		return FMonolithActionResult::Error(LimitError, -32602);
	}

	FString Args;
	FString CommandError;
	int32 BackendRecordLimit = 0;
	if (!MonolithSourceControlP4::TryBuildOpenedCommandArgs(
		Changelist,
		Limit,
		Args,
		BackendRecordLimit,
		CommandError))
	{
		return FMonolithActionResult::Error(CommandError, -32602);
	}
	Changelist.TrimStartAndEndInline();

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
	const MonolithSourceControlP4::FOpenedRecordWindow RecordWindow =
		MonolithSourceControlP4::MakeOpenedRecordWindow(Records.Num(), Limit);
	const int32 ReturnedRecordCount = RecordWindow.ReturnedRecordCount;
	TArray<FString> DepotPaths;
	DepotPaths.Reserve(ReturnedRecordCount);
	if (bResolvePackages)
	{
		for (int32 Index = 0; Index < ReturnedRecordCount; ++Index)
		{
			const FString DepotFile = RecordValue(Records[Index], TEXT("depotFile"));
			if (!DepotFile.IsEmpty())
			{
				DepotPaths.Add(DepotFile);
			}
		}
	}
	const MonolithSourceControlP4::FDepotPathBatchResult MappingResult = bResolvePackages
		? ResolveDepotPathsWithP4(DepotPaths)
		: MonolithSourceControlP4::FDepotPathBatchResult();
	if (MappingResult.bRejected)
	{
		return FMonolithActionResult::Error(MappingResult.Error, -32602);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(ReturnedRecordCount);
	for (int32 Index = 0; Index < ReturnedRecordCount; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(OpenedRecordToJson(Records[Index], bResolvePackages, MappingResult)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("changelist"), Changelist);
	Result->SetBoolField(TEXT("resolve_packages"), bResolvePackages);
	// Backward field: `count` is now the bounded observed count. When
	// count_is_lower_bound=true, at least one sentinel row proves that more
	// records exist, but no unbounded exact-count query is performed.
	Result->SetNumberField(TEXT("count"), RecordWindow.ObservedRecordCount);
	Result->SetStringField(
		TEXT("count_semantics"),
		RecordWindow.bCountIsLowerBound ? TEXT("lower_bound") : TEXT("exact"));
	Result->SetBoolField(TEXT("count_is_lower_bound"), RecordWindow.bCountIsLowerBound);
	Result->SetNumberField(TEXT("observed_count"), RecordWindow.ObservedRecordCount);
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetNumberField(TEXT("sentinel_record_count"), RecordWindow.SentinelRecordCount);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("backend_record_limit"), BackendRecordLimit);
	Result->SetBoolField(TEXT("has_more"), RecordWindow.bHasMore);
	Result->SetBoolField(TEXT("truncated"), RecordWindow.bHasMore);
	Result->SetNumberField(TEXT("mapping_raw_count"), MappingResult.RawPathCount);
	Result->SetNumberField(TEXT("mapping_requested_count"), MappingResult.RequestedPathCount);
	Result->SetNumberField(TEXT("mapping_unique_count"), MappingResult.UniquePathCount);
	Result->SetNumberField(TEXT("mapping_resolved_count"), MappingResult.ResolvedPathCount);
	Result->SetNumberField(TEXT("mapping_failed_count"), MappingResult.FailedPathCount);
	Result->SetNumberField(TEXT("mapping_command_count"), MappingResult.CommandCount);
	Result->SetArrayField(TEXT("opened"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithSourceControlActions::HandleMapDepotPaths(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	FString Error;
	if (!ReadPathValues(Params, Values, Error))
	{
		return FMonolithActionResult::Error(Error, -32602);
	}
	if (Values.Num() > MonolithSourceControlP4::MaxInputPathCount)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("paths accepts at most %d entries; received %d."),
				MonolithSourceControlP4::MaxInputPathCount,
				Values.Num()),
			-32602);
	}
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		FString Input;
		if (Values[Index].IsValid() && Values[Index]->TryGetString(Input))
		{
			FString ArgumentError;
			if (!MonolithSourceControlP4::ValidateCommandLineArgument(Input, ArgumentError))
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("paths[%d] %s"), Index, *ArgumentError),
					-32602);
			}
		}
	}

	TArray<FString> DepotPaths;
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		FString Input;
		if (Value.IsValid() && Value->TryGetString(Input))
		{
			Input.TrimStartAndEndInline();
			if (Input.StartsWith(TEXT("//")))
			{
				DepotPaths.Add(Input);
			}
		}
	}
	const MonolithSourceControlP4::FDepotPathBatchResult MappingResult = ResolveDepotPathsWithP4(DepotPaths);
	if (MappingResult.bRejected)
	{
		return FMonolithActionResult::Error(MappingResult.Error, -32602);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		FString Input;
		if (Value.IsValid())
		{
			Value->TryGetString(Input);
			Input.TrimStartAndEndInline();
		}
		if (!Value.IsValid() || Input.IsEmpty())
		{
			Row->SetBoolField(TEXT("valid"), false);
			Row->SetStringField(TEXT("error"), TEXT("path entry is not a non-empty string"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}
		Row->SetStringField(TEXT("input"), Input);

		FString LocalPath;
		FString PackagePath;
		if (Input.StartsWith(TEXT("//")))
		{
			const MonolithSourceControlP4::FDepotPathMapping* Mapping = MappingResult.Mappings.Find(Input);
			if (Mapping && Mapping->IsResolved())
			{
				LocalPath = Mapping->LocalPath;
				PackagePath = LocalPathToPackagePath(LocalPath);
				Row->SetBoolField(TEXT("valid"), true);
				Row->SetStringField(TEXT("local_path"), LocalPath);
				Row->SetStringField(TEXT("package_path"), PackagePath);
				Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
			}
			else
			{
				Row->SetBoolField(TEXT("valid"), false);
				Row->SetStringField(
					TEXT("error"),
					Mapping ? Mapping->Error : TEXT("No batched p4 where result was produced for this depot path."));
			}
		}
		else
		{
			FString MountedPackageName;
			if (FMonolithSourceControlUtils::TryGetMountedPackageName(Input, MountedPackageName))
			{
				FString Filename;
				FString FilenameError;
				if (!FMonolithSourceControlUtils::PackageNameToFilename(
					MountedPackageName,
					Filename,
					FilenameError))
				{
					Row->SetBoolField(TEXT("valid"), false);
					Row->SetStringField(TEXT("error"), FilenameError);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
					continue;
				}

				FString FilenameWithoutExtension;
				if (FPackageName::TryConvertLongPackageNameToFilename(
					MountedPackageName,
					FilenameWithoutExtension))
				{
					FilenameWithoutExtension =
						FPaths::ConvertRelativePathToFull(FilenameWithoutExtension);
					FPaths::NormalizeFilename(FilenameWithoutExtension);
					Row->SetStringField(
						TEXT("local_path_no_extension"),
						FilenameWithoutExtension);
				}

				Row->SetBoolField(TEXT("valid"), true);
				Row->SetStringField(TEXT("local_path"), Filename);
				Row->SetStringField(TEXT("package_path"), MountedPackageName);
				Row->SetBoolField(TEXT("is_package"), true);
			}
			else
			{
				FString NormalizeError;
				if (!FMonolithSourceControlUtils::NormalizePathForSourceControl(
					Input,
					LocalPath,
					NormalizeError))
				{
					Row->SetBoolField(TEXT("valid"), false);
					Row->SetStringField(TEXT("error"), NormalizeError);
					Rows.Add(MakeShared<FJsonValueObject>(Row));
					continue;
				}

				PackagePath = LocalPathToPackagePath(LocalPath);
				Row->SetBoolField(TEXT("valid"), true);
				Row->SetStringField(TEXT("local_path"), LocalPath);
				Row->SetStringField(TEXT("package_path"), PackagePath);
				Row->SetBoolField(TEXT("is_package"), !PackagePath.IsEmpty());
			}
		}

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetNumberField(TEXT("mapping_raw_count"), MappingResult.RawPathCount);
	Result->SetNumberField(TEXT("mapping_requested_count"), MappingResult.RequestedPathCount);
	Result->SetNumberField(TEXT("mapping_unique_count"), MappingResult.UniquePathCount);
	Result->SetNumberField(TEXT("mapping_resolved_count"), MappingResult.ResolvedPathCount);
	Result->SetNumberField(TEXT("mapping_failed_count"), MappingResult.FailedPathCount);
	Result->SetNumberField(TEXT("mapping_command_count"), MappingResult.CommandCount);
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
		return FMonolithActionResult::Error(Error, -32602);
	}

	bool bDryRun = false;
	if (Params.IsValid())
	{
		if (!TryReadOptionalBoolField(Params, TEXT("dry_run"), bDryRun, Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}
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
