#include "MonolithEditorActions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

namespace
{
	constexpr int32 DefaultValidationPlanLimit = 500;
	constexpr int32 MaxValidationPlanLimit = 5000;

	const TCHAR* ValidationOptionFields[] =
	{
		TEXT("validation_usecase"),
		TEXT("collect_per_asset_details"),
		TEXT("load_assets"),
		TEXT("load_external_objects"),
		TEXT("capture_logs"),
		TEXT("warnings_as_errors"),
		TEXT("skip_excluded_directories"),
		TEXT("max_assets_to_validate"),
		TEXT("silent")
	};

	TArray<TSharedPtr<FJsonValue>> StringArrayToJsonValues(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		JsonValues.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		return JsonValues;
	}

	bool ReadOptionalStringArray(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Field = Params->TryGetField(FieldName);
		if (!Field.IsValid() || Field->IsNull())
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			OutError = FString::Printf(TEXT("Param '%s' must be an array of strings."), FieldName);
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = FString::Printf(TEXT("Param '%s' must be an array of strings."), FieldName);
				return false;
			}
			StringValue.TrimStartAndEndInline();
			if (!StringValue.IsEmpty())
			{
				OutValues.AddUnique(StringValue);
			}
		}
		return true;
	}

	void CopyValidationOptions(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Target)
	{
		if (!Source.IsValid() || !Target.IsValid())
		{
			return;
		}

		for (const TCHAR* FieldName : ValidationOptionFields)
		{
			const TSharedPtr<FJsonValue> Value = Source->TryGetField(FieldName);
			if (Value.IsValid())
			{
				Target->SetField(FieldName, Value);
			}
		}

		const TSharedPtr<FJsonValue> UsecaseField = Target->TryGetField(TEXT("validation_usecase"));
		if (!UsecaseField.IsValid() || UsecaseField->IsNull())
		{
			Target->SetStringField(TEXT("validation_usecase"), TEXT("pre_submit"));
		}
	}

	bool IsDeleteAction(const FString& Action)
	{
		return Action.Equals(TEXT("delete"), ESearchCase::IgnoreCase)
			|| Action.Equals(TEXT("move/delete"), ESearchCase::IgnoreCase);
	}

	bool IsSourceFileExtension(const FString& Extension)
	{
		return Extension.Equals(TEXT("c"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("cc"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("cpp"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("cxx"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("h"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("hh"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("hpp"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("hxx"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("inl"), ESearchCase::IgnoreCase)
			|| Extension.Equals(TEXT("cs"), ESearchCase::IgnoreCase);
	}

	FString BestFilesystemPath(const TSharedPtr<FJsonObject>& Row)
	{
		for (const TCHAR* FieldName : { TEXT("local_path"), TEXT("client_file"), TEXT("depot_file"), TEXT("input") })
		{
			FString Path;
			Row->TryGetStringField(FieldName, Path);
			if (!Path.IsEmpty())
			{
				return Path;
			}
		}
		return FString();
	}

	TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Clone = MakeShared<FJsonObject>();
		if (!Source.IsValid())
		{
			return Clone;
		}

		for (const auto& Pair : Source->Values)
		{
			Clone->SetField(Pair.Key, Pair.Value);
		}
		return Clone;
	}

	void AddStringIfUnique(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Values.AddUnique(Value);
		}
	}

	void AddNextAction(
		TArray<TSharedPtr<FJsonValue>>& NextActions,
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		const FString& Reason)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("namespace"), Namespace);
		Row->SetStringField(TEXT("action"), Action);
		Row->SetStringField(TEXT("reason"), Reason);
		if (Params.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Params);
		}
		NextActions.Add(MakeShared<FJsonValueObject>(Row));
	}

	FMonolithActionResult ExecuteRequiredSourceControlAction(
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params)
	{
		if (!FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), Action))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("source_control.%s is not registered; content-validation planning requires the MonolithSourceControl module."), *Action),
				FMonolithJsonUtils::ErrMethodNotFound);
		}
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("source_control"), Action, Params);
	}

	void ClassifyMappedRow(
		const TSharedPtr<FJsonObject>& SourceRow,
		const FString& SourceKind,
		TArray<TSharedPtr<FJsonValue>>& Rows,
		TArray<TSharedPtr<FJsonValue>>& DeletedPackageRows,
		TArray<TSharedPtr<FJsonValue>>& SourceFileRows,
		TArray<TSharedPtr<FJsonValue>>& NonPackageRows,
		TArray<TSharedPtr<FJsonValue>>& UnmappedRows,
		TArray<FString>& ValidationPackages,
		TArray<FString>& DeletedPackages)
	{
		TSharedPtr<FJsonObject> Row = CloneJsonObject(SourceRow);
		Row->SetStringField(TEXT("source"), SourceKind);

		FString PackagePath;
		Row->TryGetStringField(TEXT("package_path"), PackagePath);
		FString Action;
		Row->TryGetStringField(TEXT("action"), Action);
		const TSharedPtr<FJsonValue> ValidField = Row->TryGetField(TEXT("valid"));
		bool bValidTemp = true;
		Row->TryGetBoolField(TEXT("valid"), bValidTemp);
		const bool bValid = (!ValidField.IsValid() || ValidField->IsNull()) || bValidTemp;
		bool bIsPackageTemp = !PackagePath.IsEmpty();
		Row->TryGetBoolField(TEXT("is_package"), bIsPackageTemp);
		const bool bIsPackage = bIsPackageTemp && !PackagePath.IsEmpty();
		const bool bDeleted = IsDeleteAction(Action);
		const FString FilesystemPath = BestFilesystemPath(Row);
		const FString Extension = FPaths::GetExtension(FilesystemPath).ToLower();

		Row->SetBoolField(TEXT("will_validate"), bValid && bIsPackage && !bDeleted);
		Row->SetBoolField(TEXT("deleted_package"), bValid && bIsPackage && bDeleted);
		Row->SetBoolField(TEXT("source_file"), bValid && !bIsPackage && IsSourceFileExtension(Extension));

		if (!bValid)
		{
			UnmappedRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (bIsPackage && bDeleted)
		{
			AddStringIfUnique(DeletedPackages, PackagePath);
			DeletedPackageRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		else if (bIsPackage)
		{
			AddStringIfUnique(ValidationPackages, PackagePath);
		}
		else if (IsSourceFileExtension(Extension))
		{
			SourceFileRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		else
		{
			NonPackageRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	FMonolithActionResult BuildContentValidationPlan(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("content-validation planning requires a params object."), FMonolithJsonUtils::ErrInvalidParams);
		}

		FString Error;
		TArray<FString> ExplicitPaths;
		if (!ReadOptionalStringArray(Params, TEXT("paths"), ExplicitPaths, Error)
			|| !ReadOptionalStringArray(Params, TEXT("packages"), ExplicitPaths, Error))
		{
			return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
		}

		FString Changelist;
		const TSharedPtr<FJsonValue> ChangelistField = Params->TryGetField(TEXT("changelist"));
		if (ChangelistField.IsValid() && !ChangelistField->IsNull())
		{
			if (!ChangelistField->TryGetString(Changelist))
			{
				return FMonolithActionResult::Error(TEXT("changelist must be a string."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
		Changelist.TrimStartAndEndInline();

		bool bHasIncludeOpened = false;
		bool bIncludeOpened = false;
		const TSharedPtr<FJsonValue> IncludeOpenedField = Params->TryGetField(TEXT("include_opened"));
		if (IncludeOpenedField.IsValid() && !IncludeOpenedField->IsNull())
		{
			if (IncludeOpenedField->TryGetBool(bIncludeOpened))
			{
				bHasIncludeOpened = true;
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("include_opened must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
		else
		{
			bIncludeOpened = !Changelist.IsEmpty() || ExplicitPaths.Num() == 0;
		}

		bool bResolvePackages = true;
		const TSharedPtr<FJsonValue> ResolvePackagesField = Params->TryGetField(TEXT("resolve_packages"));
		if (ResolvePackagesField.IsValid() && !ResolvePackagesField->IsNull())
		{
			if (!ResolvePackagesField->TryGetBool(bResolvePackages))
			{
				return FMonolithActionResult::Error(TEXT("resolve_packages must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		bool bIncludeNonPackages = true;
		const TSharedPtr<FJsonValue> IncludeNonPackagesField = Params->TryGetField(TEXT("include_non_packages"));
		if (IncludeNonPackagesField.IsValid() && !IncludeNonPackagesField->IsNull())
		{
			if (!IncludeNonPackagesField->TryGetBool(bIncludeNonPackages))
			{
				return FMonolithActionResult::Error(TEXT("include_non_packages must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		double LimitNumber = DefaultValidationPlanLimit;
		const TSharedPtr<FJsonValue> LimitField = Params->TryGetField(TEXT("limit"));
		if (LimitField.IsValid() && !LimitField->IsNull())
		{
			if (!LimitField->TryGetNumber(LimitNumber))
			{
				return FMonolithActionResult::Error(TEXT("limit must be a number."), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
		const int32 Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaxValidationPlanLimit);

		if (!bIncludeOpened && ExplicitPaths.Num() == 0)
		{
			return FMonolithActionResult::Error(
				TEXT("No validation planning inputs. Provide paths/packages or set include_opened=true."),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		TArray<TSharedPtr<FJsonValue>> DeletedPackageRows;
		TArray<TSharedPtr<FJsonValue>> SourceFileRows;
		TArray<TSharedPtr<FJsonValue>> NonPackageRows;
		TArray<TSharedPtr<FJsonValue>> UnmappedRows;
		TArray<FString> ValidationPackages;
		TArray<FString> DeletedPackages;
		TArray<TSharedPtr<FJsonValue>> Warnings;

		int32 OpenedCount = 0;
		bool bOpenedTruncated = false;

		if (bIncludeOpened)
		{
			TSharedPtr<FJsonObject> OpenedParams = MakeShared<FJsonObject>();
			OpenedParams->SetBoolField(TEXT("resolve_packages"), bResolvePackages);
			OpenedParams->SetNumberField(TEXT("limit"), Limit);
			if (!Changelist.IsEmpty())
			{
				OpenedParams->SetStringField(TEXT("changelist"), Changelist);
			}

			FMonolithActionResult OpenedResult = ExecuteRequiredSourceControlAction(TEXT("list_opened"), OpenedParams);
			if (!OpenedResult.bSuccess)
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("source_control.list_opened failed: %s"), *OpenedResult.ErrorMessage),
					OpenedResult.ErrorCode);
			}

			if (OpenedResult.Result.IsValid())
			{
				double Count = 0.0;
				const TSharedPtr<FJsonValue> CountField = OpenedResult.Result->TryGetField(TEXT("count"));
				if (CountField.IsValid() && !CountField->IsNull())
				{
					CountField->TryGetNumber(Count);
				}
				OpenedCount = static_cast<int32>(Count);
				const TSharedPtr<FJsonValue> TruncatedField = OpenedResult.Result->TryGetField(TEXT("truncated"));
				if (TruncatedField.IsValid() && !TruncatedField->IsNull())
				{
					TruncatedField->TryGetBool(bOpenedTruncated);
				}

				const TArray<TSharedPtr<FJsonValue>>* OpenedRows = nullptr;
				const TSharedPtr<FJsonValue> OpenedField = OpenedResult.Result->TryGetField(TEXT("opened"));
				if (OpenedField.IsValid() && !OpenedField->IsNull() && OpenedResult.Result->TryGetArrayField(TEXT("opened"), OpenedRows) && OpenedRows)
				{
					for (const TSharedPtr<FJsonValue>& Value : *OpenedRows)
					{
						const TSharedPtr<FJsonObject>* RowObj = nullptr;
						if (Value.IsValid() && Value->TryGetObject(RowObj) && RowObj && RowObj->IsValid())
						{
							ClassifyMappedRow(*RowObj, TEXT("p4_opened"), Rows, DeletedPackageRows, SourceFileRows, NonPackageRows, UnmappedRows, ValidationPackages, DeletedPackages);
						}
					}
				}
			}
		}

		if (ExplicitPaths.Num() > 0)
		{
			TSharedPtr<FJsonObject> MapParams = MakeShared<FJsonObject>();
			MapParams->SetArrayField(TEXT("paths"), StringArrayToJsonValues(ExplicitPaths));
			FMonolithActionResult MapResult = ExecuteRequiredSourceControlAction(TEXT("map_depot_paths"), MapParams);
			if (!MapResult.bSuccess)
			{
				return FMonolithActionResult::Error(
					FString::Printf(TEXT("source_control.map_depot_paths failed: %s"), *MapResult.ErrorMessage),
					MapResult.ErrorCode);
			}

			if (MapResult.Result.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* PathRows = nullptr;
				const TSharedPtr<FJsonValue> PathsField = MapResult.Result->TryGetField(TEXT("paths"));
				if (PathsField.IsValid() && !PathsField->IsNull() && MapResult.Result->TryGetArrayField(TEXT("paths"), PathRows) && PathRows)
				{
					for (const TSharedPtr<FJsonValue>& Value : *PathRows)
					{
						const TSharedPtr<FJsonObject>* RowObj = nullptr;
						if (Value.IsValid() && Value->TryGetObject(RowObj) && RowObj && RowObj->IsValid())
						{
							ClassifyMappedRow(*RowObj, TEXT("explicit_path"), Rows, DeletedPackageRows, SourceFileRows, NonPackageRows, UnmappedRows, ValidationPackages, DeletedPackages);
						}
					}
				}
			}
		}

		TSharedPtr<FJsonObject> ValidationParams = MakeShared<FJsonObject>();
		ValidationParams->SetArrayField(TEXT("packages"), StringArrayToJsonValues(ValidationPackages));
		CopyValidationOptions(Params, ValidationParams);

		if (bOpenedTruncated)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("Opened-file result was truncated at limit=%d; raise limit to include every opened file."), Limit)));
		}
		if (UnmappedRows.Num() > 0)
		{
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("Some inputs could not be mapped to local/package paths; inspect unmapped[].")));
		}

		TArray<TSharedPtr<FJsonValue>> NextActions;
		if (ValidationPackages.Num() > 0)
		{
			AddNextAction(NextActions, TEXT("editor"), TEXT("validate_assets"), ValidationParams,
				TEXT("Run DataValidation on the resolved package targets."));
		}
		if (SourceFileRows.Num() > 0)
		{
			TSharedPtr<FJsonObject> SourceHint = MakeShared<FJsonObject>();
			SourceHint->SetStringField(TEXT("note"), TEXT("Use source/project review actions for source-only changes; asset DataValidation only covers resolved packages."));
			AddNextAction(NextActions, TEXT("source"), TEXT("detect_changes"), SourceHint,
				TEXT("Optional source-impact review for code files in the changeset."));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetStringField(TEXT("changelist"), Changelist);
		Result->SetBoolField(TEXT("include_opened"), bIncludeOpened);
		Result->SetBoolField(TEXT("include_opened_explicit"), bHasIncludeOpened);
		Result->SetBoolField(TEXT("resolve_packages"), bResolvePackages);
		Result->SetBoolField(TEXT("include_non_packages"), bIncludeNonPackages);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("opened_count"), OpenedCount);
		Result->SetBoolField(TEXT("opened_truncated"), bOpenedTruncated);
		Result->SetNumberField(TEXT("explicit_path_count"), ExplicitPaths.Num());
		Result->SetNumberField(TEXT("row_count"), Rows.Num());
		Result->SetNumberField(TEXT("package_count"), ValidationPackages.Num());
		Result->SetNumberField(TEXT("deleted_package_count"), DeletedPackages.Num());
		Result->SetNumberField(TEXT("source_file_count"), SourceFileRows.Num());
		Result->SetNumberField(TEXT("non_package_count"), NonPackageRows.Num());
		Result->SetNumberField(TEXT("unmapped_count"), UnmappedRows.Num());
		Result->SetArrayField(TEXT("validation_packages"), StringArrayToJsonValues(ValidationPackages));
		Result->SetArrayField(TEXT("deleted_packages"), StringArrayToJsonValues(DeletedPackages));
		Result->SetArrayField(TEXT("rows"), Rows);
		Result->SetArrayField(TEXT("deleted_package_rows"), DeletedPackageRows);
		Result->SetArrayField(TEXT("source_files"), SourceFileRows);
		if (bIncludeNonPackages)
		{
			Result->SetArrayField(TEXT("non_package_files"), NonPackageRows);
		}
		Result->SetArrayField(TEXT("unmapped"), UnmappedRows);
		Result->SetObjectField(TEXT("validation_params"), ValidationParams);
		Result->SetArrayField(TEXT("next_actions"), NextActions);
		Result->SetArrayField(TEXT("warnings"), Warnings);
		return FMonolithActionResult::Success(Result);
	}
}

FMonolithActionResult FMonolithEditorActions::HandlePlanContentValidationChangeset(const TSharedPtr<FJsonObject>& Params)
{
	return BuildContentValidationPlan(Params);
}

FMonolithActionResult FMonolithEditorActions::HandleValidateChangesetAssets(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithActionResult PlanResult = BuildContentValidationPlan(Params);
	if (!PlanResult.bSuccess)
	{
		return PlanResult;
	}

	const TSharedPtr<FJsonObject>& Plan = PlanResult.Result;
	const TSharedPtr<FJsonObject>* ValidationParamsPtr = nullptr;
	if (!Plan.IsValid() || !Plan->TryGetObjectField(TEXT("validation_params"), ValidationParamsPtr) || !ValidationParamsPtr || !ValidationParamsPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("changeset validation planner did not produce validation_params."), -32603);
	}

	const TArray<TSharedPtr<FJsonValue>>* PackageValues = nullptr;
	const TSharedPtr<FJsonValue> PackagesField = (*ValidationParamsPtr)->TryGetField(TEXT("packages"));
	if (!PackagesField.IsValid() || PackagesField->IsNull() || !(*ValidationParamsPtr)->TryGetArrayField(TEXT("packages"), PackageValues) || !PackageValues || PackageValues->Num() == 0)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("ok"), true);
		Result->SetBoolField(TEXT("validation_skipped"), true);
		Result->SetStringField(TEXT("skip_reason"), TEXT("no_packages_resolved"));
		Result->SetObjectField(TEXT("plan"), Plan);
		return FMonolithActionResult::Success(Result);
	}

	FMonolithActionResult ValidationResult = HandleValidateAssets(*ValidationParamsPtr);
	if (!ValidationResult.bSuccess)
	{
		return ValidationResult;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	bool bValidationOk = false;
	if (ValidationResult.Result.IsValid())
	{
		ValidationResult.Result->TryGetBoolField(TEXT("ok"), bValidationOk);
	}
	Result->SetBoolField(TEXT("ok"), bValidationOk);
	Result->SetBoolField(TEXT("validation_skipped"), false);
	Result->SetObjectField(TEXT("plan"), Plan);
	Result->SetObjectField(TEXT("validation"), ValidationResult.Result);
	return FMonolithActionResult::Success(Result);
}
