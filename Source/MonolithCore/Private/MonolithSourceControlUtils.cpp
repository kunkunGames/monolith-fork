#include "MonolithSourceControlUtils.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlOperation.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlOperations.h"
#include "UObject/Package.h"

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

	void AddOperationMessages(const FSourceControlOperationRef& Operation, TSharedPtr<FJsonObject>& Result, const FString& Prefix)
	{
		const FSourceControlResultInfo& Info = Operation->GetResultInfo();

		TArray<TSharedPtr<FJsonValue>> ErrorRows;
		for (const FText& Error : Info.ErrorMessages)
		{
			ErrorRows.Add(MakeShared<FJsonValueString>(Error.ToString()));
		}
		Result->SetArrayField(Prefix + TEXT("_errors"), ErrorRows);

		TArray<TSharedPtr<FJsonValue>> InfoRows;
		for (const FText& Message : Info.InfoMessages)
		{
			InfoRows.Add(MakeShared<FJsonValueString>(Message.ToString()));
		}
		Result->SetArrayField(Prefix + TEXT("_messages"), InfoRows);
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
}

bool FMonolithSourceControlUtils::IsProviderAvailable(FString& OutReason)
{
	ISourceControlModule& Module = ISourceControlModule::Get();
	if (!Module.IsEnabled())
	{
		OutReason = TEXT("Source control provider is disabled.");
		return false;
	}

	ISourceControlProvider& Provider = Module.GetProvider();
	if (!Provider.IsEnabled())
	{
		OutReason = TEXT("Source control provider is not enabled.");
		return false;
	}

	if (!Provider.IsAvailable())
	{
		OutReason = TEXT("Source control provider is unavailable.");
		return false;
	}

	OutReason.Empty();
	return true;
}

bool FMonolithSourceControlUtils::PackageNameToFilename(const FString& PackageName, FString& OutFile, FString& OutError)
{
	FString Normalized = PackageName.TrimStartAndEnd();
	if (Normalized.IsEmpty())
	{
		OutError = TEXT("empty package name");
		return false;
	}

	if (Normalized.Contains(TEXT(".")))
	{
		Normalized = FPackageName::ObjectPathToPackageName(Normalized);
	}

	if (!FPackageName::IsValidLongPackageName(Normalized, false))
	{
		OutError = FString::Printf(TEXT("invalid long package name: %s"), *PackageName);
		return false;
	}

	if (FPackageName::DoesPackageExist(Normalized, &OutFile))
	{
		OutFile = FPaths::ConvertRelativePathToFull(OutFile);
		FPaths::NormalizeFilename(OutFile);
		return true;
	}

	const UPackage* Package = FindPackage(nullptr, *Normalized);
	const FString& Extension = (Package && Package->ContainsMap())
		? FPackageName::GetMapPackageExtension()
		: FPackageName::GetAssetPackageExtension();

	if (FPackageName::TryConvertLongPackageNameToFilename(Normalized, OutFile, Extension))
	{
		OutFile = FPaths::ConvertRelativePathToFull(OutFile);
		FPaths::NormalizeFilename(OutFile);
		return true;
	}

	OutError = FString::Printf(TEXT("could not resolve package path: %s"), *PackageName);
	return false;
}

bool FMonolithSourceControlUtils::NormalizePathForSourceControl(const FString& Input, FString& OutFile, FString& OutError)
{
	FString Path = Input.TrimStartAndEnd();
	if (Path.IsEmpty())
	{
		OutError = TEXT("empty path");
		return false;
	}

	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	if (Path.EndsWith(TEXT("'")))
	{
		Path = FPackageName::ExportTextPathToObjectPath(Path);
	}

	if (Path.StartsWith(TEXT("/")))
	{
		FString PackageName = Path.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(Path)
			: Path;

		if (FPackageName::IsValidLongPackageName(PackageName, false))
		{
			return PackageNameToFilename(PackageName, OutFile, OutError);
		}
	}

	if (FPaths::IsRelative(Path))
	{
		Path = FPaths::Combine(FPaths::ProjectDir(), Path);
	}

	OutFile = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(OutFile);
	return true;
}

