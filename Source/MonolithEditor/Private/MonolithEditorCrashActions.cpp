#include "MonolithEditorCrashActions.h"

#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithCrashBreadcrumb.h"

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
		if (Params.IsValid())
		{
			if (Params->HasField(TEXT("limit")))
			{
				Limit = FMath::Clamp((int32)Params->GetNumberField(TEXT("limit")), 1, 1000);
			}
			Params->TryGetStringField(TEXT("since"), Since);
			Params->TryGetStringField(TEXT("tool"), ToolFilter);
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
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("since"), Since);
			Params->TryGetStringField(TEXT("group_by"), GroupBy);
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
}
