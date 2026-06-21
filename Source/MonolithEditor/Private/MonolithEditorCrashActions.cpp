#include "MonolithEditorCrashActions.h"

#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithCrashBreadcrumb.h"
#include "MonolithParamUtils.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

namespace
{
	TSharedPtr<FJsonObject> ReadCrashFile(const FString& FullPath, FString& OutFileName)
	{
		OutFileName = FPaths::GetCleanFilename(FullPath);

		FString Body;
		if (!FFileHelper::LoadFileToString(Body, *FullPath))
		{
			return nullptr;
		}
		return FMonolithJsonUtils::Parse(Body);
	}

	FString GetCrashReportStatusPath()
	{
		return FPaths::Combine(FMonolithCrashBreadcrumb::GetCrashesDir(), TEXT("report_status.json"));
	}

	bool IsCrashReportStatusFile(const FString& FileName)
	{
		return FileName.Equals(TEXT("report_status.json"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonObject> LoadCrashReportStatus()
	{
		FString Body;
		if (!FFileHelper::LoadFileToString(Body, *GetCrashReportStatusPath()))
		{
			return MakeShared<FJsonObject>();
		}

		TSharedPtr<FJsonObject> Parsed = FMonolithJsonUtils::Parse(Body);
		return Parsed.IsValid() ? Parsed : MakeShared<FJsonObject>();
	}

	bool SaveCrashReportStatus(const TSharedPtr<FJsonObject>& StatusRoot, FString& OutError)
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString Dir = FMonolithCrashBreadcrumb::GetCrashesDir();
		if (!PF.DirectoryExists(*Dir))
		{
			PF.CreateDirectoryTree(*Dir);
		}

		const FString Body = FMonolithJsonUtils::Serialize(StatusRoot);
		if (!FFileHelper::SaveStringToFile(Body, *GetCrashReportStatusPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write crash report status: %s"), *GetCrashReportStatusPath());
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> GetCrashStatusEntry(const TSharedPtr<FJsonObject>& StatusRoot, const FString& FileName)
	{
		if (!StatusRoot.IsValid())
		{
			return nullptr;
		}

		TSharedPtr<FJsonValue>* Value = StatusRoot->Values.Find(*FileName);
		if (Value && Value->IsValid() && (*Value)->Type == EJson::Object)
		{
			return (*Value)->AsObject();
		}
		return nullptr;
	}

	FString GetCrashStatusString(const TSharedPtr<FJsonObject>& StatusRoot, const FString& FileName)
	{
		TSharedPtr<FJsonObject> Entry = GetCrashStatusEntry(StatusRoot, FileName);
		if (!Entry.IsValid())
		{
			return TEXT("unreported");
		}

		FString Status;
		return Entry->TryGetStringField(TEXT("status"), Status) ? Status : TEXT("unreported");
	}

	bool IsCrashIgnored(const TSharedPtr<FJsonObject>& StatusRoot, const FString& FileName)
	{
		TSharedPtr<FJsonObject> Entry = GetCrashStatusEntry(StatusRoot, FileName);
		bool bIgnored = false;
		return Entry.IsValid() && Entry->TryGetBoolField(TEXT("ignored"), bIgnored) && bIgnored;
	}

	void ApplyCrashReportStatus(const TSharedPtr<FJsonObject>& Crash, const TSharedPtr<FJsonObject>& StatusRoot, const FString& FileName)
	{
		if (!Crash.IsValid())
		{
			return;
		}

		Crash->SetStringField(TEXT("report_status"), GetCrashStatusString(StatusRoot, FileName));
		Crash->SetBoolField(TEXT("report_ignored"), IsCrashIgnored(StatusRoot, FileName));

		TSharedPtr<FJsonObject> Entry = GetCrashStatusEntry(StatusRoot, FileName);
		if (Entry.IsValid())
		{
			FString UpdatedAt;
			FString Note;
			if (Entry->TryGetStringField(TEXT("updated_at"), UpdatedAt))
			{
				Crash->SetStringField(TEXT("report_updated_at"), UpdatedAt);
			}
			if (Entry->TryGetStringField(TEXT("note"), Note) && !Note.IsEmpty())
			{
				Crash->SetStringField(TEXT("report_note"), Note);
			}
		}
	}

	bool ResolveCrashFileName(const FString& RequestedFile, FString& OutFileName, FString& OutError)
	{
		FString FileName = RequestedFile.TrimStartAndEnd();
		if (FileName.IsEmpty())
		{
			if (!FFileHelper::LoadFileToString(FileName, *FMonolithCrashBreadcrumb::GetLatestPointerPath()))
			{
				OutError = TEXT("No crash file was specified and latest.txt is unavailable.");
				return false;
			}
			FileName.TrimStartAndEndInline();
		}

		if (FileName.Contains(TEXT("/")) || FileName.Contains(TEXT("\\")) || FileName.Contains(TEXT("..")))
		{
			OutError = TEXT("file must be a crash JSON file name, not a path.");
			return false;
		}
		if (!FileName.EndsWith(TEXT(".json")))
		{
			OutError = TEXT("file must end with .json.");
			return false;
		}
		if (!FPaths::FileExists(FPaths::Combine(FMonolithCrashBreadcrumb::GetCrashesDir(), FileName)))
		{
			OutError = FString::Printf(TEXT("Crash file not found: %s"), *FileName);
			return false;
		}

		OutFileName = FileName;
		return true;
	}

	TSharedPtr<FJsonObject> BuildCrashReportPreview(
		const TSharedPtr<FJsonObject>& Crash,
		const FString& FileName,
		const TSharedPtr<FJsonObject>& StatusRoot,
		bool bIncludeParams)
	{
		TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetStringField(TEXT("file"), FileName);
		Report->SetStringField(TEXT("report_status"), GetCrashStatusString(StatusRoot, FileName));
		Report->SetBoolField(TEXT("report_ignored"), IsCrashIgnored(StatusRoot, FileName));

		const TCHAR* Fields[] = {
			TEXT("ts"),
			TEXT("kind"),
			TEXT("tool"),
			TEXT("action"),
			TEXT("monolith_version"),
			TEXT("engine_version"),
			TEXT("session_id"),
			TEXT("thread")
		};

		for (const TCHAR* Field : Fields)
		{
			FString Value;
			if (Crash->TryGetStringField(Field, Value))
			{
				Report->SetStringField(Field, Value);
			}
		}

		Report->SetBoolField(TEXT("params_included"), bIncludeParams);
		if (bIncludeParams)
		{
			FString Params;
			if (Crash->TryGetStringField(TEXT("params"), Params))
			{
				Report->SetStringField(TEXT("params"), Params);
			}
		}

		return Report;
	}

	// ---- editor.get_last_crash_reason ---------------------------------------
	FMonolithActionResult HandleGetLastCrashReason(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		const FString LatestPath = FMonolithCrashBreadcrumb::GetLatestPointerPath();
		if (!FPaths::FileExists(LatestPath))
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetBoolField(TEXT("found"), false);
			return FMonolithActionResult::Success(R);
		}

		FString LatestName;
		if (!FFileHelper::LoadFileToString(LatestName, *LatestPath))
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetBoolField(TEXT("found"), false);
			R->SetStringField(TEXT("error"), TEXT("latest.txt unreadable"));
			return FMonolithActionResult::Success(R);
		}
		LatestName.TrimStartAndEndInline();

		const FString FullPath = FPaths::Combine(
			FMonolithCrashBreadcrumb::GetCrashesDir(), LatestName);

		FString FileName;
		TSharedPtr<FJsonObject> Crash = ReadCrashFile(FullPath, FileName);
		if (!Crash.IsValid())
		{
			TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetBoolField(TEXT("found"), false);
			R->SetStringField(TEXT("error"),
				FString::Printf(TEXT("Could not read crash file: %s"), *LatestName));
			return FMonolithActionResult::Success(R);
		}

		Crash->SetBoolField(TEXT("found"), true);
		Crash->SetStringField(TEXT("file"), FileName);
		return FMonolithActionResult::Success(Crash);
	}

	// ---- editor.list_recent_crashes -----------------------------------------
	FMonolithActionResult HandleListRecentCrashes(const TSharedPtr<FJsonObject>& Params)
	{
		int32 Limit = 20;
		FString Since;
		FString ToolFilter;
		FString Error;

		if (Params.IsValid())
		{
			double LimitValue = 20.0;
			if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("limit"), LimitValue, Error, 20.0, 1.0, 1000.0))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
			Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 1000);

			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("since"), Since, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}

			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("tool"), ToolFilter, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
		}