TSharedPtr<FJsonObject> FMonolithSourceControlUtils::CheckoutOrAddFiles(
	const TArray<FString>& Inputs,
	const FMonolithSourceControlPrepareOptions& Options)
{
	TArray<FString> Files;
	TArray<TSharedPtr<FJsonValue>> PathRows;
	Files.Reserve(Inputs.Num());
	PathRows.Reserve(Inputs.Num());

	for (const FString& Input : Inputs)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("input"), Input);

		FString File;
		FString Error;
		if (NormalizePathForSourceControl(Input, File, Error))
		{
			Row->SetBoolField(TEXT("valid"), true);
			Row->SetStringField(TEXT("file"), File);
			Files.AddUnique(File);
		}
		else
		{
			Row->SetBoolField(TEXT("valid"), false);
			Row->SetStringField(TEXT("error"), Error);
		}

		PathRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("operation"), TEXT("checkout_or_add"));
	Result->SetBoolField(TEXT("dry_run"), Options.bDryRun);
	Result->SetObjectField(TEXT("provider"), ProviderToJson(Provider));
	Result->SetArrayField(TEXT("paths"), PathRows);
	Result->SetNumberField(TEXT("input_count"), Inputs.Num());
	Result->SetNumberField(TEXT("file_count"), Files.Num());

	FString AvailabilityReason;
	if (!IsProviderAvailable(AvailabilityReason))
	{
		Result->SetBoolField(TEXT("ok"), Options.bUnavailableIsSuccess);
		Result->SetBoolField(TEXT("available"), false);
		Result->SetBoolField(TEXT("skipped"), true);
		Result->SetStringField(TEXT("message"), AvailabilityReason);
		return Result;
	}

	Result->SetBoolField(TEXT("available"), true);

	TArray<FString> FilesToCheckout;
	TArray<FString> FilesToAdd;
	TArray<TSharedPtr<FJsonValue>> Decisions;
	for (const FString& File : Files)
	{
		FSourceControlStatePtr State = Provider.GetState(File, EStateCacheUsage::ForceUpdate);
		TSharedPtr<FJsonObject> Decision = StateToJson(File, State);
		const bool bFileExists = IFileManager::Get().FileExists(*File);
		Decision->SetBoolField(TEXT("file_exists"), bFileExists);

		if (State.IsValid() && (State->IsCheckedOut() || State->IsAdded()))
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("skip"));
			Decision->SetStringField(TEXT("reason"), TEXT("already checked out or added"));
		}
		else if (!bFileExists && !Options.bAddMissingFiles)
		{
			Decision->SetStringField(TEXT("planned_action"), TEXT("skip"));
			Decision->SetStringField(TEXT("reason"), TEXT("file does not exist yet"));
		}
		else if (!State.IsValid() || (!State->IsSourceControlled() && State->CanAdd()))
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

	Result->SetArrayField(TEXT("decisions"), Decisions);
	Result->SetNumberField(TEXT("checkout_count"), FilesToCheckout.Num());
	Result->SetNumberField(TEXT("add_count"), FilesToAdd.Num());

	if (Options.bDryRun)
	{
		Result->SetBoolField(TEXT("ok"), true);
		return Result;
	}

	bool bOk = true;
	if (FilesToCheckout.Num() > 0)
	{
		FSourceControlOperationRef Operation = ISourceControlOperation::Create<FCheckOut>();
		const ECommandResult::Type CommandResult = Provider.Execute(Operation, FilesToCheckout, EConcurrency::Synchronous);
		Result->SetStringField(TEXT("checkout_result"), CommandResultToString(CommandResult));
		AddOperationMessages(Operation, Result, TEXT("checkout"));
		bOk = bOk && CommandResult == ECommandResult::Succeeded;
	}

	if (FilesToAdd.Num() > 0)
	{
		FSourceControlOperationRef Operation = ISourceControlOperation::Create<FMarkForAdd>();
		const ECommandResult::Type CommandResult = Provider.Execute(Operation, FilesToAdd, EConcurrency::Synchronous);
		Result->SetStringField(TEXT("add_result"), CommandResultToString(CommandResult));
		AddOperationMessages(Operation, Result, TEXT("add"));
		bOk = bOk && CommandResult == ECommandResult::Succeeded;
	}

	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetArrayField(TEXT("states"), GetStateRows(Provider, Files));
	return Result;
}

TSharedPtr<FJsonObject> FMonolithSourceControlUtils::CheckoutOrAddPackageNames(
	const TArray<FString>& PackageNames,
	const FMonolithSourceControlPrepareOptions& Options)
{
	TArray<FString> Inputs;
	Inputs.Reserve(PackageNames.Num());
	for (const FString& PackageName : PackageNames)
	{
		if (!PackageName.IsEmpty())
		{
			Inputs.AddUnique(PackageName);
		}
	}
	return CheckoutOrAddFiles(Inputs, Options);
}

TSharedPtr<FJsonObject> FMonolithSourceControlUtils::CheckoutOrAddPackage(
	UPackage* Package,
	const FMonolithSourceControlPrepareOptions& Options)
{
	TArray<FString> PackageNames;
	if (Package)
	{
		PackageNames.Add(Package->GetName());
	}
	return CheckoutOrAddPackageNames(PackageNames, Options);
}

TSharedPtr<FJsonObject> FMonolithSourceControlUtils::CheckoutOrAddAsset(
	UObject* Asset,
	const FMonolithSourceControlPrepareOptions& Options)
{
	return CheckoutOrAddPackage(Asset ? Asset->GetOutermost() : nullptr, Options);
}