		const FString Dir = FMonolithCrashBreadcrumb::GetCrashesDir();

		TArray<FString> JsonFiles;
		IFileManager::Get().FindFiles(JsonFiles, *(Dir / TEXT("*.json")), true, false);
		// FindFiles returns names only — sort descending so latest first.
		JsonFiles.Sort([](const FString& A, const FString& B) { return A > B; });

		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(FMath::Min(Limit, JsonFiles.Num()));

		for (const FString& Name : JsonFiles)
		{
			if (Items.Num() >= Limit) break;
			if (IsCrashReportStatusFile(Name))
			{
				continue;
			}

			const FString FullPath = FPaths::Combine(Dir, Name);
			FString FileName;
			TSharedPtr<FJsonObject> Crash = ReadCrashFile(FullPath, FileName);
			if (!Crash.IsValid()) continue;

			// Optional filtering
			if (!Since.IsEmpty())
			{
				FString Ts;
				if (Crash->TryGetStringField(TEXT("ts"), Ts) && Ts < Since) continue;
			}
			if (!ToolFilter.IsEmpty())
			{
				FString Tool;
				FString Action;
				Crash->TryGetStringField(TEXT("tool"), Tool);
				Crash->TryGetStringField(TEXT("action"), Action);
				const FString Composite = Tool + TEXT(".") + Action;
				if (!Composite.Contains(ToolFilter) &&
					!Tool.Contains(ToolFilter) &&
					!Action.Contains(ToolFilter))
				{
					continue;
				}
			}

			Crash->SetStringField(TEXT("file"), FileName);
			Items.Add(MakeShared<FJsonValueObject>(Crash));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("crashes"), Items);
		Result->SetNumberField(TEXT("count"), Items.Num());
		Result->SetNumberField(TEXT("total_files_on_disk"), JsonFiles.Num());
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.get_crash_stats ---------------------------------------------
	FMonolithActionResult HandleGetCrashStats(const TSharedPtr<FJsonObject>& Params)
	{
		FString Since;
		FString GroupBy = TEXT("tool");
		FString Error;

		if (Params.IsValid())
		{
			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("since"), Since, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}

			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("group_by"), GroupBy, Error, TEXT("tool")))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
		}

		const FString Dir = FMonolithCrashBreadcrumb::GetCrashesDir();
		TArray<FString> JsonFiles;
		IFileManager::Get().FindFiles(JsonFiles, *(Dir / TEXT("*.json")), true, false);

		struct FBucket
		{
			int32 Count = 0;
			FString First;
			FString Last;
		};
		TMap<FString, FBucket> Buckets;
		int32 Total = 0;

		for (const FString& Name : JsonFiles)
		{
			if (IsCrashReportStatusFile(Name))
			{
				continue;
			}

			const FString FullPath = FPaths::Combine(Dir, Name);
			FString FileName;
			TSharedPtr<FJsonObject> Crash = ReadCrashFile(FullPath, FileName);
			if (!Crash.IsValid()) continue;

			FString Ts;
			Crash->TryGetStringField(TEXT("ts"), Ts);
			if (!Since.IsEmpty() && !Ts.IsEmpty() && Ts < Since) continue;

			FString Tool;
			FString Action;
			Crash->TryGetStringField(TEXT("tool"), Tool);
			Crash->TryGetStringField(TEXT("action"), Action);

			FString Key;
			if (GroupBy.Equals(TEXT("action"), ESearchCase::IgnoreCase))
			{
				Key = Action;
			}
			else if (GroupBy.Equals(TEXT("tool_action"), ESearchCase::IgnoreCase))
			{
				Key = Tool + TEXT(".") + Action;
			}
			else
			{
				Key = Tool;
			}

			FBucket& B = Buckets.FindOrAdd(Key);
			B.Count += 1;
			if (B.First.IsEmpty() || Ts < B.First) B.First = Ts;
			if (B.Last.IsEmpty()  || Ts > B.Last)  B.Last  = Ts;
			++Total;
		}

		TSharedPtr<FJsonObject> By = MakeShared<FJsonObject>();
		for (const TPair<FString, FBucket>& Pair : Buckets)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("count"), Pair.Value.Count);
			if (!Pair.Value.First.IsEmpty()) Entry->SetStringField(TEXT("first"), Pair.Value.First);
			if (!Pair.Value.Last.IsEmpty())  Entry->SetStringField(TEXT("last"),  Pair.Value.Last);
			By->SetObjectField(Pair.Key.IsEmpty() ? TEXT("(unknown)") : Pair.Key, Entry);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetStringField(TEXT("group_by"), GroupBy);
		if (!Since.IsEmpty()) Result->SetStringField(TEXT("since"), Since);
		Result->SetObjectField(TEXT("by"), By);
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.get_crash_report_settings -----------------------------------
	FMonolithActionResult HandleGetCrashReportSettings(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("mode"), TEXT("local_only"));
		Result->SetBoolField(TEXT("external_reporting_enabled"), false);
		Result->SetBoolField(TEXT("external_endpoint_configured"), false);
		Result->SetBoolField(TEXT("include_editor_log"), false);
		Result->SetBoolField(TEXT("include_params_by_default"), false);
		Result->SetStringField(TEXT("status_path"), GetCrashReportStatusPath());
		Result->SetStringField(TEXT("note"), TEXT("Phase 1 keeps crash reports local and never attempts network upload."));
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.list_reportable_crashes -------------------------------------
	FMonolithActionResult HandleListReportableCrashes(const TSharedPtr<FJsonObject>& Params)
	{
		int32 Limit = 20;
		FString Since;
		FString ToolFilter;
		bool bIncludeIgnored = false;
		FString Error;

		if (Params.IsValid())
		{
			double LimitValue = 20.0;
			if (!MonolithParamUtils::GetOptionalClampedDoubleParam(Params, TEXT("limit"), LimitValue, Error, 20.0, 1.0, 1000.0))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
			Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 1000);

			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("since"), Since, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}

			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("tool"), ToolFilter, Error))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}

			if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("include_ignored"), bIncludeIgnored, Error, false))
			{
				return FMonolithActionResult::Error(Error, -32602);
			}
		}

		const FString Dir = FMonolithCrashBreadcrumb::GetCrashesDir();
		TSharedPtr<FJsonObject> StatusRoot = LoadCrashReportStatus();

		TArray<FString> JsonFiles;
		IFileManager::Get().FindFiles(JsonFiles, *(Dir / TEXT("*.json")), true, false);
		JsonFiles.Sort([](const FString& A, const FString& B) { return A > B; });

		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(FMath::Min(Limit, JsonFiles.Num()));
		int32 IgnoredSkipped = 0;

		for (const FString& Name : JsonFiles)
		{
			if (Items.Num() >= Limit) break;
			if (IsCrashReportStatusFile(Name))
			{
				continue;
			}
			if (!bIncludeIgnored && IsCrashIgnored(StatusRoot, Name))
			{
				++IgnoredSkipped;
				continue;
			}

			const FString FullPath = FPaths::Combine(Dir, Name);
			FString FileName;
			TSharedPtr<FJsonObject> Crash = ReadCrashFile(FullPath, FileName);
			if (!Crash.IsValid()) continue;

			if (!Since.IsEmpty())
			{
				FString Ts;
				if (Crash->TryGetStringField(TEXT("ts"), Ts) && Ts < Since) continue;
			}
			if (!ToolFilter.IsEmpty())
			{
				FString Tool;
				FString Action;
				Crash->TryGetStringField(TEXT("tool"), Tool);
				Crash->TryGetStringField(TEXT("action"), Action);
				const FString Composite = Tool + TEXT(".") + Action;
				if (!Composite.Contains(ToolFilter) &&
					!Tool.Contains(ToolFilter) &&
					!Action.Contains(ToolFilter))
				{
					continue;
				}
			}

			Crash->SetStringField(TEXT("file"), FileName);
			ApplyCrashReportStatus(Crash, StatusRoot, FileName);
			Items.Add(MakeShared<FJsonValueObject>(Crash));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("crashes"), Items);
		Result->SetNumberField(TEXT("count"), Items.Num());
		Result->SetNumberField(TEXT("total_files_on_disk"), JsonFiles.Num());
		Result->SetNumberField(TEXT("ignored_skipped"), IgnoredSkipped);
		Result->SetBoolField(TEXT("external_reporting_enabled"), false);
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.preview_crash_report ----------------------------------------
	FMonolithActionResult HandlePreviewCrashReport(const TSharedPtr<FJsonObject>& Params)
	{
		FString RequestedFile;
		bool bIncludeParams = false;
		FString ErrorParam;
		if (Params.IsValid())
		{
			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("file"), RequestedFile, ErrorParam))
			{
				return FMonolithActionResult::Error(ErrorParam, -32602);
			}

			if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("include_params"), bIncludeParams, ErrorParam, false))
			{
				return FMonolithActionResult::Error(ErrorParam, -32602);
			}
		}

		FString FileName;
		FString Error;
		if (!ResolveCrashFileName(RequestedFile, FileName, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		FString LoadedName;
		TSharedPtr<FJsonObject> Crash = ReadCrashFile(FPaths::Combine(FMonolithCrashBreadcrumb::GetCrashesDir(), FileName), LoadedName);
		if (!Crash.IsValid())
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("Could not parse crash file: %s"), *FileName));
		}

		TSharedPtr<FJsonObject> StatusRoot = LoadCrashReportStatus();
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("would_upload"), false);
		Result->SetBoolField(TEXT("external_request_attempted"), false);
		Result->SetStringField(TEXT("mode"), TEXT("local_only"));
		Result->SetStringField(TEXT("reason"), TEXT("External crash reporting is disabled in Phase 1."));
		Result->SetObjectField(TEXT("report"), BuildCrashReportPreview(Crash, FileName, StatusRoot, bIncludeParams));
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.submit_crash_report -----------------------------------------
	FMonolithActionResult HandleSubmitCrashReport(const TSharedPtr<FJsonObject>& Params)
	{
		FString RequestedFile;
		FString ErrorParam;
		if (Params.IsValid())
		{
			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("file"), RequestedFile, ErrorParam))
			{
				return FMonolithActionResult::Error(ErrorParam, -32602);
			}
		}

		FString FileName;
		FString Error;
		if (!ResolveCrashFileName(RequestedFile, FileName, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		TSharedPtr<FJsonObject> StatusRoot = LoadCrashReportStatus();
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("status"), TEXT("blocked_external_reporting_disabled"));
		Entry->SetBoolField(TEXT("ignored"), false);
		Entry->SetStringField(TEXT("updated_at"), FDateTime::UtcNow().ToIso8601());
		Entry->SetStringField(TEXT("note"), TEXT("External crash reporting is disabled; no network request was attempted."));
		StatusRoot->SetObjectField(FileName, Entry);

		if (!SaveCrashReportStatus(StatusRoot, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("file"), FileName);
		Result->SetBoolField(TEXT("submitted"), false);
		Result->SetBoolField(TEXT("external_request_attempted"), false);
		Result->SetStringField(TEXT("status"), TEXT("blocked_external_reporting_disabled"));
		Result->SetStringField(TEXT("status_path"), GetCrashReportStatusPath());
		return FMonolithActionResult::Success(Result);
	}

	// ---- editor.mark_crash_ignored ------------------------------------------
	FMonolithActionResult HandleMarkCrashIgnored(const TSharedPtr<FJsonObject>& Params)
	{
		FString RequestedFile;
		FString Note;
		bool bIgnored = true;
		FString ErrorParam;
		if (!MonolithParamUtils::GetRequiredStringParam(Params, TEXT("file"), RequestedFile, ErrorParam))
		{
			return FMonolithActionResult::Error(ErrorParam, -32602);
		}
		if (Params.IsValid())
		{
			if (!MonolithParamUtils::GetOptionalStringParam(Params, TEXT("note"), Note, ErrorParam))
			{
				return FMonolithActionResult::Error(ErrorParam, -32602);
			}
			if (!MonolithParamUtils::GetOptionalBoolParam(Params, TEXT("ignored"), bIgnored, ErrorParam, true))
			{
				return FMonolithActionResult::Error(ErrorParam, -32602);
			}
		}

		FString FileName;
		FString Error;
		if (!ResolveCrashFileName(RequestedFile, FileName, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		TSharedPtr<FJsonObject> StatusRoot = LoadCrashReportStatus();
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("status"), bIgnored ? TEXT("ignored") : TEXT("unreported"));
		Entry->SetBoolField(TEXT("ignored"), bIgnored);
		Entry->SetStringField(TEXT("updated_at"), FDateTime::UtcNow().ToIso8601());
		if (!Note.IsEmpty())
		{
			Entry->SetStringField(TEXT("note"), Note);
		}
		StatusRoot->SetObjectField(FileName, Entry);

		if (!SaveCrashReportStatus(StatusRoot, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("file"), FileName);
		Result->SetBoolField(TEXT("ignored"), bIgnored);
		Result->SetStringField(TEXT("status"), bIgnored ? TEXT("ignored") : TEXT("unreported"));
		Result->SetStringField(TEXT("status_path"), GetCrashReportStatusPath());
		return FMonolithActionResult::Success(Result);
	}

#if !UE_BUILD_SHIPPING
	// DEV-ONLY probe (working tree only, NOT committed). Fires ensure(false)
	// inside the ScopedCapture window to verify the breadcrumb pipeline
	// without taking the editor down. Editor stays alive after this call.
	FMonolithActionResult HandleDevTriggerEnsure(const TSharedPtr<FJsonObject>& /*Params*/)
	{
		ensureMsgf(false,
			TEXT("CrashRecovery dev probe: triggering ensure to exercise breadcrumb capture (non-fatal)"));

		auto R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("triggered"), TEXT("ensure"));
		R->SetBoolField(TEXT("editor_alive_after"), true);
		return FMonolithActionResult::Success(R);
	}
#endif
}

void FMonolithEditorCrashActions::RegisterActions()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

#if !UE_BUILD_SHIPPING
	Registry.RegisterAction(TEXT("editor"), TEXT("dev_trigger_ensure"),
		TEXT("DEV ONLY: Fires ensure(false) inside the breadcrumb scope to exercise the CrashRecovery capture pipeline. Editor stays alive."),
		FMonolithActionHandler::CreateStatic(&HandleDevTriggerEnsure),
		MakeShared<FJsonObject>());
#endif

	Registry.RegisterAction(TEXT("editor"), TEXT("get_last_crash_reason"),
		TEXT("Return the most recent Monolith crash breadcrumb (tool, action, params, timestamp). "
			 "Returns {found:false} if no crash has been recorded."),
		FMonolithActionHandler::CreateStatic(&HandleGetLastCrashReason),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_recent_crashes"),
		TEXT("List recent Monolith crash breadcrumbs newest-first. "
			 "Optional filters: limit (default 20, max 1000), since (ISO8601), tool (substring)."),
		FMonolithActionHandler::CreateStatic(&HandleListRecentCrashes),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"),  TEXT("integer"), TEXT("Max items"), TEXT("20"))
			.Optional(TEXT("since"),  TEXT("string"),  TEXT("Filter to crashes at/after this ISO8601 timestamp"))
			.Optional(TEXT("tool"),   TEXT("string"),  TEXT("Filter to entries whose tool/action contains this substring"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_crash_stats"),
		TEXT("Aggregate Monolith crash counts grouped by tool, action, or tool_action. "
			 "Optional 'since' filter (ISO8601). Useful for spotting recurrent crash sources."),
		FMonolithActionHandler::CreateStatic(&HandleGetCrashStats),
		FParamSchemaBuilder()
			.Optional(TEXT("since"),    TEXT("string"), TEXT("Only include crashes at/after this ISO8601 timestamp"))
			.Optional(TEXT("group_by"), TEXT("string"), TEXT("'tool' (default), 'action', or 'tool_action'"), TEXT("tool"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_crash_report_settings"),
		TEXT("Return local-only crash reporting settings. Phase 1 never attempts network upload."),
		FMonolithActionHandler::CreateStatic(&HandleGetCrashReportSettings),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_reportable_crashes"),
		TEXT("List crash breadcrumbs with local report status. Optional filters: limit, since, tool, include_ignored."),
		FMonolithActionHandler::CreateStatic(&HandleListReportableCrashes),
		FParamSchemaBuilder()
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max items"), TEXT("20"))
			.Optional(TEXT("since"), TEXT("string"), TEXT("Filter to crashes at/after this ISO8601 timestamp"))
			.Optional(TEXT("tool"), TEXT("string"), TEXT("Filter to entries whose tool/action contains this substring"))
			.Optional(TEXT("include_ignored"), TEXT("bool"), TEXT("Include crashes marked ignored"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("preview_crash_report"),
		TEXT("Preview the local crash report metadata that would be considered for a manual report. No network request is attempted."),
		FMonolithActionHandler::CreateStatic(&HandlePreviewCrashReport),
		FParamSchemaBuilder()
			.Optional(TEXT("file"), TEXT("string"), TEXT("Crash JSON file name. Defaults to latest.txt"))
			.Optional(TEXT("include_params"), TEXT("bool"), TEXT("Include serialized action params in preview"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("submit_crash_report"),
		TEXT("Local-only Phase 1 submit stub: records that external reporting is disabled and makes no network request."),
		FMonolithActionHandler::CreateStatic(&HandleSubmitCrashReport),
		FParamSchemaBuilder()
			.Optional(TEXT("file"), TEXT("string"), TEXT("Crash JSON file name. Defaults to latest.txt"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("mark_crash_ignored"),
		TEXT("Mark or unmark a crash breadcrumb as ignored in local report status."),
		FMonolithActionHandler::CreateStatic(&HandleMarkCrashIgnored),
		FParamSchemaBuilder()
			.Required(TEXT("file"), TEXT("string"), TEXT("Crash JSON file name"))
			.Optional(TEXT("ignored"), TEXT("bool"), TEXT("Whether the crash should be ignored"), TEXT("true"))
			.Optional(TEXT("note"), TEXT("string"), TEXT("Optional local note"))
			.Build());
}
