#include "MonolithEditorActions.h"
#include "MonolithEditorGifTiming.h"
#include "MonolithAssetUtils.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithProjectionUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "EditorValidatorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/DataValidation.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h" // resolve virtual /Game output dirs to on-disk paths (capture footgun fix)
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/AutomationTest.h"
#include "Algo/Reverse.h"
#include "MonolithPackagePathValidator.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Item 8: fix_hints — borrow the shared source DB (LNK2019 owner-module +
// C4996 deprecation resolution). MonolithEditor already PrivateDependsOn
// MonolithSource (MonolithEditor.Build.cs) — one-way Editor -> Source borrow.
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSubsystem.h"
#include "SQLiteDatabase.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

// Capture action includes
#include "ProceduralMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AdvancedPreviewScene.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "NiagaraWorldManager.h"
#include "IImageWrapperModule.h"
#include "IImageWrapper.h"
#include "ImageUtils.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "Engine/Texture2D.h"
#include "RenderingThread.h"
#include "ShaderCompiler.h"
#include "TextureResource.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSingleNodeInstance.h"
// #15 capture_anim_frames: skeletal-anim preview (AnimSequence / BlendSpace / AnimBlueprint).
#include "Animation/DebugSkelMeshComponent.h" // UDebugSkelMeshComponent (UnrealEd)
#include "Animation/BlendSpace.h"             // UBlendSpace
#include "Animation/AnimBlueprint.h"          // UAnimBlueprint (GeneratedClass)
#include "Materials/MaterialInstanceDynamic.h"
// asset_type=widget (Phase 1 expansion)
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
#include "Framework/Application/SlateApplication.h"
#include "WidgetBlueprint.h"
#include "Slate/WidgetRenderer.h"
#include "RenderDeferredCleanup.h"
#include "UObject/SavePackage.h"
#include "LevelEditorViewport.h"
#include "PixelFormat.h"
#include "ObjectTools.h"
#include "Misc/ObjectThumbnail.h"
// delete_assets: unattended-guard pattern so non-interactive deletes never raise
// a modal Slate dialog (which would freeze the game thread / in-process MCP server).
#include "CoreGlobals.h"                     // GIsRunningUnattendedScript
#include "UObject/Package.h"                 // UPackage::SetDirtyFlag
#include "Subsystems/AssetEditorSubsystem.h" // CloseAllEditorsForAsset

// Scripting action includes (HOFF 7)
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "LevelEditorSubsystem.h"
#include "Editor.h"

// run_console_command needs world / PC access
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"

// PIE pre-flight compile gate + list_errored_blueprints: iterate loaded UBlueprints
// and test the same {BS_Error, bDisplayCompilePIEWarning} pair the engine's
// ResolveDirtyBlueprints uses (PlayLevel.cpp) to decide whether to raise the blocking
// "unresolved compiler errors" PIE prompt. GIsRunningUnattendedScript (CoreGlobals.h,
// included above) + TGuardValue (UnrealTemplate.h) suppress that modal in suppress mode.
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"
#include "Templates/UnrealTemplate.h"

// start_pie needs the level-editor module + asset viewport to pin PIE to in-viewport mode
#include "LevelEditor.h"
#include "IAssetViewport.h"
#include "Modules/ModuleManager.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"

// run_pie_smoke / poll_pie_smoke / stop_pie_smoke / capture_pie_movement_clip:
// async session-based PIE smoke advanced by the editor's real frame loop.
#include "MonolithPieSmokeSession.h"
#include "Animation/AnimInstance.h"
#include "GameFramework/Pawn.h"
#include "Misc/ScopeExit.h"           // ON_SCOPE_EXIT (always-unbind the PostPIEStarted handle)
#include "UObject/GarbageCollection.h" // CollectGarbage / GARBAGE_COLLECTION_KEEPFLAGS (#5/#6 world-leak fix)

// create_nav_harness_map: actor spawning + reflective property set + registry dispatch
#include "Engine/StaticMeshActor.h"
#include "Camera/CameraActor.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPath.h"
// Phase 10 (OG-E4): map post-authoring — WorldSettings GameMode override + PlayerStart spawn.
#include "GameFramework/WorldSettings.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerStart.h"

// --- Compile state ---

FMonolithLogCapture* FMonolithEditorActions::CachedLogCapture = nullptr;
double FMonolithEditorActions::LastCompileTimestamp = 0.0;
FString FMonolithEditorActions::LastCompileResult = TEXT("none");
bool FMonolithEditorActions::bIsCompiling = false;
bool FMonolithEditorActions::bPatchApplied = false;
double FMonolithEditorActions::LastCompileEndTimestamp = 0.0;
TSharedPtr<FJsonObject> FMonolithEditorActions::LastAutomationRun;
double FMonolithEditorActions::LastAutomationRunTimestamp = 0.0;
TSharedPtr<FJsonObject> FMonolithEditorActions::CurrentAutomationRun;
TArray<TSharedPtr<FJsonObject>> FMonolithEditorActions::AutomationRunHistory;
bool FMonolithEditorActions::bAutomationRunActive = false;

// --- Log capture ---

FMonolithLogCapture* FMonolithEditorActions::GetLogCapture()
{
	return CachedLogCapture;
}

void FMonolithLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);

	FMonolithLogEntry Entry;
	Entry.Sequence = NextSequence++;
	Entry.Timestamp = FPlatformTime::Seconds();
	Entry.Category = Category;
	Entry.Verbosity = Verbosity;
	Entry.Message = V;

	if (RingBuffer.Num() < MaxEntries)
	{
		RingBuffer.Add(MoveTemp(Entry));
	}
	else
	{
		RingBuffer[WriteIndex] = MoveTemp(Entry);
		bWrapped = true;
	}
	WriteIndex = (WriteIndex + 1) % MaxEntries;

	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: ++TotalFatal; break;
	case ELogVerbosity::Error: ++TotalError; break;
	case ELogVerbosity::Warning: ++TotalWarning; break;
	case ELogVerbosity::Display:
	case ELogVerbosity::Log: ++TotalLog; break;
	case ELogVerbosity::Verbose:
	case ELogVerbosity::VeryVerbose: ++TotalVerbose; break;
	default: break;
	}
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetRecentEntries(int32 Count) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;
	int32 Num = FMath::Min(Count, Total);
	int32 Begin = bWrapped ? (WriteIndex - Num + Total) % Total : FMath::Max(0, Total - Num);

	Result.Reserve(Num);
	for (int32 i = 0; i < Num; ++i)
	{
		int32 Idx = (Begin + i) % Total;
		Result.Add(RingBuffer[Idx]);
	}
	return Result;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::SearchEntries(const FString& Pattern, const FString& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	FString PatternLower = Pattern.ToLower();
	int32 Total = RingBuffer.Num();
	int32 Latest = bWrapped ? (WriteIndex - 1 + Total) % Total : Total - 1;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Latest - i + Total) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Verbosity > MaxVerbosity) continue;
		if (!CategoryFilter.IsEmpty() && Entry.Category != FName(*CategoryFilter)) continue;
		if (!PatternLower.IsEmpty() && !Entry.Message.ToLower().Contains(PatternLower)) continue;

		Result.Add(Entry);
	}

	Algo::Reverse(Result);
	return Result;
}

TArray<FString> FMonolithLogCapture::GetActiveCategories() const
{
	FScopeLock ScopeLock(&Lock);
	TSet<FString> Categories;
	for (const FMonolithLogEntry& Entry : RingBuffer)
	{
		Categories.Add(Entry.Category.ToString());
	}
	return Categories.Array();
}

int32 FMonolithLogCapture::GetCountByVerbosity(ELogVerbosity::Type Verbosity) const
{
	FScopeLock ScopeLock(&Lock);
	switch (Verbosity)
	{
	case ELogVerbosity::Fatal: return TotalFatal;
	case ELogVerbosity::Error: return TotalError;
	case ELogVerbosity::Warning: return TotalWarning;
	case ELogVerbosity::Log: return TotalLog;
	case ELogVerbosity::Verbose: return TotalVerbose;
	default: return 0;
	}
}

int32 FMonolithLogCapture::GetTotalCount() const
{
	FScopeLock ScopeLock(&Lock);
	return TotalFatal + TotalError + TotalWarning + TotalLog + TotalVerbose;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetEntriesSince(double SinceTimestamp, const TArray<FName>& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Timestamp < SinceTimestamp) continue;
		if (Entry.Verbosity > MaxVerbosity) continue;
		if (CategoryFilter.Num() > 0 && !CategoryFilter.Contains(Entry.Category)) continue;

		Result.Add(Entry);
	}
	return Result;
}

TArray<FMonolithLogEntry> FMonolithLogCapture::GetEntriesAfter(int64 Sequence, const TArray<FName>& CategoryFilter, ELogVerbosity::Type MaxVerbosity, int32 Limit) const
{
	FScopeLock ScopeLock(&Lock);
	TArray<FMonolithLogEntry> Result;

	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total && Result.Num() < Limit; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];

		if (Entry.Sequence <= Sequence) continue;
		if (Entry.Verbosity > MaxVerbosity) continue;
		if (CategoryFilter.Num() > 0 && !CategoryFilter.Contains(Entry.Category)) continue;

		Result.Add(Entry);
	}
	return Result;
}

int64 FMonolithLogCapture::GetLatestSequence() const
{
	FScopeLock ScopeLock(&Lock);
	return NextSequence - 1;
}

int32 FMonolithLogCapture::CountErrorsSince(double SinceTimestamp) const
{
	FScopeLock ScopeLock(&Lock);
	int32 Count = 0;
	int32 Total = RingBuffer.Num();
	int32 Start = bWrapped ? WriteIndex : 0;

	for (int32 i = 0; i < Total; ++i)
	{
		int32 Idx = (Start + i) % Total;
		const FMonolithLogEntry& Entry = RingBuffer[Idx];
		if (Entry.Timestamp >= SinceTimestamp && Entry.Verbosity <= ELogVerbosity::Error)
		{
			++Count;
		}
	}
	return Count;
}

// --- Helpers ---

static FString VerbosityToString(ELogVerbosity::Type V)
{
	switch (V)
	{
	case ELogVerbosity::Fatal: return TEXT("fatal");
	case ELogVerbosity::Error: return TEXT("error");
	case ELogVerbosity::Warning: return TEXT("warning");
	case ELogVerbosity::Display: return TEXT("display");
	case ELogVerbosity::Log: return TEXT("log");
	case ELogVerbosity::Verbose: return TEXT("verbose");
	case ELogVerbosity::VeryVerbose: return TEXT("very_verbose");
	default: return TEXT("unknown");
	}
}

static ELogVerbosity::Type StringToVerbosity(const FString& S)
{
	if (S == TEXT("fatal")) return ELogVerbosity::Fatal;
	if (S == TEXT("error")) return ELogVerbosity::Error;
	if (S == TEXT("warning")) return ELogVerbosity::Warning;
	if (S == TEXT("display")) return ELogVerbosity::Display;
	if (S == TEXT("verbose")) return ELogVerbosity::Verbose;
	if (S == TEXT("very_verbose")) return ELogVerbosity::VeryVerbose;
	return ELogVerbosity::Log;
}

static TSharedPtr<FJsonObject> LogEntryToJson(const FMonolithLogEntry& Entry)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("sequence"), static_cast<double>(Entry.Sequence));
	Obj->SetNumberField(TEXT("timestamp"), Entry.Timestamp);
	Obj->SetStringField(TEXT("category"), Entry.Category.ToString());
	Obj->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
	Obj->SetStringField(TEXT("message"), Entry.Message);
	return Obj;
}

// --- Live Coding delegate ---

void FMonolithEditorActions::InitLiveCodingDelegate()
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LC = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LC)
	{
		LC->GetOnPatchCompleteDelegate().AddStatic(&FMonolithEditorActions::OnLiveCodingPatchComplete);
	}
#endif
}

void FMonolithEditorActions::OnLiveCodingPatchComplete()
{
	bIsCompiling = false;
	bPatchApplied = true;
	LastCompileResult = TEXT("success");
	LastCompileEndTimestamp = FPlatformTime::Seconds();
}

static FString TimestampToIso(double PlatformSeconds)
{
	if (PlatformSeconds <= 0.0) return TEXT("never");
	FDateTime Now = FDateTime::UtcNow();
	double CurrentSeconds = FPlatformTime::Seconds();
	double Delta = CurrentSeconds - PlatformSeconds;
	FDateTime EventTime = Now - FTimespan::FromSeconds(Delta);
	return EventTime.ToIso8601();
}

static FString NormalizeLiveCodingCompileResult(const FString& RawResult)
{
	if (RawResult.Equals(TEXT("success"), ESearchCase::IgnoreCase) ||
		RawResult.Equals(TEXT("no_changes"), ESearchCase::IgnoreCase))
	{
		return TEXT("success");
	}
	if (RawResult.Equals(TEXT("failure"), ESearchCase::IgnoreCase) ||
		RawResult.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
	{
		return TEXT("failed");
	}
	if (RawResult.Equals(TEXT("in_progress"), ESearchCase::IgnoreCase) ||
		RawResult.Equals(TEXT("compile_still_active"), ESearchCase::IgnoreCase))
	{
		return TEXT("in_progress");
	}
	if (RawResult.Equals(TEXT("cancelled"), ESearchCase::IgnoreCase) ||
		RawResult.Equals(TEXT("not_started"), ESearchCase::IgnoreCase) ||
		RawResult.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		return RawResult.ToLower();
	}
	return TEXT("unknown");
}

// Resolve a capture output directory to a writable, ABSOLUTE on-disk path and create it.
//
// THE FOOTGUN THIS FIXES: a virtual long-package path (e.g. "/Game/Tests/AnimCaps" or
// "/MyPlugin/Caps") is NOT a filesystem path. SaveImageAutoFormat to it silently no-ops —
// the capture reports success but no PNG lands on disk. This helper detects such paths and
// converts them to the real content directory (e.g. <project>/Content/Tests/AnimCaps) via
// FPackageName::TryConvertLongPackageNameToFilename (verified UE 5.7 signature:
//   static bool FPackageName::TryConvertLongPackageNameToFilename(
//       const FString& InLongPackageName, FString& OutFilename, const FString& InExtension = TEXT(""));
// the 2-arg form yields a path with no extension; appending "/" first maps it to a folder).
//
// Disk-relative paths are made project-relative-absolute; absolute paths pass through.
// Returns false (with OutError set) when a virtual path cannot be converted, or when the
// resolved directory cannot be created — callers MUST surface this as an error, never success.
static bool ResolveCaptureOutputDir(const FString& InOutputDir, FString& OutResolvedAbsDir, FString& OutError)
{
	FString Resolved = InOutputDir;

	// Virtual long-package path? (starts with "/Something" — /Game, /Engine, /<Plugin>).
	// A Windows on-disk absolute path starts with a drive letter ("D:\..."), not "/", so
	// the leading-slash + IsValidLongPackageName pair cleanly distinguishes virtual paths.
	// IsValidLongPackageName rejects trailing slashes, so test the cleaned form.
	const bool bLooksVirtual = Resolved.StartsWith(TEXT("/")) &&
		FPackageName::IsValidLongPackageName(
			Resolved.EndsWith(TEXT("/")) ? Resolved.LeftChop(1) : Resolved,
			/*bIncludeReadOnlyRoots=*/true);

	if (bLooksVirtual)
	{
		// Append a trailing "/" so the package path maps to a DIRECTORY, mirroring the
		// engine idiom in AssetManager.cpp (TryConvert(Path / TEXT(""), OnDiskPath)).
		const FString PackageDir = Resolved.EndsWith(TEXT("/")) ? Resolved : (Resolved + TEXT("/"));
		FString DiskPath;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageDir, DiskPath))
		{
			OutError = FString::Printf(
				TEXT("Could not convert virtual path '%s' to an on-disk directory ")
				TEXT("(no content root mounted for that prefix)."), *InOutputDir);
			return false;
		}
		Resolved = DiskPath;
	}
	else if (FPaths::IsRelative(Resolved))
	{
		// Disk-relative (e.g. "Saved/Screenshots/...") — anchor to the project dir.
		Resolved = FPaths::ProjectDir() / Resolved;
	}

	Resolved = FPaths::ConvertRelativePathToFull(Resolved);

	// Create the directory; a failure here must NOT be reported as a successful capture.
	if (!IFileManager::Get().MakeDirectory(*Resolved, /*Tree=*/true))
	{
		// MakeDirectory returns false if it already exists in some builds — only error when
		// the directory genuinely does not exist afterward.
		if (!IFileManager::Get().DirectoryExists(*Resolved))
		{
			OutError = FString::Printf(
				TEXT("Could not create capture output directory '%s' (resolved from '%s')."),
				*Resolved, *InOutputDir);
			return false;
		}
	}

	OutResolvedAbsDir = Resolved;
	return true;
}

// --- Registration ---

void FMonolithEditorActions::RegisterActions(FMonolithLogCapture* LogCapture)
{
	CachedLogCapture = LogCapture;
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Hand the shared log capture to the async PIE-smoke session manager so poll/stop
	// can compute post-marker pattern counts from the same ring buffer.
	FPieSmokeSessionManager::Get().SetLogCapture(LogCapture);

	Registry.RegisterAction(TEXT("editor"), TEXT("trigger_build"),
		TEXT("Trigger a Live Coding compile"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("live_compile"),
		TEXT("Trigger a Live Coding compile (alias for trigger_build)"),
		FMonolithActionHandler::CreateStatic(&HandleTriggerBuild),
		FParamSchemaBuilder()
			.Optional(TEXT("wait"), TEXT("bool"), TEXT("Block until compile finishes"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_errors"),
		TEXT("Get build errors and warnings, scoped to a time window and bucketed into compile_errors (LogLiveCoding/LogCompile/LogLinker) vs other_errors. By default excludes LogPython + LogMonolith capture noise from the headline error_count (still visible under other_errors). Window precedence: since_marker > since_iso > since_seconds/since > last compile."),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildErrors),
		FParamSchemaBuilder()
			.Optional(TEXT("since_marker"), TEXT("string"), TEXT("Report only errors AFTER the latest log line containing this token (e.g. a marker you UE_LOG'd right before compiling). Highest-precedence window. Reports marker_found."))
			.Optional(TEXT("since_iso"), TEXT("string"), TEXT("Absolute ISO-8601 cutoff (e.g. 2026-06-06T12:00:00Z) — report only errors after this time."))
			.Optional(TEXT("since_seconds"), TEXT("number"), TEXT("Relative window: only errors from the last N seconds."))
			.Optional(TEXT("since"), TEXT("number"), TEXT("Legacy alias for since_seconds (last N seconds)."))
			.Optional(TEXT("clear_baseline"), TEXT("bool"), TEXT("Stamp a fresh baseline (now) and return nothing before it — the 'I just kicked off a build, ignore prior noise' reset. Returns immediately."), TEXT("false"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Narrow the QUERY to a specific log category (separate from compile/other bucketing)."))
			.Optional(TEXT("compile_only"), TEXT("bool"), TEXT("Narrow the QUERY to compile categories only (LogLiveCoding/LogCompile/LogLinker)."), TEXT("false"))
			.Optional(TEXT("exclude_categories"), TEXT("array"), TEXT("Categories bucketed under other_errors and kept OUT of the headline error_count (still returned — never hidden). Default [LogPython, LogMonolith]."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_status"),
		TEXT("Check compile status: compiling, last_result, last_compile_time, errors_since_compile, patch_applied"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildStatus),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_build_summary"),
		TEXT("Get summary of last build (errors, warnings, time)"),
		FMonolithActionHandler::CreateStatic(&HandleGetBuildSummary),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_build_output"),
		TEXT("Search build log output by pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchBuildOutput),
		FParamSchemaBuilder()
			.Required(TEXT("pattern"), TEXT("string"), TEXT("Search pattern"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return (default: 100, max: 1000)"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_recent_logs"),
		TEXT("Get recent editor log entries"),
		FMonolithActionHandler::CreateStatic(&HandleGetRecentLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of entries to return"), TEXT("100"))
			.AddAlias(TEXT("count"), TEXT("max"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("search_logs"),
		TEXT("Search log entries by category, verbosity, and text pattern"),
		FMonolithActionHandler::CreateStatic(&HandleSearchLogs),
		FParamSchemaBuilder()
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Text pattern to search for"))
			.Optional(TEXT("category"), TEXT("string"), TEXT("Log category filter"))
			.Optional(TEXT("verbosity"), TEXT("string"), TEXT("Max verbosity level (error, warning, log, verbose)"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max results to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("tail_log"),
		TEXT("Get last N log lines"),
		FMonolithActionHandler::CreateStatic(&HandleTailLog),
		FParamSchemaBuilder()
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of lines to return"), TEXT("50"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_categories"),
		TEXT("List active log categories"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogCategories),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_log_stats"),
		TEXT("Get log statistics by verbosity level"),
		FMonolithActionHandler::CreateStatic(&HandleGetLogStats),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_compile_output"),
		TEXT("Get structured compile report: result, time, log lines from compile categories, error/warning counts, patch status"),
		FMonolithActionHandler::CreateStatic(&HandleGetCompileOutput),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_live_coding_diagnostics"),
		TEXT("Return normalized Live Coding availability, compile result, diagnostic freshness, and bounded compile log excerpts."),
		FMonolithActionHandler::CreateStatic(&HandleGetLiveCodingDiagnostics),
		FParamSchemaBuilder()
			.Optional(TEXT("max_log_entries"), TEXT("integer"), TEXT("Maximum fresh diagnostic log entries to return (default: 50, max: 200)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_crash_context"),
		TEXT("Get last crash/ensure context information"),
		FMonolithActionHandler::CreateStatic(&HandleGetCrashContext),
		MakeShared<FJsonObject>());

	// Security hardening: do not expose arbitrary console command execution over MCP.

	Registry.RegisterAction(TEXT("editor"), TEXT("start_pie"),
		TEXT("Start a Play-In-Editor session (equivalent to pressing Cmd+P in the editor)."),
		FMonolithActionHandler::CreateStatic(&HandleStartPIE),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("stop_pie"),
		TEXT("Stop the active Play-In-Editor session."),
		FMonolithActionHandler::CreateStatic(&HandleStopPIE),
		MakeShared<FJsonObject>());

	// --- Package state (F1: PIE/profiling harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("list_dirty_packages"),
		TEXT("Report loaded packages with unsaved changes (UPackage::IsDirty), optionally scoped to one or more /Game path prefixes. Returns per-package {package, is_map, disk_path, transient}. Use to audit what a save_packages call would touch."),
		FMonolithActionHandler::CreateStatic(&HandleListDirtyPackages),
		FParamSchemaBuilder()
			.Optional(TEXT("scope_paths"), TEXT("array"), TEXT("Array of /Game path prefixes to filter by (e.g. [\"/Game/Tests/Monolith\"]). Omit for all dirty packages. Transient/in-memory packages are excluded unless include_transient=true."))
			.Optional(TEXT("include_transient"), TEXT("bool"), TEXT("Include /Engine/Transient and other non-disk packages. Default false."), TEXT("false"))
			.Optional(TEXT("include_maps"), TEXT("bool"), TEXT("Include dirty map packages (UPackage::ContainsMap). Default true."), TEXT("true"))
			.Optional(TEXT("include_content"), TEXT("bool"), TEXT("Include dirty non-map (content) packages. Default true."), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("save_packages"),
		TEXT("Save the requested packages to disk (UPackage::SavePackage + FSavePackageArgs). When fail_on_unrequested_dirty=true, errors before saving anything if any dirty package exists outside the requested set (within scope_paths if given). Returns per-package save status."),
		FMonolithActionHandler::CreateStatic(&HandleSavePackages),
		FParamSchemaBuilder()
			.Required(TEXT("packages"), TEXT("array"), TEXT("Array of long package names (e.g. [\"/Game/Tests/Monolith/DA_Foo\"]) to save."))
			.Optional(TEXT("fail_on_unrequested_dirty"), TEXT("bool"), TEXT("If true, abort (saving nothing) when a dirty package outside the request set is found. Default false."), TEXT("false"))
			.Optional(TEXT("scope_paths"), TEXT("array"), TEXT("Path prefixes that bound the unrequested-dirty pre-scan (only used with fail_on_unrequested_dirty). Omit to scan all dirty packages."))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("If true, report which packages WOULD be saved (per-package would_save status) without writing anything to disk. Default false."), TEXT("false"))
			.Build());

	// --- PIE smoke + capture (F2/F3: PIE/profiling harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("run_pie_smoke"),
		TEXT("Start an ASYNC PIE smoke session on a map and RETURN IMMEDIATELY. Loads the map, starts PIE (synchronously), emits a UE_LOG marker, and registers a session that the editor's REAL frame loop advances over real frames (sampling the target pawn's AnimInstance vars). Returns {session_id, status:'running', started:true}. Poll progress / the final report with poll_pie_smoke; force-end with stop_pie_smoke. Does NOT block the editor frame (the old synchronous pump re-entered UWorld::Tick and crashed)."),
		FMonolithActionHandler::CreateStatic(&HandleRunPieSmoke),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("map"), TEXT("Level asset path to load before PIE (e.g. /Game/Tests/Monolith/Maps/M_Harness). Omit to use the current editor level."))
			.Optional(TEXT("marker"), TEXT("string"), TEXT("Marker token emitted to the log; post-marker pattern matching counts only lines after it. Default MONOLITH_SMOKE."), TEXT("MONOLITH_SMOKE"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Seconds the editor loop advances PIE before the session auto-completes (clamped 0-120). Default 5."), TEXT("5"))
			.Optional(TEXT("sample_vars"), TEXT("array"), TEXT("AnimInstance variable names sampled each frame. Default [GroundSpeed, bShouldMove, DesiredYawDelta]."))
			.Optional(TEXT("pawn_class"), TEXT("string"), TEXT("Substring of the target pawn's class name to sample (resolves a matching pawn). Omit to use the first player controller's pawn."))
			.Optional(TEXT("console_script"), TEXT("array"), TEXT("Console command strings run on the PIE world at start (e.g. [\"WalkLoop\"])."))
			.Optional(TEXT("log_patterns"), TEXT("array|object"), TEXT("Post-marker patterns. FLAT ARRAY (legacy) = must_absent substrings. OBJECT (grouped) = {must_absent:[...], must_present:[...], observe_only:[...], warn:[...]}: must_absent any-match fails ok; must_present each must match >=1 for ok; observe_only counted only; warn surfaces a warnings list. Default-set substrings are always added to must_absent."))
			.Optional(TEXT("ignore_after_pattern"), TEXT("string"), TEXT("Substring marking the teardown boundary; post-marker entries split into active-runtime (before, ok-bearing) + teardown buckets. Default \"BeginTearingDown\". Empty disables the split."), TEXT("BeginTearingDown"))
			.Optional(TEXT("teardown_allowed"), TEXT("bool"), TEXT("If true (default), must_absent hits in the teardown bucket NEVER affect ok (e.g. RecastNavMesh teardown warnings). Set false to also fail ok on teardown-bucket hits."), TEXT("true"))
			.Optional(TEXT("probe_scripts"), TEXT("array"), TEXT("Delayed in-session probes: [{at_seconds:number, console?:[string]}]. Each fires ONCE against the LIVE PIE world from the frame observer when session elapsed reaches at_seconds (avoids the start-time teardown race). Results reported under 'probes'."))
			.Optional(TEXT("stages"), TEXT("object"), TEXT("Staged startup hooks fired at lifecycle moments with console payloads only: {pre_pie:{console?:[...]} (runs synchronously BEFORE PIE start, against the editor), on_begin_play:{...} (first observer tick after HasBegunPlay), after_n_ticks:{n:int, console?:[...]} (after N observer ticks), before_capture:{...} (clip variant: before first frame grab)}. Results reported under 'stages'."))
			.Optional(TEXT("on_compile_errors"), TEXT("string"), TEXT("Policy when loaded Blueprints have unresolved compile errors: \"refuse\" (default, safe) returns an error + the offending {name,path} list and does NOT start PIE; \"suppress\" starts PIE anyway and silences the engine's blocking compile-error modal (which would otherwise freeze the editor + MCP server)."), TEXT("refuse"))
			.Optional(TEXT("actor_setup"), TEXT("array"), TEXT("Declarative spawn/apply/move block executed ONCE against the live PIE world on the first ready tick (after BeginPlay). [{class:\"/Game/.../BP_Foo\" (BP or native class path; _C suffix optional), count:<int, default 1>, locations:[[x,y,z],...] (per-actor spawn; index falls back to origin), apply_data_asset:\"/Game/.../DA_Bar\" (optional — copies the DataAsset's reflected fields onto matching-named actor properties of a COMPATIBLE type; the field->prop map is NOT 1:1), move_to:[x,y,z] (optional — AAIController::MoveToLocation via the spawned pawn's controller)}]. Reported under 'actor_setup' as {class, class_resolved, requested_count, spawned_count, data_asset_loaded?, actors:[{spawned, name, runtime_class, applied:[...], unmatched:[...], move_to:{issued,result}}]} so partial-vs-full apply is programmatically distinguishable."))
			.Optional(TEXT("csv_profile"), TEXT("bool"), TEXT("If true, start the engine CSV profiler on session start (first post-BeginPlay tick) and stop it on completion, bracketing the capture to EXACTLY the PIE window. The .csv is written to <project>/Saved/Profiling and its path is reported under 'profiling.csv_path'. Stopped on EVERY end path (success/failure/abort). Reports profiling.csv.available=false when the build config disables the CSV profiler (CSV_PROFILER off). Default false."), TEXT("false"))
			.Optional(TEXT("trace_channels"), TEXT("array"), TEXT("Channel names (e.g. [\"cpu\",\"frame\",\"gpu\"]) for an Unreal Insights trace started on session start and stopped on completion, bracketing the capture to the PIE window. The .utrace is written to <project>/Saved/Profiling and its path is reported under 'profiling.trace_path'. Stopped on EVERY end path. Omit/empty to disable tracing. If a trace is already connected, this session does not start (or later stop) it."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_errored_blueprints"),
		TEXT("Read-only scan of all loaded Blueprints for unresolved compile errors (status==BS_Error && bDisplayCompilePIEWarning) — the exact condition the engine tests before raising its blocking PIE compile-error modal. Returns {count, blueprints:[{name, path}]}. Run this before run_pie_smoke to know whether PIE will be refused / blocked."),
		FMonolithActionHandler::CreateStatic(&HandleListErroredBlueprints),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("poll_pie_smoke"),
		TEXT("Poll an async PIE-smoke session by id. Returns {status (running/complete/stopped/error), elapsed_seconds, sample_count, pie_active, summarized per-var min/max/last, post_marker_counts:{pattern:count}}. When status==complete it includes the full report (all samples + captured frame paths for the clip variant). Does not advance PIE — the editor frame loop does that."),
		FMonolithActionHandler::CreateStatic(&HandlePollPieSmoke),
		FParamSchemaBuilder()
			.Required(TEXT("session_id"), TEXT("string"), TEXT("Session id returned by run_pie_smoke / capture_pie_movement_clip."))
			.Optional(TEXT("include_samples"), TEXT("bool"), TEXT("If true, include the full per-frame sample array even before completion. Default false (summary only)."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stop_pie_smoke"),
		TEXT("Force-stop a PIE-smoke session (RequestEndPlayMap + mark stopped) and return its final report. With no session_id, stops ALL running sessions. Also serves as cleanup — the shared frame observer self-unregisters once no sessions remain running."),
		FMonolithActionHandler::CreateStatic(&HandleStopPieSmoke),
		FParamSchemaBuilder()
			.Optional(TEXT("session_id"), TEXT("string"), TEXT("Session to stop. Omit to stop every running session."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_pie_movement_clip"),
		TEXT("Start an async PIE-smoke session (same model as run_pie_smoke) that ALSO captures a PIE viewport frame every capture_interval seconds into output_path, plus per-frame AnimInstance sampling. Returns {session_id, status:'running', started:true} immediately; poll_pie_smoke returns the sampled values + captured frame paths. If viewport capture is unavailable during PIE the session continues and poll reports capture_deferred."),
		FMonolithActionHandler::CreateStatic(&HandleCapturePieMovementClip),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("map"), TEXT("Level asset path to load before PIE. Omit to use the current editor level."))
			.Optional(TEXT("marker"), TEXT("string"), TEXT("Log marker token. Default MONOLITH_CLIP."), TEXT("MONOLITH_CLIP"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Seconds the editor loop advances PIE before the session auto-completes (clamped 0-120). Default 5."), TEXT("5"))
			.Optional(TEXT("capture_interval"), TEXT("number"), TEXT("Seconds between captured frames (clamped 0.05-5). Default 0.25."), TEXT("0.25"))
			.Optional(TEXT("sample_vars"), TEXT("array"), TEXT("AnimInstance variable names sampled each frame. Default [GroundSpeed, bShouldMove, DesiredYawDelta]."))
			.Optional(TEXT("pawn_class"), TEXT("string"), TEXT("Substring of the target pawn's class name to sample. Omit to use the first player controller's pawn."))
			.Optional(TEXT("console_script"), TEXT("array"), TEXT("Console commands run on the PIE world at start (drive the movement)."))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Directory for frame PNGs. Disk-relative, absolute, OR a virtual /Game/... path (resolved to <project>/Content/...). Default Saved/Screenshots/Monolith/PieClip/<timestamp>/. The resolved absolute dir is echoed as resolved_output_dir; a virtual path that can't be written is a hard error (no silent no-op)."))
			.Optional(TEXT("log_patterns"), TEXT("array|object"), TEXT("Post-marker patterns. FLAT ARRAY (legacy) = must_absent. OBJECT = {must_absent, must_present, observe_only, warn} (see run_pie_smoke)."))
			.Optional(TEXT("ignore_after_pattern"), TEXT("string"), TEXT("Teardown-boundary substring; splits post-marker entries into active-runtime + teardown buckets. Default \"BeginTearingDown\"."), TEXT("BeginTearingDown"))
			.Optional(TEXT("teardown_allowed"), TEXT("bool"), TEXT("If true (default), teardown-bucket must_absent hits never affect ok."), TEXT("true"))
			.Optional(TEXT("probe_scripts"), TEXT("array"), TEXT("Delayed in-session probes [{at_seconds, console?:[...]}] fired once against the live PIE world (see run_pie_smoke)."))
			.Optional(TEXT("stages"), TEXT("object"), TEXT("Staged startup hooks {pre_pie, on_begin_play, after_n_ticks:{n,...}, before_capture} with console payloads only (see run_pie_smoke). before_capture fires immediately before the first frame grab. Reported under 'stages'."))
			.Optional(TEXT("view_target_actor"), TEXT("string"), TEXT("Outliner-label-, object-name- or class-substring of a PIE actor to APlayerController::SetViewTarget on at session begin, so captured frames frame the intended subject. Label (GetActorLabel) is matched first, then object name, then class name. Reported (with the active view target + per-frame validity) under 'view_target' / 'capture_validity'."))
			.Optional(TEXT("discard_first_frames"), TEXT("number"), TEXT("Warm-up policy: the first N captured frames are still saved to disk (the clip stays complete) but are EXCLUDED from valid/invalid frame accounting. Prevents an un-warmed/uniform first frame from false-failing captured_ok. Clamped 0-16. Default 1; 0 disables the warm-up. A render-flush is also issued before the first ReadPixels."), TEXT("1"))
			.Optional(TEXT("expected_anim_class"), TEXT("string"), TEXT("If set, assert the live mesh AnimClass path CONTAINS this substring each sampled tick; mismatch is reported under runtime_identity.expected_mismatch (never crashes)."))
			.Optional(TEXT("actor_setup"), TEXT("array"), TEXT("Declarative spawn/apply/move block executed ONCE against the live PIE world on the first ready tick (after BeginPlay). [{class, count, locations:[[x,y,z],...], apply_data_asset, move_to:[x,y,z]}] — see run_pie_smoke for full semantics. Reported under 'actor_setup' with per-actor applied/unmatched DataAsset fields + move-request result."))
			.Optional(TEXT("csv_profile"), TEXT("bool"), TEXT("If true, start the engine CSV profiler on session start (first post-BeginPlay tick) and stop it on completion, bracketing the capture to EXACTLY the PIE window. The .csv is written to <project>/Saved/Profiling and its path is reported under 'profiling.csv_path'. Stopped on EVERY end path (success/failure/abort). Reports profiling.csv.available=false when the build config disables the CSV profiler (CSV_PROFILER off). Default false."), TEXT("false"))
			.Optional(TEXT("trace_channels"), TEXT("array"), TEXT("Channel names (e.g. [\"cpu\",\"frame\",\"gpu\"]) for an Unreal Insights trace started on session start and stopped on completion, bracketing the capture to the PIE window. The .utrace is written to <project>/Saved/Profiling and its path is reported under 'profiling.trace_path'. Stopped on EVERY end path. Omit/empty to disable tracing. If a trace is already connected, this session does not start (or later stop) it."))
			.Build());

	// --- Nav harness map builder (F4: PIE/profiling harness plan 2026-06-04) ---

	Registry.RegisterAction(TEXT("editor"), TEXT("create_nav_harness_map"),
		TEXT("Build a navigation test map from a JSON spec: blank UWorld, floor, nav bounds, camera, target points, and BP/actor instances with reflective UPROPERTY defaults (scalars, FSoftObjectPath, object/soft-object refs, CLASS/SOFTCLASS refs `_C`-normalized, and arrays of those). Optional WorldSettings GameMode override + APlayerStart spawns. All spawned actors get a SetFolderPath. Rebuilds + validates nav via runtime `ai` dispatch and saves. Writes to a throwaway map path only."),
		FMonolithActionHandler::CreateStatic(&HandleCreateNavHarnessMap),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("path"), TEXT("Asset path for the new UWorld (e.g. /Game/Tests/Monolith/Maps/M_NavHarness)."))
			.Optional(TEXT("floor"), TEXT("object"), TEXT("{location:[x,y,z], scale:[x,y,z], mesh:\"/Engine/BasicShapes/Plane\"}. Omitted = a default 50x50m plane at origin."))
			.Optional(TEXT("nav_bounds"), TEXT("object"), TEXT("{location:[x,y,z], extent:[x,y,z]} for the NavMeshBoundsVolume. Omitted = bounds sized to the floor."))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r]} for a spawned ACameraActor."))
			.Optional(TEXT("target_points"), TEXT("array"), TEXT("[{name:\"start\", location:[x,y,z]}, ...] spawned as ATargetPoint actors; also used as nav validation points."))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("[{class:\"/Game/.../BP_Foo.BP_Foo_C\", location:[x,y,z], rotation:[p,y,r], folder:\"Harness\", properties:{Prop:value, SoftRefProp:\"/Game/...\"}}, ...]"))
			.Optional(TEXT("validate_pairs"), TEXT("array"), TEXT("[{from:\"start\", to:\"goal\"}, ...] target-point name pairs that must have a nav path."))
			.Optional(TEXT("game_mode_override"), TEXT("string"), TEXT("Class path for the map's GameMode Override (AWorldSettings::DefaultGameMode). Blueprint paths are `_C` normalized automatically; native paths (/Script/...) work directly. Must resolve to an AGameModeBase subclass."))
			.Optional(TEXT("player_starts"), TEXT("array"), TEXT("[{location:[x,y,z], rotation:[p,y,r], name:\"Start_0\"}, ...] spawned as APlayerStart actors under Harness/PlayerStarts."))
			.Optional(TEXT("nav_timeout"), TEXT("number"), TEXT("Seconds to wait for nav generation (passed to ai.rebuild_navigation). Default 30."), TEXT("30"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("author_map_settings"),
		TEXT("Author map-level settings on the currently-open (or a specified) editor map: set the WorldSettings GameMode Override (AWorldSettings::DefaultGameMode), spawn APlayerStart actors at given transforms, and optionally spawn native/Blueprint actor instances (with reflective UPROPERTY defaults — float/int/bool/string/name, FSoftObjectPath, object & soft-object refs, CLASS / SOFTCLASS refs `_C`-normalized, and arrays of those leaf types). Generic complement to create_nav_harness_map — not tied to the nav-harness path. Only dirties packages on actual change; optionally saves."),
		FMonolithActionHandler::CreateStatic(&HandleAuthorMapSettings),
		FParamSchemaBuilder()
			.OptionalAssetPath(TEXT("path"), TEXT("UWorld to author. Omitted = the currently-open editor world. If provided, the map is loaded as the active editor world first."))
			.Optional(TEXT("game_mode_override"), TEXT("string"), TEXT("Class path for the GameMode Override (AWorldSettings::DefaultGameMode). Blueprint paths are `_C` normalized; native paths (/Script/...) work directly. Must resolve to an AGameModeBase subclass."))
			.Optional(TEXT("player_starts"), TEXT("array"), TEXT("[{location:[x,y,z], rotation:[p,y,r], name:\"Start_0\"}, ...] spawned as APlayerStart actors."))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("[{class:\"/Game/.../BP_Foo.BP_Foo_C\" or /Script/Engine.PointLight, location:[x,y,z], rotation:[p,y,r], folder:\"...\", properties:{...}}, ...] — native or Blueprint actor instances with reflective property defaults."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the authored map package after applying. Default false."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("set_world_settings_property"),
		TEXT("Set one reflected property on the active or specified map's AWorldSettings object. Supports scalar values, object/soft-object refs, class/soft-class refs with `_C` normalization, and arrays of those leaf values. Intended for settings beyond GameMode Override, such as ALyraWorldSettings::DefaultGameplayExperience. Compares before dirtying and supports dry_run."),
		FMonolithActionHandler::CreateStatic(&HandleSetWorldSettingsProperty),
		FParamSchemaBuilder()
			.EnableValidation()
			.OptionalAssetPath(TEXT("path"), TEXT("UWorld to author. Omitted = the currently-open editor world. If provided, the map is loaded as the active editor world first."))
			.Required(TEXT("property_name"), TEXT("string"), TEXT("Reflected AWorldSettings property name to set, e.g. DefaultGameplayExperience."))
			.Required(TEXT("value"), TEXT("any"), TEXT("JSON value to assign. Strings can be asset/object/class paths; arrays set array properties."))
			.Optional(TEXT("save"), TEXT("bool"), TEXT("Save the map package after applying a change. Default false."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("bool"), TEXT("Preview the change without mutating the map. Default false."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("validate_assets"),
		TEXT("Run UEditorValidatorSubsystem::ValidateAssetsWithSettings for explicit package/object paths or a recursive package-path prefix. Returns aggregate counts plus optional per-asset details. This is the generic engine DataValidation wrapper; project-specific ValidateProjectSettings helpers are reported as unsupported unless implemented by a project-facing adapter."),
		FMonolithActionHandler::CreateStatic(&HandleValidateAssets),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("asset_paths"), TEXT("array"), TEXT("Asset package or object paths to validate, e.g. [\"/Game/Foo/DA_Bar\"] or [\"/Game/Foo/DA_Bar.DA_Bar\"]."))
			.Optional(TEXT("packages"), TEXT("array"), TEXT("Alias for asset_paths when callers already have package names."))
			.OptionalAssetPath(TEXT("path"), TEXT("Recursive package path prefix to validate, e.g. /Game/UI."))
			.Optional(TEXT("validation_usecase"), TEXT("string"), TEXT("none | manual | commandlet | save | pre_submit | script. Default commandlet."), TEXT("commandlet"))
			.Optional(TEXT("collect_per_asset_details"), TEXT("bool"), TEXT("Collect result/errors/warnings per asset. Default true."), TEXT("true"))
			.Optional(TEXT("load_assets"), TEXT("bool"), TEXT("Load unloaded assets for validation. Default true."), TEXT("true"))
			.Optional(TEXT("load_external_objects"), TEXT("bool"), TEXT("Load external objects associated with assets such as maps. Default true."), TEXT("true"))
			.Optional(TEXT("capture_logs"), TEXT("bool"), TEXT("Capture validation warnings/errors from load and validation operations. Default true."), TEXT("true"))
			.Optional(TEXT("warnings_as_errors"), TEXT("bool"), TEXT("Treat captured validation warnings as errors. Default false."), TEXT("false"))
			.Optional(TEXT("skip_excluded_directories"), TEXT("bool"), TEXT("Honor DataValidation excluded directories. Default true."), TEXT("true"))
			.Optional(TEXT("max_assets_to_validate"), TEXT("integer"), TEXT("Maximum assets to validate. Default unlimited."))
			.Optional(TEXT("silent"), TEXT("bool"), TEXT("Suppress progress UI/message log visibility. Default true."), TEXT("true"))
			.Optional(TEXT("validate_project_settings"), TEXT("bool"), TEXT("Request project-settings validation. Unreal has no generic UEditorValidatorSubsystem API for this; response reports unsupported unless a project adapter exists. Default false."), TEXT("false"))
			.Build());

	FMonolithActionExecutionPolicy ExplicitReadOnly = FMonolithActionExecutionPolicy::DefaultReadOnly();
	ExplicitReadOnly.bDefaulted = false;

	Registry.RegisterAction(TEXT("editor"), TEXT("plan_content_validation_changeset"),
		TEXT("Plan editor DataValidation targets from a Perforce changelist, opened files, and/or explicit depot/local/package paths. Maps package files to Unreal long package names and returns the exact validation params without saving, checkout, or mutation."),
		FMonolithActionHandler::CreateStatic(&HandlePlanContentValidationChangeset),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("changelist"), TEXT("string"), TEXT("Perforce changelist number or 'default'. Omit for all opened files when include_opened=true."))
			.Optional(TEXT("paths"), TEXT("array"), TEXT("Depot, client, local filesystem, /Game package, or object paths to include."))
			.Optional(TEXT("packages"), TEXT("array"), TEXT("Alias for paths when callers already have package names."))
			.Optional(TEXT("include_opened"), TEXT("bool"), TEXT("Include p4 opened rows. Default true when paths/packages is omitted or changelist is supplied; otherwise false."))
			.Optional(TEXT("resolve_packages"), TEXT("bool"), TEXT("Resolve p4 depot paths to local/package paths. Default true."), TEXT("true"))
			.Optional(TEXT("include_non_packages"), TEXT("bool"), TEXT("Return non-package rows alongside package targets. Default true."), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum opened/path rows to inspect. Default 500, max 5000."), TEXT("500"))
			.Optional(TEXT("validation_usecase"), TEXT("string"), TEXT("Suggested validate_assets usecase for validation_params. Default pre_submit."), TEXT("pre_submit"))
			.Build(),
		FString(),
		ExplicitReadOnly);

	Registry.RegisterAction(TEXT("editor"), TEXT("validate_changeset_assets"),
		TEXT("Resolve a Perforce changelist/opened-file set or explicit paths with plan_content_validation_changeset, then run validate_assets on the resolved package targets. Code-only changesets return a skipped validation payload rather than an error."),
		FMonolithActionHandler::CreateStatic(&HandleValidateChangesetAssets),
		FParamSchemaBuilder()
			.EnableValidation()
			.Optional(TEXT("changelist"), TEXT("string"), TEXT("Perforce changelist number or 'default'. Omit for all opened files when include_opened=true."))
			.Optional(TEXT("paths"), TEXT("array"), TEXT("Depot, client, local filesystem, /Game package, or object paths to include."))
			.Optional(TEXT("packages"), TEXT("array"), TEXT("Alias for paths when callers already have package names."))
			.Optional(TEXT("include_opened"), TEXT("bool"), TEXT("Include p4 opened rows. Default true when paths/packages is omitted or changelist is supplied; otherwise false."))
			.Optional(TEXT("resolve_packages"), TEXT("bool"), TEXT("Resolve p4 depot paths to local/package paths. Default true."), TEXT("true"))
			.Optional(TEXT("include_non_packages"), TEXT("bool"), TEXT("Return non-package rows alongside package targets. Default true."), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum opened/path rows to inspect. Default 500, max 5000."), TEXT("500"))
			.Optional(TEXT("validation_usecase"), TEXT("string"), TEXT("none | manual | commandlet | save | pre_submit | script. Default pre_submit."), TEXT("pre_submit"))
			.Optional(TEXT("collect_per_asset_details"), TEXT("bool"), TEXT("Collect result/errors/warnings per asset. Default true."), TEXT("true"))
			.Optional(TEXT("load_assets"), TEXT("bool"), TEXT("Load unloaded assets for validation. Default true."), TEXT("true"))
			.Optional(TEXT("load_external_objects"), TEXT("bool"), TEXT("Load external objects associated with assets such as maps. Default true."), TEXT("true"))
			.Optional(TEXT("capture_logs"), TEXT("bool"), TEXT("Capture validation warnings/errors from load and validation operations. Default true."), TEXT("true"))
			.Optional(TEXT("warnings_as_errors"), TEXT("bool"), TEXT("Treat captured validation warnings as errors. Default false."), TEXT("false"))
			.Optional(TEXT("skip_excluded_directories"), TEXT("bool"), TEXT("Honor DataValidation excluded directories. Default true."), TEXT("true"))
			.Optional(TEXT("max_assets_to_validate"), TEXT("integer"), TEXT("Maximum assets to validate. Default unlimited."))
			.Optional(TEXT("silent"), TEXT("bool"), TEXT("Suppress progress UI/message log visibility. Default true."), TEXT("true"))
			.Build(),
		FString(),
		ExplicitReadOnly);

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("plan_content_validation_changeset"),
		{ TEXT("data validation"), TEXT("p4 opened"), TEXT("changelist"), TEXT("package path"), TEXT("pre submit") },
		{ TEXT("plan_validation_changeset"), TEXT("validation_plan"), TEXT("opened_assets_to_validate") },
		{ TEXT("plan validation targets for changelist 1006"), TEXT("map opened p4 files to packages for validation") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("validate_changeset_assets"),
		{ TEXT("data validation"), TEXT("p4 opened"), TEXT("changelist"), TEXT("validate packages"), TEXT("pre submit") },
		{ TEXT("validate_changelist"), TEXT("validate_opened_assets"), TEXT("pre_submit_validation") },
		{ TEXT("validate assets opened in changelist 1006"), TEXT("run data validation for these depot paths") });

	// --- Capture actions ---

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_scene_preview"),
		TEXT("Capture a screenshot of an asset (Niagara, material, static_mesh, skeletal_mesh, widget) rendered in a preview scene"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureScenePreview),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara | material | static_mesh | skeletal_mesh | widget"))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Mesh for materials: plane, sphere, cube"), TEXT("plane"))
			.Optional(TEXT("seek_time"), TEXT("number"), TEXT("Advance Niagara sim or skeletal animation to this time (seconds)"), TEXT("0.0"))
			.OptionalAssetPath(TEXT("animation_path"), TEXT("skeletal_mesh only: UAnimSequence to pose with at seek_time"))
			.Optional(TEXT("scale"), TEXT("number"), TEXT("widget only: DPI multiplier (>=0.01)"), TEXT("1.0"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path (absolute or relative to project)"))
			.Build());

	// --- Inspect actions (Phase 2: 2026-05-26-monolith-editor-preview-expansion plan) ---
	// Pure reflection / source-mip-read. No render path. Bodies in
	// MonolithEditorInspectActions.cpp.

	Registry.RegisterAction(TEXT("editor"), TEXT("inspect_material_pbr"),
		TEXT("Inspect a UMaterialInterface's PBR parameter set. Returns scalar/vector/texture parameter lists plus heuristic classification of base color / normal / roughness / metallic textures and ORM / ARM / MRA channel-packing detection. Pure reflection — no render, no thumbnail. Use this when capture_scene_preview's pixel output isn't enough and you need the actual parameter values."),
		FMonolithActionHandler::CreateStatic(&HandleInspectMaterialPBR),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UMaterialInterface asset path (e.g. /Game/Materials/M_Foo or /Game/Materials/MI_Foo)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("inspect_texture_channels"),
		TEXT("Inspect a UTexture2D's R/G/B/A channel statistics (min/max/mean per channel) plus format/dimensions/sRGB/alpha. Optional per-channel split PNGs for visual debugging. Reads source mip 0 directly — bypasses runtime mip selection and compression. Useful for ORM/ARM channel-packing audits, alpha-coverage checks, and verifying source authoring against runtime appearance."),
		FMonolithActionHandler::CreateStatic(&HandleInspectTextureChannels),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UTexture2D asset path (e.g. /Game/Textures/T_Foo)"))
			.Optional(TEXT("emit_splits"), TEXT("bool"), TEXT("If true, emit 4 grayscale PNGs (R/G/B/A) under output_dir. Default false (stats only)."), TEXT("false"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Output directory for split PNGs (default: Saved/Tests/Monolith/InspectTexture/<TextureName>/)"))
			.Build());

	// --- Composite-capture actions (Phase 3: 2026-05-26-monolith-editor-preview-expansion plan) ---
	// Multi-asset / show-flag overlay captures. Bodies live in
	// MonolithEditorPreviewActions.cpp.

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_material_grid"),
		TEXT("Render N materials side-by-side on identical preview meshes in ONE scene, ONE camera, ONE PNG. Shares lighting + HDRI across all cells (the value-add over N separate captures). Auto-grid layout via ceil(sqrt(N)) columns unless overridden. Use when comparing material variations visually — e.g. tweaking roughness across MI tiers, A/B-testing a master vs an instance, or auditing a packs's hero/variant materials."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureMaterialGrid),
		FParamSchemaBuilder()
			.Required(TEXT("material_paths"), TEXT("array"), TEXT("Array of UMaterialInterface asset paths (1..16). Each becomes one grid cell."))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path. Default: Saved/Screenshots/Monolith/CaptureMaterialGrid/<timestamp>.png"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height] total grid PNG size. Default [1024, 1024]."), TEXT("[1024,1024]"))
			.Optional(TEXT("columns"), TEXT("integer"), TEXT("Grid columns. Default: ceil(sqrt(material_count))."))
			.Optional(TEXT("preview_mesh"), TEXT("string"), TEXT("Mesh per cell: plane | sphere | cube. Default sphere."), TEXT("sphere"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60} — overrides auto-framing"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_with_overlay"),
		TEXT("Render a static mesh with an FEngineShowFlags overlay (wireframe | normals | uv_density | lightmap_density | shader_complexity). Useful for visual debugging — overdraw audits, UV-density checks, lightmap-density layout review, shader-complexity heatmaps. Static-mesh only in v1 (skeletal/material flavours can be added later)."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureWithOverlay),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UStaticMesh asset path (e.g. /Engine/BasicShapes/Cube)"))
			.Required(TEXT("mode"), TEXT("string"), TEXT("Overlay: wireframe | normals | uv_density | lightmap_density | shader_complexity"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output PNG path. Default: Saved/Screenshots/Monolith/CaptureWithOverlay/<timestamp>.png"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]. Default [512, 512]."), TEXT("[512,512]"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_sequence_frames"),
		TEXT("Capture multiple frames of an animated effect at specified timestamps"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSequenceFrames),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Asset path to preview"))
			.Required(TEXT("asset_type"), TEXT("string"), TEXT("niagara"))
			.Required(TEXT("timestamps"), TEXT("array"), TEXT("Array of capture times in seconds (Max: 1000)"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}"))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]"), TEXT("[512,512]"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Output directory for frame PNGs"))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("Prefix for frame files"), TEXT("frame"))
			.Optional(TEXT("persistent"), TEXT("bool"), TEXT("Use persistent component (preserves ribbons/accumulation). Default: false (per-frame recreate)."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_anim_frames"),
		TEXT("Preview + capture a SKELETAL ANIMATION asset (UAnimSequence | UBlendSpace | UAnimBlueprint) to PNG frames in an isolated FPreviewScene (NO PIE). For each time sample the pose is evaluated (single-node SetPosition for sequences, blend-param + SetPosition for blendspaces, AnimBlueprint mode + TickComponent for ABPs) then rendered via the shared scene-capture->PNG path. Use this for AnimBP/sequence/blendspace visual diffing — the anim counterpart to capture_sequence_frames (which is Niagara-only)."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureAnimFrames),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("UAnimSequence / UBlendSpace / UAnimBlueprint asset path."))
			.OptionalAssetPath(TEXT("skeletal_mesh"), TEXT("USkeletalMesh to preview on. Omit to use the asset's skeleton preview mesh."))
			.Optional(TEXT("time_samples"), TEXT("array"), TEXT("Explicit sample times in seconds (e.g. [0, 0.5, 1.0]). Omit to use count + duration."))
			.Optional(TEXT("count"), TEXT("integer"), TEXT("Number of evenly-spaced samples over duration (used when time_samples omitted). Default 8."), TEXT("8"))
			.Optional(TEXT("duration"), TEXT("number"), TEXT("Total seconds to sample across when using count (default = the sequence length, else 1.0)."))
			.Optional(TEXT("blend_params"), TEXT("array"), TEXT("BlendSpace only: [X, Y] blend-space input applied each sample. Default [0,0]."))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("{location:[x,y,z], rotation:[p,y,r], fov:60}. Default framing looks at the mesh from the front."))
			.Optional(TEXT("resolution"), TEXT("array"), TEXT("[width, height]. Default [512,512]."), TEXT("[512,512]"))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Directory for frame PNGs. Disk-relative, absolute, OR a virtual /Game/... path (resolved to <project>/Content/...). Default Saved/Screenshots/Monolith/AnimFrames/<timestamp>_<asset>/. The resolved absolute dir is echoed as resolved_output_dir."))
			.Optional(TEXT("filename_prefix"), TEXT("string"), TEXT("Frame filename prefix. Default 'frame'."), TEXT("frame"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("import_texture"),
		TEXT("Import an external image as a UTexture2D with configurable settings"),
		FMonolithActionHandler::CreateStatic(&HandleImportTexture),
		FParamSchemaBuilder()
			.RequiredDiskPath(TEXT("source_path"), TEXT("Absolute path to source image (PNG, TGA, EXR, HDR)"))
			.Required(TEXT("destination"), TEXT("string"), TEXT("UE asset path for imported texture"))
			.Optional(TEXT("settings"), TEXT("object"), TEXT("{compression, srgb, tiling, max_size, lod_group}"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stitch_flipbook"),
		TEXT("Stitch individual frame images into a flipbook atlas texture and import as UTexture2D"),
		FMonolithActionHandler::CreateStatic(&HandleStitchFlipbook),
		FParamSchemaBuilder()
			.Required(TEXT("frame_paths"), TEXT("array"), TEXT("Ordered array of absolute file paths to frame PNGs"))
			.RequiredAssetPath(TEXT("dest_path"), TEXT("UE asset path for the output texture (e.g. /Game/AgentTraining/Textures/T_FB_001)"))
			.Required(TEXT("grid"), TEXT("array"), TEXT("[columns, rows] grid layout (e.g. [4, 4] for 16 frames)"))
			.Optional(TEXT("srgb"), TEXT("bool"), TEXT("sRGB color space (true for color, false for masks)"), TEXT("true"))
			.Optional(TEXT("no_mipmaps"), TEXT("bool"), TEXT("Disable mipmap generation to prevent atlas bleed"), TEXT("true"))
			.Optional(TEXT("delete_sources"), TEXT("bool"), TEXT("Delete source PNG files after successful stitch"), TEXT("true"))
			.Optional(TEXT("lod_group"), TEXT("string"), TEXT("Texture LOD group"), TEXT("TEXTUREGROUP_Effects"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("delete_assets"),
		TEXT("Delete UE assets by path. Optional safety: restrict to allowed path prefixes"),
		FMonolithActionHandler::CreateStatic(&HandleDeleteAssets),
		FParamSchemaBuilder()
			.Required(TEXT("asset_paths"), TEXT("array"), TEXT("Array of UE asset paths to delete"))
			.Optional(TEXT("allowed_prefixes"), TEXT("array"), TEXT("If set, only paths starting with one of these prefixes can be deleted (e.g. [\"/Game/AgentTraining/\"])"))
			.Optional(TEXT("force"), TEXT("bool"), TEXT("When true, force-delete even if referenced (nulls referencers, including EXTERNAL ones, silently). Default false: soft-delete after closing open asset editors. Use allowed_prefixes as a sandbox when force=true."), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_viewport_info"),
		TEXT("Get current editor viewport camera position, rotation, FOV, and resolution"),
		FMonolithActionHandler::CreateStatic(&HandleGetViewportInfo),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_open_viewports"),
		TEXT("List open level editor viewports and their camera/source metadata."),
		FMonolithActionHandler::CreateStatic(&HandleListOpenViewports),
		MakeShared<FJsonObject>());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_level_viewport"),
		TEXT("Capture a level editor viewport to a PNG file. Errors instead of substituting a different viewport."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureLevelViewport),
		FParamSchemaBuilder()
			.Optional(TEXT("viewport_index"), TEXT("integer"), TEXT("Index from editor.list_open_viewports"), TEXT("0"))
			.Optional(TEXT("camera"), TEXT("object"), TEXT("Optional {location:[x,y,z], rotation:[p,y,r], fov:60} applied before capture"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Output PNG path. Defaults under Saved/Screenshots/Monolith."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_asset_editor_viewport"),
		TEXT("Report asset editor viewport capture availability. Does not fall back to level viewport capture."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureAssetEditorViewport),
		FParamSchemaBuilder().Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset editor target path")).Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_widget_designer"),
		TEXT("Report widget designer capture availability. Does not fall back to level viewport capture."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureWidgetDesigner),
		FParamSchemaBuilder().Required(TEXT("widget_path"), TEXT("string"), TEXT("Widget Blueprint path")).Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_asset_thumbnail"),
		TEXT("Capture an asset thumbnail to PNG when thumbnail_fallback=true. Does not claim asset-editor viewport capture."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureAssetThumbnail),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("Asset path to capture"))
			.Optional(TEXT("thumbnail_fallback"), TEXT("bool"), TEXT("Must be true to allow thumbnail fallback capture"), TEXT("false"))
			.Optional(TEXT("thumbnail_size"), TEXT("integer"), TEXT("Square thumbnail size in pixels, clamped to 16..2048"), TEXT("256"))
			.Optional(TEXT("output_path"), TEXT("string"), TEXT("Output PNG path. Defaults under Saved/Screenshots/Monolith."))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("capture_system_gif"),
		TEXT("Capture a Niagara system as a sequence of PNG frames with adaptive 60 fps GIF encoding via ffmpeg or python"),
		FMonolithActionHandler::CreateStatic(&HandleCaptureSystemGif),
		FParamSchemaBuilder()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Niagara system asset path"))
			.Optional(TEXT("duration_seconds"), TEXT("number"), TEXT("Capture duration in seconds (default: 2.0)"))
			.Optional(TEXT("fps"), TEXT("integer"), TEXT("Target frames per second (default: 60, auto-degrades to 30/20 under resource limits, quality minimum: 20)"))
			.Optional(TEXT("resolution"), TEXT("integer"), TEXT("Output resolution width/height in pixels (default: 256)"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output directory (default: Saved/Screenshots/Monolith/GIF_<timestamp>)"))
			.Optional(TEXT("encoder"), TEXT("string"), TEXT("auto (default), frames_only, ffmpeg, or python"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("encode_frame_sequence_gif"),
		TEXT("Encode an existing ordered PNG frame sequence to a GIF with adaptive 60 fps target, 30/20 fps fallback, and ffmpeg/python encoders"),
		FMonolithActionHandler::CreateStatic(&HandleEncodeFrameSequenceGif),
		FParamSchemaBuilder()
			.Optional(TEXT("frame_paths"), TEXT("array"), TEXT("Ordered absolute or project-relative PNG frame paths"))
			.OptionalDiskPath(TEXT("frame_dir"), TEXT("Directory containing PNG frames; sorted lexically when frame_paths is omitted or combined"))
			.Optional(TEXT("pattern"), TEXT("string"), TEXT("Filename glob for frame_dir"), TEXT("*.png"))
			.OptionalDiskPath(TEXT("output_path"), TEXT("Output GIF path. Defaults beside frame_dir or under Saved/Screenshots/Monolith."))
			.Optional(TEXT("fps"), TEXT("integer"), TEXT("Target GIF frames per second (default: 60, auto-degrades to 30/20 under resource limits, quality minimum: 20)"))
			.Optional(TEXT("source_fps"), TEXT("integer"), TEXT("FPS represented by the input frames; defaults to fps and preserves duration when downsampling"))
			.Optional(TEXT("max_frames"), TEXT("integer"), TEXT("Optional caller cap for encoded frames, clamped to the 1000-frame hard cap"))
			.Optional(TEXT("scale_width"), TEXT("integer"), TEXT("Optional GIF output width. Requires ffmpeg; default preserves source frame size."))
			.Optional(TEXT("encoder"), TEXT("string"), TEXT("auto (default), frames_only, ffmpeg, or python"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_automation_tests"),
		TEXT("List registered automation tests, optionally filtered by prefix. Results are paged (default 500 per page); chain pages through next_cursor."),
		FMonolithActionHandler::CreateStatic(&HandleListAutomationTests),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Filter tests whose full path starts with this prefix (e.g. 'MazeLegends.Bow')"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Max tests per page (default: 500, max: 5000)"))
			.Optional(TEXT("offset"), TEXT("integer"), TEXT("Skip the first N tests; response echoes total/returned and next_cursor (the next offset) while more tests remain"))
			.Optional(TEXT("cursor"), TEXT("string"), TEXT("Pagination cursor from a prior next_cursor; takes precedence over offset"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("find_automation_tests"),
		TEXT("Find registered automation tests by prefix and/or case-insensitive query across full path, display name, and test name."),
		FMonolithActionHandler::CreateStatic(&HandleFindAutomationTests),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Filter tests whose full path starts with this prefix"))
			.Optional(TEXT("query"), TEXT("string"), TEXT("Case-insensitive substring match"))
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum tests to return (default: 100, max: 1000)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("run_automation_tests"),
		TEXT("Run automation tests by prefix in the running editor (no PIE, no separate process). Returns run status, progress counters, success/passed/failed counts, and per-test errors."),
		FMonolithActionHandler::CreateStatic(&HandleRunAutomationTests),
		FParamSchemaBuilder()
			.Required(TEXT("prefix"), TEXT("string"), TEXT("Run tests whose full path starts with this prefix (e.g. 'MazeLegends.Bow')"))
			.Optional(TEXT("max_tests"), TEXT("integer"), TEXT("Hard cap on number of tests to run (default: 200, max: 1000)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_automation_run_status"),
		TEXT("Return current or last Monolith automation run status plus history capacity. Synchronous runner reports can_stop=false."),
		FMonolithActionHandler::CreateStatic(&HandleGetAutomationRunStatus),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("stop_automation_tests"),
		TEXT("Return the explicit cancellation contract for Monolith automation runs. Current synchronous runner cannot be cancelled."),
		FMonolithActionHandler::CreateStatic(&HandleStopAutomationTests),
		FParamSchemaBuilder().Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("list_automation_history"),
		TEXT("List recent compact Monolith-triggered automation run summaries newest-first."),
		FMonolithActionHandler::CreateStatic(&HandleListAutomationHistory),
		FParamSchemaBuilder()
			.Optional(TEXT("max_results"), TEXT("integer"), TEXT("Maximum history rows to return (default: 20, capped to history capacity)"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("get_automation_summary"),
		TEXT("Summarize discovered automation tests and include the most recent Monolith-triggered automation run when available."),
		FMonolithActionHandler::CreateStatic(&HandleGetAutomationSummary),
		FParamSchemaBuilder()
			.Optional(TEXT("prefix"), TEXT("string"), TEXT("Filter discovered tests whose full path starts with this prefix"))
			.Build());

	Registry.RegisterAction(TEXT("editor"), TEXT("export_automation_report"),
		TEXT("Export the most recent Monolith-triggered automation run report to Project/Saved/Monolith/AutomationReports."),
		FMonolithActionHandler::CreateStatic(&HandleExportAutomationReport),
		FParamSchemaBuilder()
			.Optional(TEXT("output_dir"), TEXT("string"), TEXT("Output directory, relative to or under Project/Saved"))
			.Build());

	// --- Scripting actions (HOFF 7) ---
	// Security hardening: do not expose Python execution over MCP.

	Registry.RegisterAction(TEXT("editor"), TEXT("load_level"),
		TEXT("Close the current persistent level (without saving) and load the specified level by /Game/... asset path. Wraps ULevelEditorSubsystem::LoadLevel. If a PIE world is still resident this REFUSES while a smoke session is running, else drives PIE teardown to completion + forces a GC before loading (prevents the 'World Memory Leaks' assert)."),
		FMonolithActionHandler::CreateStatic(&HandleLoadLevel),
		FParamSchemaBuilder()
			.RequiredAssetPath(
				TEXT("path"),
				TEXT("Asset path of the level to load (e.g. /Game/Maps/L_Backyard). Must exist."),
				{ TEXT("level_path") })
			.Build());

	InitLiveCodingDelegate();

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("trigger_build"),
		{ TEXT("compile"), TEXT("recompile"), TEXT("hot reload"), TEXT("build the code"), TEXT("rebuild module"), TEXT("apply C++ changes") },
		{ TEXT("build"), TEXT("compile"), TEXT("live_compile"), TEXT("hot reload") },
		{ TEXT("recompile my C++ changes"), TEXT("trigger a live coding build"), TEXT("hot reload the editor") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("get_build_errors"),
		{ TEXT("why did the build fail"), TEXT("compile errors"), TEXT("linker errors"), TEXT("build warnings"), TEXT("what broke the compile"), TEXT("failed build output") },
		{ TEXT("build errors"), TEXT("compile errors"), TEXT("list errors"), TEXT("show build failures") },
		{ TEXT("show me why the build failed"), TEXT("get the compile errors from the last build"), TEXT("list linker errors since the last compile") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("search_logs"),
		{ TEXT("grep the log"), TEXT("find in output log"), TEXT("filter log by category"), TEXT("search for a warning"), TEXT("look for a log message") },
		{ TEXT("grep log"), TEXT("find in log"), TEXT("filter logs"), TEXT("search output log") },
		{ TEXT("search the log for LogTemp warnings"), TEXT("find every error line in the output log"), TEXT("grep the log for a specific message") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("run_console_command"),
		{ TEXT("exec command"), TEXT("console variable"), TEXT("set cvar at runtime"), TEXT("type into the console"), TEXT("stat fps"), TEXT("slomo") },
		{ TEXT("exec"), TEXT("console command"), TEXT("cmd"), TEXT("set cvar") },
		{ TEXT("run stat unit in the console"), TEXT("execute a console command in PIE"), TEXT("set a cvar with showflag") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("start_pie"),
		{ TEXT("play in editor"), TEXT("press play"), TEXT("run the game"), TEXT("launch a play session"), TEXT("enter play mode") },
		{ TEXT("play"), TEXT("play in editor"), TEXT("run game"), TEXT("begin play") },
		{ TEXT("start a play-in-editor session"), TEXT("press play to test the level"), TEXT("run the game in the editor") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("stop_pie"),
		{ TEXT("end play"), TEXT("exit play mode"), TEXT("stop the game"), TEXT("quit play session"), TEXT("leave PIE") },
		{ TEXT("stop"), TEXT("end play"), TEXT("stop game"), TEXT("exit pie") },
		{ TEXT("stop the play-in-editor session"), TEXT("end the running game"), TEXT("exit play mode") });

	FMonolithToolRegistry::Get().SetActionSearchMetadata(TEXT("editor"), TEXT("run_python"),
		{ TEXT("execute python script"), TEXT("run a py file"), TEXT("editor scripting"), TEXT("automate the editor"), TEXT("evaluate a python expression") },
		{ TEXT("python"), TEXT("exec python"), TEXT("run script"), TEXT("py") },
		{ TEXT("run a python script in the editor"), TEXT("execute a python statement"), TEXT("evaluate a python expression and return the result") });
}

// --- Build actions ---

FMonolithActionResult FMonolithEditorActions::HandleTriggerBuild(const TSharedPtr<FJsonObject>& Params)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return FMonolithActionResult::Error(TEXT("Live Coding module not available"));
	}

	if (!LiveCoding->IsEnabledForSession() && !LiveCoding->IsEnabledByDefault())
	{
		LiveCoding->EnableByDefault(true);
		LiveCoding->EnableForSession(true);
	}

	if (LiveCoding->IsCompiling())
	{
		return FMonolithActionResult::Error(TEXT("A compile is already in progress"));
	}

	bool bWait = false;
	if (Params->HasField(TEXT("wait")))
	{
		if (!Params->TryGetBoolField(TEXT("wait"), bWait))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'wait' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	LastCompileTimestamp = FPlatformTime::Seconds();
	bIsCompiling = true;
	bPatchApplied = false;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (bWait)
	{
		ELiveCodingCompileResult CompileResult;
		bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &CompileResult);

		bIsCompiling = false;
		LastCompileEndTimestamp = FPlatformTime::Seconds();
		Root->SetBoolField(TEXT("started"), bStarted);

		FString ResultStr;
		switch (CompileResult)
		{
		case ELiveCodingCompileResult::Success: ResultStr = TEXT("success"); bPatchApplied = true; break;
		case ELiveCodingCompileResult::NoChanges: ResultStr = TEXT("no_changes"); break;
		case ELiveCodingCompileResult::Failure: ResultStr = TEXT("failure"); break;
		case ELiveCodingCompileResult::Cancelled: ResultStr = TEXT("cancelled"); break;
		case ELiveCodingCompileResult::CompileStillActive: ResultStr = TEXT("compile_still_active"); break;
		case ELiveCodingCompileResult::NotStarted: ResultStr = TEXT("not_started"); break;
		default: ResultStr = TEXT("unknown"); break;
		}
		LastCompileResult = ResultStr;
		Root->SetStringField(TEXT("result"), ResultStr);
	}
	else
	{
		LiveCoding->Compile();
		LastCompileResult = TEXT("in_progress");
		Root->SetBoolField(TEXT("started"), true);
		Root->SetStringField(TEXT("result"), TEXT("in_progress"));
	}

	return FMonolithActionResult::Success(Root);
#else
	return FMonolithActionResult::Error(TEXT("Live Coding is only available on Windows"));
#endif
}

// #12 convert an ISO-8601 string back to a FPlatformTime::Seconds() value (inverse of
// TimestampToIso). Returns false if the string does not parse.
static bool IsoToPlatformSeconds(const FString& Iso, double& OutSeconds)
{
	FDateTime EventTime;
	if (!FDateTime::ParseIso8601(*Iso, EventTime))
	{
		return false;
	}
	// PlatformSeconds = now_platform - (now_utc - event_utc).
	const double DeltaSeconds = (FDateTime::UtcNow() - EventTime).GetTotalSeconds();
	OutSeconds = FPlatformTime::Seconds() - DeltaSeconds;
	return true;
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildErrors(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// #12 clear_baseline: stamp a fresh baseline (LastCompileTimestamp = now) so this and
	// future calls report nothing before it. Returns immediately with the new baseline —
	// the canonical "I just triggered a build, ignore all prior log noise" reset.
	bool bClearBaseline = false;
	Params->TryGetBoolField(TEXT("clear_baseline"), bClearBaseline);
	if (bClearBaseline)
	{
		LastCompileTimestamp = FPlatformTime::Seconds();
		Root->SetBoolField(TEXT("baseline_cleared"), true);
		Root->SetStringField(TEXT("since"), TimestampToIso(LastCompileTimestamp));
		Root->SetStringField(TEXT("since_source"), TEXT("clear_baseline"));
		Root->SetNumberField(TEXT("error_count"), 0);
		Root->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetNumberField(TEXT("compile_error_count"), 0);
		Root->SetArrayField(TEXT("compile_errors"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetNumberField(TEXT("other_error_count"), 0);
		Root->SetArrayField(TEXT("other_errors"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetNumberField(TEXT("warning_count"), 0);
		Root->SetArrayField(TEXT("warnings"), TArray<TSharedPtr<FJsonValue>>());
		return FMonolithActionResult::Success(Root);
	}

	// #12 determine the time window, in precedence order:
	//   since_marker (latest log line containing the token) >
	//   since_iso (absolute) > since_seconds / since (relative) >
	//   LastCompileTimestamp (default: since last compile).
	double SinceTimestamp = LastCompileTimestamp;
	FString SinceSource = TEXT("last_compile");

	FString SinceMarker;
	if (Params->HasField(TEXT("since_marker")) && !Params->TryGetStringField(TEXT("since_marker"), SinceMarker))
	{
		return FMonolithActionResult::Error(TEXT("since_marker must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	if (!SinceMarker.IsEmpty() && CachedLogCapture)
	{
		// Latest entry containing the marker token (any category/verbosity). SearchEntries
		// returns in ring order, so the last match is the most recent occurrence.
		TArray<FMonolithLogEntry> Marks = CachedLogCapture->SearchEntries(
			SinceMarker, FString(), ELogVerbosity::VeryVerbose, FMonolithLogCapture::MaxEntries);
		if (Marks.Num() > 0)
		{
			SinceTimestamp = Marks.Last().Timestamp;
			SinceSource = TEXT("since_marker");
			Root->SetBoolField(TEXT("marker_found"), true);
		}
		else
		{
			Root->SetBoolField(TEXT("marker_found"), false);
		}
		Root->SetStringField(TEXT("since_marker"), SinceMarker);
	}
	else
	{
		FString SinceIso;
		if (Params->HasField(TEXT("since_iso")) && !Params->TryGetStringField(TEXT("since_iso"), SinceIso))
		{
			return FMonolithActionResult::Error(TEXT("since_iso must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}

		double Parsed = 0.0;
		if (!SinceIso.IsEmpty() &&
			IsoToPlatformSeconds(SinceIso, Parsed))
		{
			SinceTimestamp = Parsed;
			SinceSource = TEXT("since_iso");
		}
		else if (Params->HasField(TEXT("since_seconds")))
		{
			double SinceSeconds = 0.0;
			if (!Params->TryGetNumberField(TEXT("since_seconds"), SinceSeconds))
			{
				return FMonolithActionResult::Error(TEXT("since_seconds must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			SinceTimestamp = FPlatformTime::Seconds() - SinceSeconds;
			SinceSource = TEXT("since_seconds");
		}
		else if (Params->HasField(TEXT("since")))
		{
			double Since = 0.0;
			if (!Params->TryGetNumberField(TEXT("since"), Since))
			{
				return FMonolithActionResult::Error(TEXT("since must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			// Legacy relative-seconds param (kept for back-compat).
			SinceTimestamp = FPlatformTime::Seconds() - Since;
			SinceSource = TEXT("since");
		}
	}

	// Optional category filter / compile_only narrowing of the QUERY (separate from the
	// compile-vs-other bucketing below, which always applies).
	TArray<FName> CategoryFilter;
	bool bCompileOnly = false;
	if (Params->HasField(TEXT("compile_only")))
	{
		if (!Params->TryGetBoolField(TEXT("compile_only"), bCompileOnly))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'compile_only' must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	if (bCompileOnly)
	{
		CategoryFilter.Add(FName(TEXT("LogLiveCoding")));
		CategoryFilter.Add(FName(TEXT("LogCompile")));
		CategoryFilter.Add(FName(TEXT("LogLinker")));
	}
	else if (Params->HasField(TEXT("category")))
	{
		FString Category;
		if (!Params->TryGetStringField(TEXT("category"), Category))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'category' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
		CategoryFilter.Add(FName(*Category));
	}

	// #12 exclude_categories: categories whose errors are bucketed under other_errors and
	// NOT counted in the headline error_count. Defaults separate Python + capture-tool
	// noise so a stale LogPython line doesn't masquerade as a current build failure. The
	// noise is bucketed (visible) — never silently dropped.
	TArray<FString> ExcludeCategories;
	const TArray<TSharedPtr<FJsonValue>>* ExclArr = nullptr;
	if (Params->HasField(TEXT("exclude_categories")))
	{
		if (!Params->TryGetArrayField(TEXT("exclude_categories"), ExclArr) || !ExclArr)
		{
			return FMonolithActionResult::Error(TEXT("exclude_categories must be an array of strings"), FMonolithJsonUtils::ErrInvalidParams);
		}
		for (const TSharedPtr<FJsonValue>& Val : *ExclArr)
		{
			FString C;
			if (Val.IsValid() && Val->TryGetString(C) && !C.IsEmpty()) { ExcludeCategories.AddUnique(C); }
		}
	}
	else
	{
		ExcludeCategories = { TEXT("LogPython"), TEXT("LogMonolith") };
	}

	// The compile categories that define the compile_errors bucket.
	static const TArray<FString> CompileCategories = {
		TEXT("LogLiveCoding"), TEXT("LogCompile"), TEXT("LogLinker") };

	TArray<TSharedPtr<FJsonValue>> ErrorsArr;        // headline errors (compile + non-excluded other)
	TArray<TSharedPtr<FJsonValue>> CompileErrorsArr; // LogLiveCoding/LogCompile/LogLinker
	TArray<TSharedPtr<FJsonValue>> OtherErrorsArr;   // everything else, incl. excluded-category noise
	TArray<TSharedPtr<FJsonValue>> WarningsArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			SinceTimestamp, CategoryFilter, ELogVerbosity::Warning, 500);

		ErrorsArr.Reserve(Entries.Num());
		WarningsArr.Reserve(Entries.Num());

		for (const FMonolithLogEntry& Entry : Entries)
		{
			const FString CatStr = Entry.Category.ToString();
			auto MakeEntryObj = [&]() -> TSharedPtr<FJsonObject>
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetStringField(TEXT("message"), Entry.Message);
				O->SetStringField(TEXT("category"), CatStr);
				O->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
				return O;
			};

			if (Entry.Verbosity <= ELogVerbosity::Error)
			{
				const bool bIsCompile = CompileCategories.Contains(CatStr);
				const bool bIsExcluded = ExcludeCategories.Contains(CatStr);
				if (bIsCompile)
				{
					CompileErrorsArr.Add(MakeShared<FJsonValueObject>(MakeEntryObj()));
					ErrorsArr.Add(MakeShared<FJsonValueObject>(MakeEntryObj()));
				}
				else
				{
					OtherErrorsArr.Add(MakeShared<FJsonValueObject>(MakeEntryObj()));
					// Excluded-category errors are bucketed under other_errors only — kept
					// out of the headline error_count so they don't look like a build break.
					if (!bIsExcluded)
					{
						ErrorsArr.Add(MakeShared<FJsonValueObject>(MakeEntryObj()));
					}
				}
			}
			else if (Entry.Verbosity == ELogVerbosity::Warning)
			{
				WarningsArr.Add(MakeShared<FJsonValueObject>(MakeEntryObj()));
			}
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorsArr.Num());
	Root->SetArrayField(TEXT("errors"), ErrorsArr);
	Root->SetNumberField(TEXT("compile_error_count"), CompileErrorsArr.Num());
	Root->SetArrayField(TEXT("compile_errors"), CompileErrorsArr);
	Root->SetNumberField(TEXT("other_error_count"), OtherErrorsArr.Num());
	Root->SetArrayField(TEXT("other_errors"), OtherErrorsArr);
	Root->SetNumberField(TEXT("warning_count"), WarningsArr.Num());
	Root->SetArrayField(TEXT("warnings"), WarningsArr);
	Root->SetStringField(TEXT("since"), TimestampToIso(SinceTimestamp));
	Root->SetStringField(TEXT("since_source"), SinceSource);

	// Echo the excluded categories so callers know what was bucketed out of error_count.
	TArray<TSharedPtr<FJsonValue>> ExclEcho;
	ExclEcho.Reserve(ExcludeCategories.Num());
	for (const FString& C : ExcludeCategories) { ExclEcho.Add(MakeShared<FJsonValueString>(C)); }
	Root->SetArrayField(TEXT("excluded_categories"), ExclEcho);

	// Item 8 (ADDITIVE): deterministic error->fix-hint pattern table. Computed over
	// the SAME error objects already in `errors[]` so the per-error `fix_hint` lands
	// on the objects callers see. Existing fields are byte-identical — BuildFixHints
	// only ADDS a `fix_hint` string to matched objects + returns a top-level array.
	TArray<TSharedPtr<FJsonValue>> FixHints = BuildFixHints(ErrorsArr);
	Root->SetArrayField(TEXT("fix_hints"), FixHints);

	return FMonolithActionResult::Success(Root);
}

// ============================================================================
// Item 8: BuildFixHints — deterministic error->fix-hint pattern table.
//
// ADDITIVE-ONLY contract: stamps a `fix_hint` string onto matched error objects
// in-place and returns a top-level `fix_hints[]` array; never mutates any
// pre-existing error field. Borrows the shared source DB the way the RI adapter
// does (game-thread ensure + FScopeLock(GetLock()); statement finalized before
// the lock releases; never a second handle on EngineSource.db).
// ============================================================================

TArray<TSharedPtr<FJsonValue>> FMonolithEditorActions::BuildFixHints(const TArray<TSharedPtr<FJsonValue>>& ErrorObjs)
{
	TArray<TSharedPtr<FJsonValue>> Hints;

	// Resolve the borrowed source DB (may be null — degrade gracefully).
	FMonolithSourceDatabase* SourceDb = nullptr;
	if (IsInGameThread() && GEditor)
	{
		if (UMonolithSourceSubsystem* SS = GEditor->GetEditorSubsystem<UMonolithSourceSubsystem>())
		{
			SourceDb = SS->GetDatabase();
		}
	}

	// LNK2019 on Z_Construct_* -> the missing module owns the referenced UClass.
	// Resolve owner module via the source DB; statement is finalized before the
	// lock releases (single borrow, no second handle).
	auto ResolveOwnerModuleForZConstruct = [SourceDb](const FString& SymbolFragment, FString& OutModule) -> bool
	{
		OutModule.Empty();
		if (!SourceDb || !SourceDb->GetRawHandle()) { return false; }
		// Z_Construct_UClass_UFoo_NoRegister -> recover the "UFoo" type name.
		FString TypeName;
		{
			FString S = SymbolFragment;
			int32 Idx = S.Find(TEXT("Z_Construct_UClass_"), ESearchCase::CaseSensitive);
			if (Idx != INDEX_NONE) { S = S.Mid(Idx + FCString::Strlen(TEXT("Z_Construct_UClass_"))); }
			// Trim any trailing _NoRegister / decoration after the type name.
			int32 Cut = INDEX_NONE;
			if (S.FindChar(TEXT('_'), Cut)) { TypeName = S.Left(Cut); }
			else { TypeName = S; }
		}
		if (TypeName.IsEmpty()) { return false; }

		bool bResolved = false;
		{
			FScopeLock Lock(&SourceDb->GetLock());
			FSQLiteDatabase* DB = SourceDb->GetRawHandle();
			if (!DB) { return false; }
			FSQLitePreparedStatement Stmt;
			if (Stmt.Create(*DB, TEXT(
				"SELECT m.name FROM symbols s "
				"JOIN files f ON f.id = s.file_id "
				"JOIN modules m ON m.id = f.module_id "
				"WHERE s.name = ? GROUP BY m.name LIMIT 1;")))
			{
				Stmt.SetBindingValueByIndex(1, TypeName);
				if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					Stmt.GetColumnValueByIndex(0, OutModule);
					bResolved = !OutModule.IsEmpty();
				}
				// Finalize the statement BEFORE the lock releases (borrow-contract end).
				Stmt.Destroy();
			}
		}
		return bResolved;
	};

	for (int32 Idx = 0; Idx < ErrorObjs.Num(); ++Idx)
	{
		const TSharedPtr<FJsonObject>* ErrObjPtr = nullptr;
		if (!ErrorObjs[Idx].IsValid() || !ErrorObjs[Idx]->TryGetObject(ErrObjPtr) || !ErrObjPtr) { continue; }
		const TSharedPtr<FJsonObject>& ErrObj = *ErrObjPtr;

		FString Message;
		ErrObj->TryGetStringField(TEXT("message"), Message);
		if (Message.IsEmpty()) { continue; }

		FString Pattern;
		FString Hint;

		// --- LNK2019 on Z_Construct_* (missing module dep). ---
		if (Message.Contains(TEXT("LNK2019")) && Message.Contains(TEXT("Z_Construct_")))
		{
			Pattern = TEXT("LNK2019_Z_Construct");
			FString OwnerModule;
			if (ResolveOwnerModuleForZConstruct(Message, OwnerModule))
			{
				Hint = FString::Printf(
					TEXT("Unresolved UClass constructor — the referenced type is owned by module '%s'. Add \"%s\" to your Build.cs PrivateDependencyModuleNames (or PublicDependencyModuleNames)."),
					*OwnerModule, *OwnerModule);
			}
			else
			{
				Hint = TEXT("Unresolved UClass constructor (Z_Construct_*) — the referenced type's owning module is missing from your Build.cs dependency list. Add the module that declares that UCLASS.");
			}
		}
		// --- DeveloperSettings-module LNK2019 (common gotcha). ---
		else if (Message.Contains(TEXT("LNK2019")) &&
				 (Message.Contains(TEXT("UDeveloperSettings")) || Message.Contains(TEXT("DeveloperSettings"))))
		{
			Pattern = TEXT("LNK2019_DeveloperSettings");
			Hint = TEXT("UDeveloperSettings lives in the 'DeveloperSettings' module (NOT 'Engine'). Add \"DeveloperSettings\" to your Build.cs dependency list.");
		}
		// --- C4996 deprecation. ---
		else if (Message.Contains(TEXT("C4996")))
		{
			Pattern = TEXT("C4996_deprecation");
			// Try to recover the deprecated identifier and attach the indexed message.
			FString DeprMsg;
			if (SourceDb && SourceDb->IsOpen())
			{
				// Parse a quoted identifier or "'Name': ..." form from the warning.
				FString Ident;
				int32 Q1 = Message.Find(TEXT("'"), ESearchCase::CaseSensitive);
				if (Q1 != INDEX_NONE)
				{
					FString Rest = Message.Mid(Q1 + 1);
					int32 Q2 = Rest.Find(TEXT("'"), ESearchCase::CaseSensitive);
					if (Q2 != INDEX_NONE) { Ident = Rest.Left(Q2); }
				}
				// Take a trailing :: scope as the method name.
				if (!Ident.IsEmpty())
				{
					int32 ScopeIdx = Ident.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					const FString LookupName = (ScopeIdx != INDEX_NONE) ? Ident.Mid(ScopeIdx + 2) : Ident;
					TArray<FString> Names; Names.Add(LookupName);
					TMap<FString, FMonolithDeprecationRow> Dep = SourceDb->GetDeprecationsBatch(Names);
					if (const FMonolithDeprecationRow* Row = Dep.Find(LookupName))
					{
						DeprMsg = FString::Printf(TEXT("'%s' is UE_DEPRECATED (%s): %s"), *LookupName, *Row->Version, *Row->Message);
					}
				}
			}
			Hint = DeprMsg.IsEmpty()
				? TEXT("Deprecated API used (C4996). Query source.check_deprecations with the symbol name for the replacement, or read the deprecation message in the warning text.")
				: DeprMsg;
		}
		// --- 'generated.h must be the last include'. ---
		else if (Message.Contains(TEXT("generated.h")) && Message.Contains(TEXT("last")))
		{
			Pattern = TEXT("generated_h_not_last");
			Hint = TEXT("The '*.generated.h' include must be the LAST #include in the header. Move it below every other include.");
		}

		if (!Hint.IsEmpty())
		{
			// ADDITIVE: stamp the per-error hint on the object (new field only).
			ErrObj->SetStringField(TEXT("fix_hint"), Hint);

			TSharedPtr<FJsonObject> HintObj = MakeShared<FJsonObject>();
			HintObj->SetNumberField(TEXT("error_index"), Idx);
			HintObj->SetStringField(TEXT("pattern"), Pattern);
			HintObj->SetStringField(TEXT("hint"), Hint);
			Hints.Add(MakeShared<FJsonValueObject>(HintObj));
		}
	}

	return Hints;
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		bool bCurrentlyCompiling = LiveCoding->IsCompiling();

		// Update tracked state if LC reports done but we haven't caught it yet
		if (bIsCompiling && !bCurrentlyCompiling)
		{
			bIsCompiling = false;
			if (LastCompileEndTimestamp < LastCompileTimestamp)
			{
				LastCompileEndTimestamp = FPlatformTime::Seconds();
			}
		}

		Root->SetBoolField(TEXT("live_coding_available"), true);
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
		Root->SetBoolField(TEXT("live_coding_enabled"), LiveCoding->IsEnabledForSession());
		Root->SetBoolField(TEXT("compiling"), bCurrentlyCompiling);
	}
	else
	{
		Root->SetBoolField(TEXT("live_coding_available"), false);
		Root->SetBoolField(TEXT("compiling"), false);
	}
#else
	Root->SetBoolField(TEXT("live_coding_available"), false);
	Root->SetBoolField(TEXT("compiling"), false);
#endif

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		Root->SetNumberField(TEXT("errors_since_compile"), CachedLogCapture->CountErrorsSince(LastCompileTimestamp));
	}
	else
	{
		Root->SetNumberField(TEXT("errors_since_compile"), 0);
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetBuildSummary(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Get error/warning counts from log capture
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	if (CachedLogCapture)
	{
		ErrorCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error);
		WarningCount = CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning);
	}

	Root->SetNumberField(TEXT("total_errors"), ErrorCount);
	Root->SetNumberField(TEXT("total_warnings"), WarningCount);

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		Root->SetBoolField(TEXT("compiling"), LiveCoding->IsCompiling());
		Root->SetBoolField(TEXT("live_coding_started"), LiveCoding->HasStarted());
	}
#endif

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchBuildOutput(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern;
	if (!Params->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid parameter: 'pattern' must be a non-empty string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 Limit = 100;
	TSharedPtr<FJsonValue> LimitJson = Params->TryGetField(TEXT("limit"));
	if (LimitJson.IsValid())
	{
		if (LimitJson->Type != EJson::Number)
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'limit' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
		double LimitValue = 0.0;
		if (!LimitJson->TryGetNumber(LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'limit' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Matches;

	if (CachedLogCapture)
	{
		// Search for compile-related messages matching the pattern
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(
			Pattern, TEXT(""), ELogVerbosity::VeryVerbose, Limit);

		Matches.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			Matches.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetStringField(TEXT("pattern"), Pattern);
	Root->SetNumberField(TEXT("match_count"), Matches.Num());
	Root->SetArrayField(TEXT("matches"), Matches);

	return FMonolithActionResult::Success(Root);
}

// --- Compile output ---

FMonolithActionResult FMonolithEditorActions::HandleGetCompileOutput(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetStringField(TEXT("last_compile_end_time"), TimestampToIso(LastCompileEndTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);
	Root->SetBoolField(TEXT("compiling"), bIsCompiling);

	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	TArray<TSharedPtr<FJsonValue>> LogLines;

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		// Get all log lines from compile-related categories since last compile
		TArray<FName> CompileCategories;
		CompileCategories.Add(FName(TEXT("LogLiveCoding")));
		CompileCategories.Add(FName(TEXT("LogCompile")));
		CompileCategories.Add(FName(TEXT("LogLinker")));

		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			LastCompileTimestamp, CompileCategories, ELogVerbosity::VeryVerbose, 500);

		LogLines.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogLines.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
			if (Entry.Verbosity <= ELogVerbosity::Error) ++ErrorCount;
			else if (Entry.Verbosity == ELogVerbosity::Warning) ++WarningCount;
		}
	}

	Root->SetNumberField(TEXT("error_count"), ErrorCount);
	Root->SetNumberField(TEXT("warning_count"), WarningCount);
	Root->SetNumberField(TEXT("log_line_count"), LogLines.Num());
	Root->SetArrayField(TEXT("compile_log"), LogLines);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLiveCodingDiagnostics(const TSharedPtr<FJsonObject>& Params)
{
	int32 MaxLogEntries = 50;
	if (Params.IsValid())
	{
		double MaxNum = static_cast<double>(MaxLogEntries);
		if (Params->TryGetNumberField(TEXT("max_log_entries"), MaxNum))
		{
			MaxLogEntries = FMath::Clamp(FMath::FloorToInt(MaxNum), 1, 200);
		}
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	bool bLiveCodingAvailable = false;
	bool bLiveCodingEnabledForSession = false;
	bool bLiveCodingEnabledByDefault = false;
	bool bLiveCodingStarted = false;
	bool bCurrentlyCompiling = bIsCompiling;

#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (LiveCoding)
	{
		bLiveCodingAvailable = true;
		bLiveCodingEnabledForSession = LiveCoding->IsEnabledForSession();
		bLiveCodingEnabledByDefault = LiveCoding->IsEnabledByDefault();
		bLiveCodingStarted = LiveCoding->HasStarted();
		bCurrentlyCompiling = LiveCoding->IsCompiling();

		if (bIsCompiling && !bCurrentlyCompiling)
		{
			bIsCompiling = false;
			if (LastCompileEndTimestamp < LastCompileTimestamp)
			{
				LastCompileEndTimestamp = FPlatformTime::Seconds();
			}
			if (LastCompileResult.Equals(TEXT("in_progress"), ESearchCase::IgnoreCase))
			{
				LastCompileResult = bPatchApplied ? TEXT("success") : TEXT("unknown");
			}
		}
	}
#endif

	const FString NormalizedResult = NormalizeLiveCodingCompileResult(LastCompileResult);

	Root->SetStringField(TEXT("last_result"), LastCompileResult);
	Root->SetStringField(TEXT("compile_result_normalized"), NormalizedResult);
	Root->SetStringField(TEXT("last_compile_time"), TimestampToIso(LastCompileTimestamp));
	Root->SetStringField(TEXT("last_compile_end_time"), TimestampToIso(LastCompileEndTimestamp));
	Root->SetBoolField(TEXT("patch_applied"), bPatchApplied);
	Root->SetBoolField(TEXT("live_coding_available"), bLiveCodingAvailable);
	Root->SetBoolField(TEXT("live_coding_enabled_for_session"), bLiveCodingEnabledForSession);
	Root->SetBoolField(TEXT("live_coding_enabled_by_default"), bLiveCodingEnabledByDefault);
	Root->SetBoolField(TEXT("live_coding_started"), bLiveCodingStarted);
	Root->SetBoolField(TEXT("compiling"), bCurrentlyCompiling);
	Root->SetNumberField(TEXT("max_log_entries"), MaxLogEntries);

	TArray<FName> CompileCategories;
	CompileCategories.Add(FName(TEXT("LogLiveCoding")));
	CompileCategories.Add(FName(TEXT("LogCompile")));
	CompileCategories.Add(FName(TEXT("LogLinker")));

	TArray<TSharedPtr<FJsonValue>> LogEntriesJson;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;
	FString DiagnosticSource = TEXT("none");

	if (CachedLogCapture && LastCompileTimestamp > 0.0)
	{
		const TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetEntriesSince(
			LastCompileTimestamp,
			CompileCategories,
			ELogVerbosity::VeryVerbose,
			MaxLogEntries);

		LogEntriesJson.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			if (DiagnosticSource.Equals(TEXT("none"), ESearchCase::IgnoreCase))
			{
				DiagnosticSource = Entry.Category.ToString();
			}
			if (Entry.Verbosity <= ELogVerbosity::Error)
			{
				++ErrorCount;
			}
			else if (Entry.Verbosity == ELogVerbosity::Warning)
			{
				++WarningCount;
			}
			LogEntriesJson.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	const bool bDiagnosticSourceFresh = LogEntriesJson.Num() > 0;
	TArray<TSharedPtr<FJsonValue>> UbtDiagnosticsJson;
	Root->SetStringField(TEXT("diagnostic_source"), DiagnosticSource);
	Root->SetBoolField(TEXT("diagnostic_source_fresh"), bDiagnosticSourceFresh);
	Root->SetNumberField(TEXT("diagnostic_count"), LogEntriesJson.Num());
	Root->SetNumberField(TEXT("error_count"), ErrorCount);
	Root->SetNumberField(TEXT("warning_count"), WarningCount);
	Root->SetArrayField(TEXT("log_excerpt"), LogEntriesJson);
	Root->SetArrayField(TEXT("ubt_diagnostics"), UbtDiagnosticsJson);
	Root->SetStringField(TEXT("ubt_diagnostic_source"), TEXT("not_checked"));
	Root->SetBoolField(TEXT("ubt_diagnostic_source_fresh"), false);

	FString Message;
	if (!bLiveCodingAvailable)
	{
		Message = TEXT("Live Coding module is not available in this editor session.");
	}
	else if (LastCompileTimestamp <= 0.0)
	{
		Message = TEXT("No Live Coding compile has been recorded by Monolith in this editor session.");
	}
	else if (bCurrentlyCompiling)
	{
		Message = TEXT("Live Coding compile is currently in progress.");
	}
	else if (NormalizedResult.Equals(TEXT("failed"), ESearchCase::IgnoreCase))
	{
		Message = FString::Printf(TEXT("Live Coding compile failed with %d errors and %d warnings in fresh captured diagnostics."), ErrorCount, WarningCount);
	}
	else if (NormalizedResult.Equals(TEXT("success"), ESearchCase::IgnoreCase))
	{
		Message = FString::Printf(TEXT("Live Coding compile succeeded with %d fresh diagnostic entries."), LogEntriesJson.Num());
	}
	else if (!bDiagnosticSourceFresh)
	{
		Message = FString::Printf(TEXT("Live Coding result is '%s', but no fresh compile diagnostics were captured."), *NormalizedResult);
	}
	else
	{
		Message = FString::Printf(TEXT("Live Coding result is '%s' with %d fresh diagnostic entries."), *NormalizedResult, LogEntriesJson.Num());
	}

	Root->SetStringField(TEXT("message"), Message);
	return FMonolithActionResult::Success(Root);
}

// --- Log actions ---

FMonolithActionResult FMonolithEditorActions::HandleGetRecentLogs(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 100;
	double CountValue = 0.0;
	TSharedPtr<FJsonValue> CountJson = Params->TryGetField(TEXT("count"));
	if (CountJson.IsValid())
	{
		if (CountJson->Type != EJson::Number || !CountJson->TryGetNumber(CountValue))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'count' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
		Count = static_cast<int32>(CountValue);
	}
	else
	{
		TSharedPtr<FJsonValue> MaxJson = Params->TryGetField(TEXT("max"));
		if (MaxJson.IsValid())
		{
			if (MaxJson->Type != EJson::Number || !MaxJson->TryGetNumber(CountValue))
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'max' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			Count = static_cast<int32>(CountValue);
		}
	}
	Count = FMath::Clamp(Count, 1, 1000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		LogArr.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleSearchLogs(const TSharedPtr<FJsonObject>& Params)
{
	FString Pattern;
	if (Params->HasField(TEXT("pattern")) && !Params->TryGetStringField(TEXT("pattern"), Pattern))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'pattern' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Category;
	if (Params->HasField(TEXT("category")) && !Params->TryGetStringField(TEXT("category"), Category))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'category' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString VerbosityStr;
	if (Params->HasField(TEXT("verbosity")) && !Params->TryGetStringField(TEXT("verbosity"), VerbosityStr))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'verbosity' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	ELogVerbosity::Type MaxVerbosity = VerbosityStr.IsEmpty() ? ELogVerbosity::VeryVerbose : StringToVerbosity(VerbosityStr);

	int32 Limit = 200;
	double LimitValue = 0.0;
	if (Params->TryGetNumberField(TEXT("limit"), LimitValue))
	{
		Limit = static_cast<int32>(LimitValue);
	}
	else if (Params->HasField(TEXT("limit")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'limit' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
	}
	Limit = FMath::Clamp(Limit, 1, 2000);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LogArr;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->SearchEntries(Pattern, Category, MaxVerbosity, Limit);
		LogArr.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			LogArr.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
	}

	Root->SetNumberField(TEXT("match_count"), LogArr.Num());
	Root->SetArrayField(TEXT("entries"), LogArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleTailLog(const TSharedPtr<FJsonObject>& Params)
{
	int32 Count = 50;
	double CountValue = 0.0;
	if (Params->TryGetNumberField(TEXT("count"), CountValue))
	{
		Count = static_cast<int32>(CountValue);
	}
	else if (Params->HasField(TEXT("count")))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'count' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
	}
	Count = FMath::Clamp(Count, 1, 500);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Lines;

	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> Entries = CachedLogCapture->GetRecentEntries(Count);
		Lines.Reserve(Entries.Num());
		for (const FMonolithLogEntry& Entry : Entries)
		{
			FString Line = FString::Printf(TEXT("[%s][%s] %s"),
				*Entry.Category.ToString(),
				*VerbosityToString(Entry.Verbosity),
				*Entry.Message);
			Lines.Add(MakeShared<FJsonValueString>(Line));
		}
	}

	Root->SetNumberField(TEXT("count"), Lines.Num());
	Root->SetArrayField(TEXT("lines"), Lines);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogCategories(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CatArr;

	if (CachedLogCapture)
	{
		TArray<FString> Categories = CachedLogCapture->GetActiveCategories();
		Categories.Sort();
		for (const FString& Cat : Categories)
		{
			CatArr.Add(MakeShared<FJsonValueString>(Cat));
		}
	}

	Root->SetNumberField(TEXT("count"), CatArr.Num());
	Root->SetArrayField(TEXT("categories"), CatArr);

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetLogStats(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	if (CachedLogCapture)
	{
		Root->SetNumberField(TEXT("total"), CachedLogCapture->GetTotalCount());
		Root->SetNumberField(TEXT("fatal"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Fatal));
		Root->SetNumberField(TEXT("error"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Error));
		Root->SetNumberField(TEXT("warning"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Warning));
		Root->SetNumberField(TEXT("log"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Log));
		Root->SetNumberField(TEXT("verbose"), CachedLogCapture->GetCountByVerbosity(ELogVerbosity::Verbose));
	}
	else
	{
		Root->SetNumberField(TEXT("total"), 0);
		Root->SetStringField(TEXT("status"), TEXT("log_capture_not_initialized"));
	}

	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleGetCrashContext(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// Check for crash log file on disk
	FString CrashLogPath = FPaths::ProjectLogDir() / TEXT("CrashContext.runtime-xml");
	bool bHasCrashLog = FPaths::FileExists(CrashLogPath);
	Root->SetBoolField(TEXT("has_crash_context"), bHasCrashLog);

	if (bHasCrashLog)
	{
		FString CrashXml;
		if (FFileHelper::LoadFileToString(CrashXml, *CrashLogPath))
		{
			// Truncate if very large
			if (CrashXml.Len() > 4096)
			{
				CrashXml = CrashXml.Left(4096) + TEXT("...(truncated)");
			}
			Root->SetStringField(TEXT("crash_xml"), CrashXml);
		}
	}

	// Also check ensure log
	FString EnsureLogPath = FPaths::ProjectLogDir() / TEXT("Ensures.log");
	if (FPaths::FileExists(EnsureLogPath))
	{
		FString EnsureLog;
		if (FFileHelper::LoadFileToString(EnsureLog, *EnsureLogPath))
		{
			if (EnsureLog.Len() > 4096)
			{
				EnsureLog = EnsureLog.Right(4096);
			}
			Root->SetStringField(TEXT("ensure_log"), EnsureLog);
		}
	}

	// Provide recent errors/fatals from log capture
	if (CachedLogCapture)
	{
		TArray<FMonolithLogEntry> ErrorEntries = CachedLogCapture->SearchEntries(
			TEXT(""), TEXT(""), ELogVerbosity::Error, 20);
		TArray<TSharedPtr<FJsonValue>> RecentErrors;
		RecentErrors.Reserve(ErrorEntries.Num());
		for (const FMonolithLogEntry& Entry : ErrorEntries)
		{
			RecentErrors.Add(MakeShared<FJsonValueObject>(LogEntryToJson(Entry)));
		}
		Root->SetArrayField(TEXT("recent_errors"), RecentErrors);
	}

	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// run_console_command — execute a console command on the active world
// ---------------------------------------------------------------------------
FMonolithActionResult FMonolithEditorActions::HandleRunConsoleCommand(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required field: command"));
	}

	// Resolve a target world: prefer an active PIE world (so exec functions on
	// the player character actually fire), fall back to the editor world.
	UWorld* TargetWorld = nullptr;
	FString WorldType = TEXT("none");
	if (GEditor)
	{
		for (const FWorldContext& Context : GEditor->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				TargetWorld = Context.World();
				WorldType = TEXT("pie");
				break;
			}
		}
		if (!TargetWorld)
		{
			TargetWorld = GEditor->GetEditorWorldContext().World();
			WorldType = TEXT("editor");
		}
	}

	if (!TargetWorld)
	{
		return FMonolithActionResult::Error(TEXT("No usable world found (no PIE active and no editor world)"));
	}

	// Prefer the player controller's command path so exec UFUNCTIONs on the
	// possessed pawn (BowLoop, WalkLoop, Cam3P, …) get dispatched. Fall back
	// to the world-level Exec for cheats that don't need a PC.
	bool bExecutedViaPC = false;
	if (APlayerController* PC = TargetWorld->GetFirstPlayerController())
	{
		PC->ConsoleCommand(Command, /*bWriteToLog=*/true);
		bExecutedViaPC = true;
	}
	else
	{
		if (!GEngine)
		{
			return FMonolithActionResult::Error(TEXT("GEngine is null — run_console_command requires engine context."));
		}
		GEngine->Exec(TargetWorld, *Command);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("command"), Command);
	Root->SetStringField(TEXT("world"), WorldType);
	Root->SetBoolField(TEXT("via_player_controller"), bExecutedViaPC);
	return FMonolithActionResult::Success(Root);
}

// ---------------------------------------------------------------------------
// start_pie / stop_pie — drive Play-In-Editor sessions programmatically
// ---------------------------------------------------------------------------
UWorld* FMonolithEditorActions::FindActivePieWorld()
{
	if (!GEditor)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEditor->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}

namespace
{
	// #5/#6 shared map-load guard. RequestEndPlayMap() only QUEUES the EndPlayMap
	// teardown (sets bRequestEndPlayMapQueued) — the actual EditorDestroyWorld runs on
	// the NEXT editor tick. Calling ULevelEditorSubsystem::LoadLevel while a PIE world
	// is still resident drives Map_Load -> EditorDestroyWorld, which asserts
	// "World Memory Leaks: N" because the live PIE world keeps references rooted.
	//
	// Policy (identical for run_pie_smoke #5 and load_level #6):
	//   1. A PIE world is resident AND a smoke session is still running => REFUSE
	//      (backstop — caller must stop the session first).
	//   2. A PIE world is resident with no running session => drive teardown to
	//      completion (RequestEndPlayMap + synchronous EndPlayMap, bounded), force a GC,
	//      then proceed.
	//   3. No resident PIE world => proceed immediately.
	//
	// Returns true when it is safe to call LoadLevel; false (with OutError) to refuse.
	bool EnsureNoResidentPieWorldBeforeMapLoad(FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor unavailable — cannot evaluate PIE residency before map load.");
			return false;
		}

		UWorld* ResidentPie = FMonolithEditorActions::FindActivePieWorld();
		if (!ResidentPie)
		{
			return true; // No resident PIE world — safe to load.
		}

		// Backstop: refuse while a smoke session is still observing the world.
		if (FPieSmokeSessionManager::Get().HasRunningSessions())
		{
			OutError = TEXT("A PIE smoke session is still resident; stop it before loading a new map "
				"(call stop_pie_smoke, then retry). Loading now would leak the live PIE world.");
			return false;
		}

		// No running session, but a PIE world lingers (e.g. the deferred EndPlayMap from a
		// completed session has not ticked yet). Drive teardown to completion.
		// RequestEndPlayMap() queues; EndPlayMap() flushes synchronously and is reentrancy-
		// guarded (bIsEndingPlay), so the bounded retry loop is safe.
		GEditor->RequestEndPlayMap();

		constexpr int32 MaxTeardownIterations = 8;
		for (int32 Iter = 0; Iter < MaxTeardownIterations; ++Iter)
		{
			if (!FMonolithEditorActions::FindActivePieWorld())
			{
				break; // Teardown complete.
			}
			// EndPlayMap is the public UNREALED_API method the editor runs on the next
			// tick; calling it directly from this (non-PIE-tick) MCP handler stack frame
			// performs the teardown synchronously. Idempotent once bIsEndingPlay is set.
			GEditor->EndPlayMap();
		}

		if (FMonolithEditorActions::FindActivePieWorld())
		{
			OutError = TEXT("PIE world teardown did not complete within the bounded retry budget; "
				"refusing the map load to avoid a world memory leak. Stop PIE and retry.");
			return false;
		}

		// The torn-down PIE world keeps references rooted until a GC pass runs. Force one
		// before LoadLevel so EditorDestroyWorld's leak check sees a clean slate.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		return true;
	}
}

namespace
{
	// One errored Blueprint discovered by the PIE pre-flight scan.
	struct FErroredBlueprintEntry
	{
		FString Name;
		FString Path;
	};

	// Scan every loaded UBlueprint for the same condition the engine's
	// ResolveDirtyBlueprints (PlayLevel.cpp) tests before raising the blocking
	// "unresolved compiler errors" PIE prompt: status == BS_Error AND the BP still
	// wants to warn on PIE (bDisplayCompilePIEWarning). Starting PIE on such a world
	// pops a Slate modal that runs a nested loop on the game thread, starving the
	// in-process MCP HTTP server until a human clicks.
	void ScanErroredBlueprints(TArray<FErroredBlueprintEntry>& OutErrored)
	{
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* Blueprint = *It;
			if (!IsValid(Blueprint))
			{
				continue;
			}
			if (Blueprint->Status == BS_Error && Blueprint->bDisplayCompilePIEWarning)
			{
				FErroredBlueprintEntry Entry;
				Entry.Name = Blueprint->GetName();
				Entry.Path = Blueprint->GetPathName();
				OutErrored.Add(MoveTemp(Entry));
			}
		}
	}

	// Build a JSON array of {name, path} from a scan result.
	TArray<TSharedPtr<FJsonValue>> ErroredBlueprintsToJson(const TArray<FErroredBlueprintEntry>& Errored)
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		Items.Reserve(Errored.Num());
		for (const FErroredBlueprintEntry& Entry : Errored)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), Entry.Name);
			Obj->SetStringField(TEXT("path"), Entry.Path);
			Items.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Items;
	}
}

bool FMonolithEditorActions::StartPieInternal(FString& OutError, bool bSuppressModals)
{
	if (!GUnrealEd)
	{
		OutError = TEXT("GUnrealEd not available");
		return false;
	}

	// Reject if a PIE session is already running so we don't queue duplicates.
	if (FindActivePieWorld())
	{
		OutError = TEXT("PIE already running");
		return false;
	}

	// Pin to in-viewport mode so the action is independent of the user's
	// last-used PIE flavour (Simulate / NewWindow / etc.).
	FLevelEditorModule& LevelEditorModule = FModuleManager::GetModuleChecked<FLevelEditorModule>("LevelEditor");
	TSharedPtr<IAssetViewport> ActiveLevelViewport = LevelEditorModule.GetFirstActiveViewport();
	if (!ActiveLevelViewport.IsValid())
	{
		OutError = TEXT("No active level viewport — cannot pin PIE to in-viewport mode.");
		return false;
	}

	FRequestPlaySessionParams SessionParams;
	SessionParams.WorldType = EPlaySessionWorldType::PlayInEditor;
	SessionParams.DestinationSlateViewport = ActiveLevelViewport;

	// In suppress mode, force the unattended-script global true ONLY around the PIE
	// request. The engine's ShowBlueprintErrorDialog (PlayLevel.cpp) and
	// FSlateApplication::AddModalWindow both early-out on GIsRunningUnattendedScript,
	// so the compile-error prompt resolves to its default instead of blocking the
	// game thread (and with it the in-process MCP server). Self-restoring via RAII;
	// game-thread only, matching the engine's canonical usage. Not blanket-applied to
	// all MCP dispatch — only the one call that can trigger the PIE prompt.
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript,
		bSuppressModals ? true : GIsRunningUnattendedScript);

	GUnrealEd->RequestPlaySession(SessionParams);
	GUnrealEd->StartQueuedPlaySessionRequest();
	return true;
}

bool FMonolithEditorActions::StopPieInternal()
{
	if (!GEditor)
	{
		return false;
	}

	const bool bWasRunning = FindActivePieWorld() != nullptr;
	if (bWasRunning)
	{
		GEditor->RequestEndPlayMap();
	}
	return bWasRunning;
}

FMonolithActionResult FMonolithEditorActions::HandleStartPIE(const TSharedPtr<FJsonObject>& Params)
{
	if (!GUnrealEd) return FMonolithActionResult::Error(TEXT("GUnrealEd not available"));

	// Reject if a PIE session is already running so we don't queue duplicates.
	if (FindActivePieWorld())
	{
		TSharedPtr<FJsonObject> AlreadyRunning = MakeShared<FJsonObject>();
		AlreadyRunning->SetBoolField(TEXT("started"), false);
		AlreadyRunning->SetStringField(TEXT("reason"), TEXT("PIE already running"));
		return FMonolithActionResult::Success(AlreadyRunning);
	}

	FString StartError;
	if (!StartPieInternal(StartError))
	{
		return FMonolithActionResult::Error(StartError);
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("started"), true);
	Root->SetStringField(TEXT("mode"), TEXT("in_viewport"));
	return FMonolithActionResult::Success(Root);
}

FMonolithActionResult FMonolithEditorActions::HandleStopPIE(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor) return FMonolithActionResult::Error(TEXT("GEditor not available"));

	const bool bWasRunning = StopPieInternal();

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("stopped"), bWasRunning);
	return FMonolithActionResult::Success(Root);
}

// --- Capture helpers ---

bool FMonolithEditorActions::RenderAndSaveCapture(
	USceneCaptureComponent2D* CaptureComp,
	UTextureRenderTarget2D* RT,
	int32 ResX, int32 ResY,
	const FString& OutputPath)
{
	if (!CaptureComp || !RT)
	{
		return false;
	}

	// Trigger the capture — submits render commands to the render thread
	CaptureComp->CaptureScene();

	// Use GameThread_GetRenderTargetResource — the non-GameThread variant
	// asserts IsInRenderingThread() which crashes when called from game thread.
	FTextureRenderTargetResource* RTResource = RT->GameThread_GetRenderTargetResource();
	if (!RTResource)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: Failed to get RT resource"));
		return false;
	}

	// ReadPixels internally calls FlushRenderingCommands() to synchronize the GPU readback
	TArray<FColor> Pixels;
	bool bReadOk = RTResource->ReadPixels(Pixels);

	if (!bReadOk || Pixels.Num() == 0)
	{
		UE_LOG(LogMonolith, Error, TEXT("CaptureScenePreview: ReadPixels failed (read=%d, count=%d)"),
			bReadOk, Pixels.Num());
		return false;
	}

	// Ensure output directory exists
	FString Dir = FPaths::GetPath(OutputPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	// Encode as PNG and save
	FImage Image;
	Image.Init(ResX, ResY, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));

	return FImageUtils::SaveImageAutoFormat(*OutputPath, Image);
}

bool FMonolithEditorActions::CaptureNiagaraFrame(
	UNiagaraSystem* System,
	float SeekTime,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource)
{
	if (!System)
	{
		return false;
	}

	// Create preview scene with black background (no environment lighting)
	// VFX effects (especially fire, emissives) need a dark background to evaluate properly
	FPreviewScene::ConstructionValues CVs;
	CVs.bDefaultLighting = false;
	CVs.LightBrightness = 0.0f;
	CVs.SkyBrightness = 0.0f;
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(CVs));
	PreviewScene->SetFloorVisibility(false);
	PreviewScene->SetEnvironmentVisibility(false);

	// Create Niagara component
	UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	NiagaraComp->CastShadow = false;
	NiagaraComp->bCastDynamicShadow = false;
	NiagaraComp->SetAllowScalability(false);
	NiagaraComp->SetAsset(System);
	NiagaraComp->SetForceSolo(true);
	NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
	NiagaraComp->SetCanRenderWhileSeeking(true);
	NiagaraComp->SetMaxSimTime(0.0f);
	NiagaraComp->Activate(true);

	PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

	// Seek to desired time
	const float SeekDelta = 1.0f / 30.0f;
	UWorld* World = NiagaraComp->GetWorld();

	if (SeekTime > 0.0f)
	{
		NiagaraComp->SetSeekDelta(SeekDelta);
		NiagaraComp->SeekToDesiredAge(SeekTime);

		if (World)
		{
			World->TimeSeconds = SeekTime;
			World->UnpausedTimeSeconds = SeekTime;
			World->RealTimeSeconds = SeekTime;
			World->DeltaRealTimeSeconds = SeekDelta;
			World->DeltaTimeSeconds = SeekDelta;
			World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f);
		}

		NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);

		if (World)
		{
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);  // Wait for GPU
			}
		}
	}

	// Warm-up ticks: pump the world + component so GPU particle buffers are populated.
	// Runs even at SeekTime==0 — particles need frames to spawn and fill GPU buffers.
	if (World)
	{
		constexpr int32 WarmUpFrames = 3;
		for (int32 i = 0; i < WarmUpFrames; i++)
		{
			World->Tick(ELevelTick::LEVELTICK_PauseTick, SeekDelta);
			NiagaraComp->TickComponent(SeekDelta, ELevelTick::LEVELTICK_All, nullptr);
			World->SendAllEndOfFrameUpdates();
			if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
			{
				WorldManager->FlushComputeAndDeferredQueues(true);
			}
			FlushRenderingCommands();
		}
	}

	// Wait for any in-flight shader compilation before capture
	if (GShaderCompilingManager)
	{
		GShaderCompilingManager->FinishAllCompilation();
	}
	FlushRenderingCommands();

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = FLinearColor::Black;
	RT->UpdateResourceImmediate(true);

	// Create scene capture component (same as Baker)
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	// Register with the preview scene's world (World already declared above)
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(NiagaraComp);

	return bSuccess;
}

bool FMonolithEditorActions::CaptureMaterialFrame(
	UMaterialInterface* Material,
	const FString& MeshType,
	const FVector& CameraLocation,
	const FRotator& CameraRotation,
	float FOV,
	int32 ResX, int32 ResY,
	const FString& OutputPath,
	ESceneCaptureSource CaptureSource,
	float UVTiling,
	const FLinearColor& BackgroundColor)
{
	if (!Material)
	{
		return false;
	}

	// Create preview scene
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UPrimitiveComponent* SpawnedMeshComp = nullptr;

	if (!FMath::IsNearlyEqual(UVTiling, 1.0f) && (MeshType.Equals(TEXT("plane"), ESearchCase::IgnoreCase) || MeshType.IsEmpty()))
	{
		// Build a procedural quad with scaled UVs for tiling preview
		UProceduralMeshComponent* ProcMeshComp = NewObject<UProceduralMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);

		const float HalfSize = 100.0f; // 200x200 cm quad
		TArray<FVector> Vertices;
		Vertices.Add(FVector(-HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize, -HalfSize, 0.0f));
		Vertices.Add(FVector( HalfSize,  HalfSize, 0.0f));
		Vertices.Add(FVector(-HalfSize,  HalfSize, 0.0f));

		TArray<int32> Triangles = { 0, 1, 2, 0, 2, 3 };

		TArray<FVector> Normals;
		Normals.Init(FVector::UpVector, 4);

		TArray<FVector2D> UV0;
		UV0.Add(FVector2D(0.0f, 0.0f));
		UV0.Add(FVector2D(UVTiling, 0.0f));
		UV0.Add(FVector2D(UVTiling, UVTiling));
		UV0.Add(FVector2D(0.0f, UVTiling));

		TArray<FColor> VertexColors;
		VertexColors.Init(FColor::White, 4);

		TArray<FProcMeshTangent> Tangents;
		Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), 4);

		ProcMeshComp->CreateMeshSection(0, Vertices, Triangles, Normals, UV0, VertexColors, Tangents, false);
		ProcMeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		ProcMeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = ProcMeshComp;
	}
	else
	{
		// Standard static mesh path
		FString MeshPath;
		if (MeshType.Equals(TEXT("sphere"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Sphere");
		}
		else if (MeshType.Equals(TEXT("cube"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cube");
		}
		else if (MeshType.Equals(TEXT("cylinder"), ESearchCase::IgnoreCase))
		{
			MeshPath = TEXT("/Engine/BasicShapes/Cylinder");
		}
		else // default: plane
		{
			MeshPath = TEXT("/Engine/BasicShapes/Plane");
		}

		UStaticMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(*MeshPath);
		if (!Mesh)
		{
			UE_LOG(LogMonolith, Error, TEXT("CaptureMaterialFrame: Failed to load mesh %s"), *MeshPath);
			return false;
		}

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		MeshComp->SetStaticMesh(Mesh);
		MeshComp->SetMaterial(0, const_cast<UMaterialInterface*>(Material));
		MeshComp->SetRelativeScale3D(FVector(2.0f, 2.0f, 1.0f));
		SpawnedMeshComp = MeshComp;
	}

	PreviewScene->AddComponent(SpawnedMeshComp, SpawnedMeshComp->GetRelativeTransform());

	// Create render target
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	RT->InitAutoFormat(ResX, ResY);
	RT->ClearColor = BackgroundColor;
	RT->UpdateResourceImmediate(true);

	// Create scene capture
	USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
		GetTransientPackage(), NAME_None, RF_Transient);
	CaptureComp->bTickInEditor = false;
	CaptureComp->SetComponentTickEnabled(false);
	CaptureComp->SetVisibility(true);
	CaptureComp->bCaptureEveryFrame = false;
	CaptureComp->bCaptureOnMovement = false;
	CaptureComp->TextureTarget = RT;
	CaptureComp->CaptureSource = CaptureSource;
	CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
	CaptureComp->FOVAngle = FOV;
	CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

	UWorld* World = PreviewScene->GetWorld();
	CaptureComp->RegisterComponentWithWorld(World);
	CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

	// Capture and save
	bool bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

	// Cleanup
	CaptureComp->TextureTarget = nullptr;
	CaptureComp->UnregisterComponent();
	PreviewScene->RemoveComponent(SpawnedMeshComp);

	return bSuccess;
}

// --- Capture action handlers ---

FMonolithActionResult FMonolithEditorActions::HandleCaptureScenePreview(
	const TSharedPtr<FJsonObject>& Params)
{
	// Parse required params
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	FString AssetType;
	Params->TryGetStringField(TEXT("asset_type"), AssetType);

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	// Parse optional params
	float SeekTime = 0.0f;
	if (Params->HasField(TEXT("seek_time")))
	{
		double TempSeekTime = 0.0;
		if (Params->TryGetNumberField(TEXT("seek_time"), TempSeekTime))
		{
			SeekTime = (float)TempSeekTime;
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'seek_time' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	FString PreviewMesh = TEXT("plane");
	if (Params->HasField(TEXT("preview_mesh")))
	{
		if (!Params->TryGetStringField(TEXT("preview_mesh"), PreviewMesh))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'preview_mesh' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ResArray = nullptr;
		if (Params->TryGetArrayField(TEXT("resolution"), ResArray) && ResArray)
		{
			if (ResArray->Num() >= 2)
			{
				double TempX = 0.0, TempY = 0.0;
				if ((*ResArray)[0]->TryGetNumber(TempX) && (*ResArray)[1]->TryGetNumber(TempY))
				{
					ResX = (int32)TempX;
					ResY = (int32)TempY;
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'resolution' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'resolution' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// Parse camera
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;

	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;

		// Handle both object and string-serialized (Claude Code quirk)
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr;
			if (Params->TryGetStringField(TEXT("camera"), CameraStr))
			{
				if (!CameraStr.IsEmpty())
				{
					ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
					CameraObj = &ParsedCamera;
				}
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'camera' must be an object or JSON string"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
				if ((*CameraObj)->TryGetArrayField(TEXT("location"), Loc) && Loc)
				{
					if (Loc->Num() >= 3)
					{
						double TempX = 0.0, TempY = 0.0, TempZ = 0.0;
						if ((*Loc)[0]->TryGetNumber(TempX) && (*Loc)[1]->TryGetNumber(TempY) && (*Loc)[2]->TryGetNumber(TempZ))
						{
							CameraLocation = FVector(TempX, TempY, TempZ);
						}
						else
						{
							return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.location' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
						}
					}
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.location' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
				if ((*CameraObj)->TryGetArrayField(TEXT("rotation"), Rot) && Rot)
				{
					if (Rot->Num() >= 3)
					{
						double TempPitch = 0.0, TempYaw = 0.0, TempRoll = 0.0;
						if ((*Rot)[0]->TryGetNumber(TempPitch) && (*Rot)[1]->TryGetNumber(TempYaw) && (*Rot)[2]->TryGetNumber(TempRoll))
						{
							CameraRotation = FRotator(TempPitch, TempYaw, TempRoll);
						}
						else
						{
							return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.rotation' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
						}
					}
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.rotation' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				double TempFov = 0.0;
				if ((*CameraObj)->TryGetNumberField(TEXT("fov"), TempFov))
				{
					FOV = (float)TempFov;
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.fov' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
		}
	}

	// Generate output path
	FString OutputPath;
	if (Params->HasField(TEXT("output_path")))
	{
		if (Params->TryGetStringField(TEXT("output_path"), OutputPath))
		{
			if (FPaths::IsRelative(OutputPath))
			{
				OutputPath = FPaths::ProjectDir() / OutputPath;
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'output_path' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputPath = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s.png"), *Timestamp, *SafeName);
	}

	// UE's FHttpServerModule dispatches handlers on the game thread via FTicker,
	// so we're already on the game thread here. Call capture functions directly.
	check(IsInGameThread());

	double StartTime = FPlatformTime::Seconds();
	bool bSuccess = false;

	if (AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(AssetPath);
		if (!System)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));
		}
		bSuccess = CaptureNiagaraFrame(System, SeekTime, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath);
	}
	else if (AssetType.Equals(TEXT("material"), ESearchCase::IgnoreCase))
	{
		UMaterialInterface* Material = FMonolithAssetUtils::LoadAssetByPath<UMaterialInterface>(AssetPath);
		if (!Material)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load material: %s"), *AssetPath));
		}
		float UVTiling = 1.0f;
		if (Params->HasField(TEXT("uv_tiling")))
		{
			double TempUVTiling = 0.0;
			if (Params->TryGetNumberField(TEXT("uv_tiling"), TempUVTiling))
			{
				UVTiling = (float)TempUVTiling;
				if (UVTiling <= 0.0f) UVTiling = 1.0f;
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'uv_tiling' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		FLinearColor BgColor(0.18f, 0.18f, 0.18f);
		if (Params->HasField(TEXT("background_color")))
		{
			const TArray<TSharedPtr<FJsonValue>>* BgArr = nullptr;
			if (Params->TryGetArrayField(TEXT("background_color"), BgArr) && BgArr)
			{
				if (BgArr->Num() >= 3)
				{
					double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
					if (!(*BgArr)[0]->TryGetNumber(R) || !(*BgArr)[1]->TryGetNumber(G) || !(*BgArr)[2]->TryGetNumber(B) || (BgArr->Num() >= 4 && !(*BgArr)[3]->TryGetNumber(A)))
					{
						return FMonolithActionResult::Error(TEXT("Invalid param: 'background_color' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
					}
					BgColor = FLinearColor((float)R, (float)G, (float)B, (float)A);
				}
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'background_color' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		bSuccess = CaptureMaterialFrame(Material, PreviewMesh, CameraLocation, CameraRotation,
			FOV, ResX, ResY, OutputPath, ESceneCaptureSource::SCS_FinalToneCurveHDR,
			UVTiling, BgColor);
	}
	else if (AssetType.Equals(TEXT("static_mesh"), ESearchCase::IgnoreCase))
	{
		// Load asset.
		UStaticMesh* Mesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(AssetPath);
		if (!Mesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load static mesh: %s"), *AssetPath));
		}

		// Allocate transient preview scene (matches CaptureMaterialFrame pattern).
		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
		PreviewScene->SetFloorVisibility(false);

		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		MeshComp->SetStaticMesh(Mesh);
		PreviewScene->AddComponent(MeshComp, MeshComp->GetRelativeTransform());

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(ResX, ResY);
		RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComp->bTickInEditor = false;
		CaptureComp->SetComponentTickEnabled(false);
		CaptureComp->SetVisibility(true);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->TextureTarget = RT;
		CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->FOVAngle = FOV;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

		UWorld* World = PreviewScene->GetWorld();
		CaptureComp->RegisterComponentWithWorld(World);
		CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

		bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

		// Cleanup.
		CaptureComp->TextureTarget = nullptr;
		CaptureComp->UnregisterComponent();
		PreviewScene->RemoveComponent(MeshComp);
	}
	else if (AssetType.Equals(TEXT("skeletal_mesh"), ESearchCase::IgnoreCase))
	{
		USkeletalMesh* SkelMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(AssetPath);
		if (!SkelMesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load skeletal mesh: %s"), *AssetPath));
		}

		// Optional animation_path — when present, pose at seek_time. seek_time is
		// already parsed at the top of this function (default 0.0).
		UAnimSequence* AnimSeq = nullptr;
		if (Params->HasField(TEXT("animation_path")))
		{
			FString AnimPath;
			Params->TryGetStringField(TEXT("animation_path"), AnimPath);
			if (!AnimPath.IsEmpty())
			{
				AnimSeq = FMonolithAssetUtils::LoadAssetByPath<UAnimSequence>(AnimPath);
				if (!AnimSeq)
				{
					return FMonolithActionResult::Error(
						FString::Printf(TEXT("Failed to load animation sequence: %s"), *AnimPath));
				}
			}
		}

		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
		PreviewScene->SetFloorVisibility(false);

		USkeletalMeshComponent* SkelMeshComp = NewObject<USkeletalMeshComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		SkelMeshComp->SetSkeletalMesh(SkelMesh);

		if (AnimSeq)
		{
			// Pair-and-evaluate posing per UE 5.7 contract: PlayAnimation puts the
			// component into single-node-instance mode + assigns the asset, then
			// SetPosition forces evaluation at the target time without ticking.
			SkelMeshComp->PlayAnimation(AnimSeq, /*bLooping=*/false);
			SkelMeshComp->SetPosition(SeekTime, /*bFireNotifies=*/false);
		}

		PreviewScene->AddComponent(SkelMeshComp, SkelMeshComp->GetRelativeTransform());

		// Tick the world once so the pose evaluation lands before capture.
		UWorld* World = PreviewScene->GetWorld();
		if (World)
		{
			World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f);
			SkelMeshComp->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
			World->SendAllEndOfFrameUpdates();
		}

		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(ResX, ResY);
		RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComp->bTickInEditor = false;
		CaptureComp->SetComponentTickEnabled(false);
		CaptureComp->SetVisibility(true);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->TextureTarget = RT;
		CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->FOVAngle = FOV;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

		CaptureComp->RegisterComponentWithWorld(World);
		CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

		bSuccess = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, OutputPath);

		// Cleanup.
		CaptureComp->TextureTarget = nullptr;
		CaptureComp->UnregisterComponent();
		PreviewScene->RemoveComponent(SkelMeshComp);
	}
	else if (AssetType.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
	{
		// Headless / nullrhi / commandlet contexts have no rendering path — bail
		// with the same -32603 convention claudedesign::capture_widget uses so
		// agents can pattern-match the error code.
		if (!FApp::CanEverRender())
		{
			return FMonolithActionResult::Error(
				TEXT("Cannot render widget: this app has no rendering path (server / nullrhi / commandlet)"),
				-32603);
		}

		// Load Widget Blueprint. UMG widget assets live in UWidgetBlueprint with
		// the runtime UClass on GeneratedClass.
		UWidgetBlueprint* WBP = FMonolithAssetUtils::LoadAssetByPath<UWidgetBlueprint>(AssetPath);
		if (!WBP)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath),
				-32602);
		}
		if (!WBP->GeneratedClass)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Widget Blueprint '%s' has no GeneratedClass (needs compile?)"), *AssetPath),
				-32603);
		}

		UClass* GenClass = WBP->GeneratedClass.Get();
		if (!GenClass || !GenClass->IsChildOf(UUserWidget::StaticClass()))
		{
			return FMonolithActionResult::Error(
				TEXT("Widget Blueprint GeneratedClass is not a UUserWidget subclass"),
				-32603);
		}

		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!EditorWorld)
		{
			return FMonolithActionResult::Error(
				TEXT("No editor world available to create widget in"), -32603);
		}

		UUserWidget* Instance = CreateWidget<UUserWidget>(EditorWorld, GenClass);
		if (!Instance)
		{
			return FMonolithActionResult::Error(
				TEXT("CreateWidget<UUserWidget> returned null"), -32603);
		}

		// Resolved-question #2: ship optional `scale` param defaulting to 1.0, clamp >= 0.01.
		double ScaleD = 1.0;
		Params->TryGetNumberField(TEXT("scale"), ScaleD);
		const float Scale = FMath::Max(0.01f, (float)ScaleD);

		const uint32 PhysicalW = FMath::Max(1u, (uint32)(ResX * Scale));
		const uint32 PhysicalH = FMath::Max(1u, (uint32)(ResY * Scale));

		const bool bUseGammaCorrection = true;
		const bool bIsLinearSpace = !bUseGammaCorrection;
		const EPixelFormat RequestedFormat = FSlateApplication::Get().GetRenderer()->GetSlateRecommendedColorFormat();

		UTextureRenderTarget2D* WidgetRT = NewObject<UTextureRenderTarget2D>();
		WidgetRT->ClearColor = FLinearColor::Transparent;
		WidgetRT->Filter = TF_Bilinear;
		WidgetRT->SRGB = bIsLinearSpace;
		WidgetRT->TargetGamma = 1;
		WidgetRT->InitCustomFormat(PhysicalW, PhysicalH, RequestedFormat, bIsLinearSpace);
		WidgetRT->UpdateResourceImmediate(/*bClearRenderTarget=*/true);

		// FWidgetRenderer derives from FDeferredCleanupInterface — must be deleted
		// via BeginCleanup, not raw `delete`. Mirrors sibling claudedesign pattern.
		FWidgetRenderer* WRenderer = new FWidgetRenderer(bUseGammaCorrection, /*bInClearTarget=*/true);
		WRenderer->SetIsPrepassNeeded(true);

		const TSharedRef<SWidget> SlateWidget = Instance->TakeWidget();

		// First draw triggers material handle creation + shader compilation. Without
		// the warmup + FinishAllCompilation, material-backed widget batches are
		// silently skipped (SlateRHIRenderingPolicy.cpp:1109).
		WRenderer->DrawWidget(WidgetRT, SlateWidget, Scale, FVector2D(ResX, ResY), 0.0f);
		FlushRenderingCommands();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}

		WRenderer->DrawWidget(WidgetRT, SlateWidget, Scale, FVector2D(ResX, ResY), 0.0f);
		FlushRenderingCommands();

		// Export — match the sibling pattern. ExportRenderTarget2DAsPNG is soft-
		// deprecated but functional in 5.7; mirror existing-pattern choice.
		const FString OutDir = FPaths::GetPath(OutputPath);
		if (!OutDir.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
		}

		bool bExportOk = false;
		{
			TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*OutputPath));
			if (Writer)
			{
				bExportOk = FImageUtils::ExportRenderTarget2DAsPNG(WidgetRT, *Writer);
				Writer->Close();
			}
		}

		BeginCleanup(WRenderer);

		bSuccess = bExportOk;

		// Physical (scale-adjusted) resolution dominates for the widget branch;
		// stash it back in ResX/ResY so the success payload below reflects what
		// was actually written.
		ResX = (int32)PhysicalW;
		ResY = (int32)PhysicalH;
	}
	else
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Unsupported asset_type: %s (supported: niagara, material, static_mesh, skeletal_mesh, widget)"),
				*AssetType));
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	if (!bSuccess)
	{
		return FMonolithActionResult::Error(TEXT("Capture failed — check log for details"));
	}

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), OutputPath);
	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ResX);
	ResObj->SetNumberField(TEXT("height"), ResY);
	Result->SetObjectField(TEXT("resolution"), ResObj);
	Result->SetNumberField(TEXT("seek_time"), SeekTime);
	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureSequenceFrames(
	const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	FString AssetType;
	Params->TryGetStringField(TEXT("asset_type"), AssetType);

	if (AssetPath.IsEmpty() || AssetType.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path and asset_type are required"));
	}

	// Parse timestamps
	const TArray<TSharedPtr<FJsonValue>>* TimestampArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("timestamps"), TimestampArray) || !TimestampArray)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is required"));
	}

	if (TimestampArray->Num() > 1000)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array exceeds maximum allowed size (1000)"));
	}

	TArray<float> Timestamps;
	for (const auto& Val : *TimestampArray)
	{
		Timestamps.Add((float)Val->AsNumber());
	}
	Timestamps.Sort();

	if (Timestamps.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("timestamps array is empty"));
	}

	// Parse optional params (same as capture_scene_preview)
	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ResArray = nullptr;
		if (Params->TryGetArrayField(TEXT("resolution"), ResArray) && ResArray)
		{
			if (ResArray->Num() >= 2)
			{
				double TempX = 0.0, TempY = 0.0;
				if ((*ResArray)[0]->TryGetNumber(TempX) && (*ResArray)[1]->TryGetNumber(TempY))
				{
					ResX = (int32)TempX;
					ResY = (int32)TempY;
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'resolution' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'resolution' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	// Same camera parsing as HandleCaptureScenePreview (with string fallback)
	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		TSharedPtr<FJsonObject> ParsedCamera;
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObj))
		{
			FString CameraStr;
			if (Params->TryGetStringField(TEXT("camera"), CameraStr))
			{
				if (!CameraStr.IsEmpty())
				{
					ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
					CameraObj = &ParsedCamera;
				}
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'camera' must be an object or JSON string"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
		if (CameraObj && (*CameraObj).IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
				if ((*CameraObj)->TryGetArrayField(TEXT("location"), Loc) && Loc)
				{
					if (Loc->Num() >= 3)
					{
						double TempX = 0.0, TempY = 0.0, TempZ = 0.0;
						if ((*Loc)[0]->TryGetNumber(TempX) && (*Loc)[1]->TryGetNumber(TempY) && (*Loc)[2]->TryGetNumber(TempZ))
						{
							CameraLocation = FVector(TempX, TempY, TempZ);
						}
						else
						{
							return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.location' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
						}
					}
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.location' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
				if ((*CameraObj)->TryGetArrayField(TEXT("rotation"), Rot) && Rot)
				{
					if (Rot->Num() >= 3)
					{
						double TempPitch = 0.0, TempYaw = 0.0, TempRoll = 0.0;
						if ((*Rot)[0]->TryGetNumber(TempPitch) && (*Rot)[1]->TryGetNumber(TempYaw) && (*Rot)[2]->TryGetNumber(TempRoll))
						{
							CameraRotation = FRotator(TempPitch, TempYaw, TempRoll);
						}
						else
						{
							return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.rotation' elements must be numbers"), FMonolithJsonUtils::ErrInvalidParams);
						}
					}
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.rotation' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				double TempFov = 0.0;
				if ((*CameraObj)->TryGetNumberField(TEXT("fov"), TempFov))
				{
					FOV = (float)TempFov;
				}
				else
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.fov' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
				}
			}
		}
	}

	FString OutputDir;
	if (Params->HasField(TEXT("output_dir")))
	{
		if (Params->TryGetStringField(TEXT("output_dir"), OutputDir))
		{
			if (FPaths::IsRelative(OutputDir))
			{
				OutputDir = FPaths::ProjectDir() / OutputDir;
			}
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'output_dir' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("%s_%s"), *Timestamp, *SafeName);
	}

	FString FilenamePrefix = TEXT("frame");
	if (Params->HasField(TEXT("filename_prefix")))
	{
		if (!Params->TryGetStringField(TEXT("filename_prefix"), FilenamePrefix))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'filename_prefix' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}

	// Currently only supports Niagara for multi-frame
	if (!AssetType.Equals(TEXT("niagara"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("capture_sequence_frames currently only supports asset_type: niagara"));
	}

	// Already on game thread (UE HTTP server dispatches via FTicker)
	check(IsInGameThread());

	UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(AssetPath);
	if (!System)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Failed to load: %s"), *AssetPath));
	}

	bool bPersistent = false;
	Params->TryGetBoolField(TEXT("persistent"), bPersistent);

	double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	if (bPersistent)
	{
		// PERSISTENT MODE: Create component ONCE, advance through time, capture at intervals.
		// Preserves ribbons, particle accumulation, and inter-frame state.
		FPreviewScene::ConstructionValues CVs;
		CVs.bDefaultLighting = false;
		CVs.LightBrightness = 0.0f;
		CVs.SkyBrightness = 0.0f;
		TSharedPtr<FAdvancedPreviewScene> PreviewScene =
			MakeShareable(new FAdvancedPreviewScene(CVs));
		PreviewScene->SetFloorVisibility(false);
		PreviewScene->SetEnvironmentVisibility(false);

		UNiagaraComponent* NiagaraComp = NewObject<UNiagaraComponent>(
			GetTransientPackage(), NAME_None, RF_Transient);
		NiagaraComp->CastShadow = false;
		NiagaraComp->bCastDynamicShadow = false;
		NiagaraComp->SetAllowScalability(false);
		NiagaraComp->SetAsset(System);
		NiagaraComp->SetForceSolo(true);
		NiagaraComp->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
		NiagaraComp->SetCanRenderWhileSeeking(true);
		NiagaraComp->SetMaxSimTime(0.0f);
		NiagaraComp->Activate(true);

		PreviewScene->AddComponent(NiagaraComp, NiagaraComp->GetRelativeTransform());

		UWorld* World = NiagaraComp->GetWorld();
		const float TickDelta = 1.0f / 30.0f;
		float CurrentTime = 0.0f;

		// Sort timestamps to ensure we advance monotonically
		TArray<float> SortedTimestamps = Timestamps;
		SortedTimestamps.Sort();

		for (int32 i = 0; i < SortedTimestamps.Num(); i++)
		{
			float TargetTime = SortedTimestamps[i];

			// Advance from current time to target time
			NiagaraComp->SetSeekDelta(TickDelta);
			NiagaraComp->SeekToDesiredAge(TargetTime);

			if (World)
			{
				World->TimeSeconds = TargetTime;
				World->DeltaTimeSeconds = TickDelta;
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
			}

			NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);

			// GPU flush for particle buffers
			if (World)
			{
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
			}
			FlushRenderingCommands();

			// Warm-up extra tick so GPU buffers are populated
			if (World)
			{
				World->Tick(ELevelTick::LEVELTICK_PauseTick, TickDelta);
				NiagaraComp->TickComponent(TickDelta, ELevelTick::LEVELTICK_All, nullptr);
				World->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(World))
				{
					WorldManager->FlushComputeAndDeferredQueues(true);
				}
				FlushRenderingCommands();
			}

			// Set up capture component and render
			UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
			RT->InitAutoFormat(ResX, ResY);
			RT->ClearColor = FLinearColor::Black;
			RT->UpdateResourceImmediate(true);

			USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
				GetTransientPackage(), NAME_None, RF_Transient);
			CaptureComp->TextureTarget = RT;
			CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
			CaptureComp->bCaptureEveryFrame = false;
			CaptureComp->bCaptureOnMovement = false;
			CaptureComp->bAlwaysPersistRenderingState = true;
			CaptureComp->FOVAngle = FOV;
			CaptureComp->SetRelativeLocation(CameraLocation);
			CaptureComp->SetRelativeRotation(CameraRotation);

			PreviewScene->AddComponent(CaptureComp, FTransform::Identity);

			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, TargetTime);

			bool bOk = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, FramePath);

			PreviewScene->RemoveComponent(CaptureComp);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), TargetTime);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));

			CurrentTime = TargetTime;
		}

		// Cleanup
		PreviewScene->RemoveComponent(NiagaraComp);
		NiagaraComp->DeactivateImmediate();
	}
	else
	{
		// PER-FRAME MODE: Use CaptureNiagaraFrame per frame — the proven working path
		// (DesiredAge + warm-up ticks + GPU flush). Reliable but recreates component each frame.
		for (int32 i = 0; i < Timestamps.Num(); i++)
		{
			float T = Timestamps[i];
			FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
				*FilenamePrefix, i, T);

			bool bOk = CaptureNiagaraFrame(System, T, CameraLocation, CameraRotation,
				FOV, ResX, ResY, FramePath);

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("timestamp"), T);
			FrameObj->SetStringField(TEXT("file"), FramePath);
			FrameObj->SetBoolField(TEXT("success"), bOk);
			FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}

	double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("persistent_mode"), bPersistent);
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}

// #15 capture_anim_frames — preview + capture a skeletal-animation asset to PNG frames.
// Supports UAnimSequence (single-node SetPosition), UBlendSpace (blend-param +
// SetPosition), and UAnimBlueprint (anim-blueprint mode + per-sample TickComponent).
// Reuses the SAME render path as the skeletal_mesh branch of capture_scene_preview:
// FAdvancedPreviewScene -> USceneCaptureComponent2D -> RenderAndSaveCapture.
FMonolithActionResult FMonolithEditorActions::HandleCaptureAnimFrames(
	const TSharedPtr<FJsonObject>& Params)
{
	check(IsInGameThread());

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!FApp::CanEverRender())
	{
		return FMonolithActionResult::Error(
			TEXT("Cannot capture anim frames: this app has no rendering path (server / nullrhi / commandlet)"),
			-32603);
	}

	// Resolve the asset. The path may be an AnimSequence, a BlendSpace, or an AnimBlueprint.
	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(AssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}

	UAnimSequence*  AnimSeq   = Cast<UAnimSequence>(Asset);
	UBlendSpace*    BlendSpc  = Cast<UBlendSpace>(Asset);
	UAnimBlueprint* AnimBP    = Cast<UAnimBlueprint>(Asset);
	UAnimationAsset* AnimAsset = Cast<UAnimationAsset>(Asset); // covers sequence + blendspace

	FString AssetKind;
	USkeleton* Skeleton = nullptr;
	if (AnimBP)        { AssetKind = TEXT("anim_blueprint"); Skeleton = AnimBP->TargetSkeleton; }
	else if (BlendSpc) { AssetKind = TEXT("blend_space");    Skeleton = BlendSpc->GetSkeleton(); }
	else if (AnimSeq)  { AssetKind = TEXT("anim_sequence");  Skeleton = AnimSeq->GetSkeleton(); }
	else if (AnimAsset){ AssetKind = TEXT("animation_asset");Skeleton = AnimAsset->GetSkeleton(); }
	else
	{
		return FMonolithActionResult::Error(
			TEXT("asset_path must be a UAnimSequence, UBlendSpace, or UAnimBlueprint"));
	}

	// Resolve the preview mesh: explicit skeletal_mesh param, else the skeleton's preview mesh.
	USkeletalMesh* PreviewMesh = nullptr;
	FString MeshParam;
	if (Params->TryGetStringField(TEXT("skeletal_mesh"), MeshParam) && !MeshParam.IsEmpty())
	{
		PreviewMesh = FMonolithAssetUtils::LoadAssetByPath<USkeletalMesh>(MeshParam);
		if (!PreviewMesh)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Failed to load skeletal_mesh: %s"), *MeshParam));
		}
	}
	else if (Skeleton)
	{
		PreviewMesh = Skeleton->GetPreviewMesh(/*bFindIfNotSet=*/true);
	}
	if (!PreviewMesh)
	{
		return FMonolithActionResult::Error(
			TEXT("No preview mesh available — pass skeletal_mesh, or set a preview mesh on the asset's skeleton."));
	}

	// --- Resolve time samples ---
	TArray<float> TimeSamples;
	const TArray<TSharedPtr<FJsonValue>>* SamplesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("time_samples"), SamplesArr) && SamplesArr && SamplesArr->Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& V : *SamplesArr)
		{
			if (V.IsValid()) { TimeSamples.Add((float)V->AsNumber()); }
		}
	}
	if (TimeSamples.Num() == 0)
	{
		int32 Count = 8;
		if (Params->HasField(TEXT("count")))
		{
			double CountVal = 0.0;
			if (!Params->TryGetNumberField(TEXT("count"), CountVal))
			{
				return FMonolithActionResult::Error(TEXT("count must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			Count = FMath::Max(1, (int32)CountVal);
		}
		float DurationS = 1.0f;
		if (Params->HasField(TEXT("duration")))
		{
			double DurationVal = 0.0;
			if (!Params->TryGetNumberField(TEXT("duration"), DurationVal))
			{
				return FMonolithActionResult::Error(TEXT("duration must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			DurationS = (float)DurationVal;
		}
		else if (AnimSeq) { DurationS = AnimSeq->GetPlayLength(); }
		DurationS = FMath::Max(0.0f, DurationS);
		for (int32 i = 0; i < Count; ++i)
		{
			const float T = (Count == 1) ? 0.0f : (DurationS * (float)i / (float)(Count - 1));
			TimeSamples.Add(T);
		}
	}
	TimeSamples.Sort();

	// --- Blend-space input (BlendSpace only) ---
	FVector BlendInput(0.0f, 0.0f, 0.0f);
	const TArray<TSharedPtr<FJsonValue>>* BlendArr = nullptr;
	if (Params->TryGetArrayField(TEXT("blend_params"), BlendArr) && BlendArr)
	{
		if (BlendArr->Num() >= 1) { BlendInput.X = (float)(*BlendArr)[0]->AsNumber(); }
		if (BlendArr->Num() >= 2) { BlendInput.Y = (float)(*BlendArr)[1]->AsNumber(); }
	}

	// --- Resolution / camera (same parsing convention as capture_sequence_frames) ---
	int32 ResX = 512, ResY = 512;
	if (Params->HasField(TEXT("resolution")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ResArrayPtr = nullptr;
		if (!Params->TryGetArrayField(TEXT("resolution"), ResArrayPtr) || !ResArrayPtr)
		{
			return FMonolithActionResult::Error(TEXT("resolution must be an array"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (ResArrayPtr->Num() >= 2) { ResX = (int32)(*ResArrayPtr)[0]->AsNumber(); ResY = (int32)(*ResArrayPtr)[1]->AsNumber(); }
	}
	FVector CameraLocation(200.0f, 0.0f, 100.0f);
	FRotator CameraRotation(0.0f, 180.0f, 0.0f);
	float FOV = 60.0f;
	if (Params->HasField(TEXT("camera")))
	{
		const TSharedPtr<FJsonObject>* CameraObj = nullptr;
		if (Params->TryGetObjectField(TEXT("camera"), CameraObj) && CameraObj && CameraObj->IsValid())
		{
			if ((*CameraObj)->HasField(TEXT("location")))
			{
				const TArray<TSharedPtr<FJsonValue>>* LocPtr = nullptr;
				if (!(*CameraObj)->TryGetArrayField(TEXT("location"), LocPtr) || !LocPtr)
				{
					return FMonolithActionResult::Error(TEXT("camera.location must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
				if (LocPtr->Num() >= 3) CameraLocation = FVector((*LocPtr)[0]->AsNumber(), (*LocPtr)[1]->AsNumber(), (*LocPtr)[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("rotation")))
			{
				const TArray<TSharedPtr<FJsonValue>>* RotPtr = nullptr;
				if (!(*CameraObj)->TryGetArrayField(TEXT("rotation"), RotPtr) || !RotPtr)
				{
					return FMonolithActionResult::Error(TEXT("camera.rotation must be an array"), FMonolithJsonUtils::ErrInvalidParams);
				}
				if (RotPtr->Num() >= 3) CameraRotation = FRotator((*RotPtr)[0]->AsNumber(), (*RotPtr)[1]->AsNumber(), (*RotPtr)[2]->AsNumber());
			}
			if ((*CameraObj)->HasField(TEXT("fov")))
			{
				double FOVVal = 0.0;
				if (!(*CameraObj)->TryGetNumberField(TEXT("fov"), FOVVal))
				{
					return FMonolithActionResult::Error(TEXT("camera.fov must be a number"), FMonolithJsonUtils::ErrInvalidParams);
				}
				FOV = (float)FOVVal;
			}
		}
	}

	// --- Output dir / prefix ---
	FString OutputDir;
	if (!Params->TryGetStringField(TEXT("output_dir"), OutputDir) || OutputDir.IsEmpty())
	{
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith/AnimFrames") /
			FString::Printf(TEXT("%s_%s"), *Stamp, *SafeName);
	}
	// Resolve virtual /Game paths to on-disk dirs + create the directory. A virtual path
	// that can't be converted is a hard error — never report success with no files written.
	FString ResolvedError;
	if (!ResolveCaptureOutputDir(OutputDir, OutputDir, ResolvedError))
	{
		return FMonolithActionResult::Error(ResolvedError);
	}
	FString FilenamePrefix = TEXT("frame");
	Params->TryGetStringField(TEXT("filename_prefix"), FilenamePrefix);

	// --- Build the preview scene + debug skel mesh component (reuses the proven
	// skeletal_mesh capture path; UDebugSkelMeshComponent is a USkeletalMeshComponent). ---
	TSharedPtr<FAdvancedPreviewScene> PreviewScene =
		MakeShareable(new FAdvancedPreviewScene(FPreviewScene::ConstructionValues()));
	PreviewScene->SetFloorVisibility(false);

	UDebugSkelMeshComponent* SkelComp = NewObject<UDebugSkelMeshComponent>(
		GetTransientPackage(), NAME_None, RF_Transient);
	SkelComp->SetSkeletalMesh(PreviewMesh);

	// Configure the animation source per asset kind.
	if (AnimBP)
	{
		UClass* AnimClass = AnimBP->GeneratedClass;
		if (!AnimClass)
		{
			return FMonolithActionResult::Error(
				TEXT("AnimBlueprint has no GeneratedClass (compile it first)."));
		}
		SkelComp->SetAnimationMode(EAnimationMode::AnimationBlueprint);
		SkelComp->SetAnimInstanceClass(AnimClass);
	}
	else
	{
		// Sequence + BlendSpace: single-node mode posed via SetPosition each sample.
		SkelComp->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		SkelComp->SetAnimation(AnimAsset);
		if (BlendSpc)
		{
			if (UAnimSingleNodeInstance* SingleNode = SkelComp->GetSingleNodeInstance())
			{
				SingleNode->SetBlendSpacePosition(BlendInput);
			}
		}
	}

	PreviewScene->AddComponent(SkelComp, SkelComp->GetRelativeTransform());
	UWorld* World = PreviewScene->GetWorld();

	const double StartTime = FPlatformTime::Seconds();
	TArray<TSharedPtr<FJsonValue>> FrameResults;

	for (int32 i = 0; i < TimeSamples.Num(); ++i)
	{
		const float T = TimeSamples[i];

		if (AnimBP)
		{
			// ABP: advance the graph by ticking the component up to the target time. We
			// re-evaluate from the current pose each sample (monotonic since sorted).
			const float Step = (i == 0) ? T : (T - TimeSamples[i - 1]);
			if (World) { World->Tick(ELevelTick::LEVELTICK_PauseTick, FMath::Max(0.0f, Step)); }
			SkelComp->TickComponent(FMath::Max(0.0f, Step), ELevelTick::LEVELTICK_All, nullptr);
		}
		else
		{
			// Single-node: SetPosition forces evaluation at the absolute target time.
			if (BlendSpc)
			{
				if (UAnimSingleNodeInstance* SingleNode = SkelComp->GetSingleNodeInstance())
				{
					SingleNode->SetBlendSpacePosition(BlendInput);
				}
			}
			SkelComp->SetPosition(T, /*bFireNotifies=*/false);
			if (World) { World->Tick(ELevelTick::LEVELTICK_PauseTick, 0.0f); }
			SkelComp->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
		}

		if (World) { World->SendAllEndOfFrameUpdates(); }

		// --- Shared render path: RT + scene capture + RenderAndSaveCapture ---
		UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		RT->InitAutoFormat(ResX, ResY);
		RT->ClearColor = FLinearColor(0.18f, 0.18f, 0.18f);
		RT->UpdateResourceImmediate(true);

		USceneCaptureComponent2D* CaptureComp = NewObject<USceneCaptureComponent2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		CaptureComp->bTickInEditor = false;
		CaptureComp->SetComponentTickEnabled(false);
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->TextureTarget = RT;
		CaptureComp->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		CaptureComp->ProjectionType = ECameraProjectionMode::Perspective;
		CaptureComp->FOVAngle = FOV;
		CaptureComp->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;
		CaptureComp->RegisterComponentWithWorld(World);
		CaptureComp->SetWorldLocationAndRotation(CameraLocation, CameraRotation);

		const FString FramePath = OutputDir / FString::Printf(TEXT("%s_%03d_t%.2f.png"),
			*FilenamePrefix, i, T);
		const bool bOk = RenderAndSaveCapture(CaptureComp, RT, ResX, ResY, FramePath);

		CaptureComp->TextureTarget = nullptr;
		CaptureComp->UnregisterComponent();

		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("time"), T);
		FrameObj->SetStringField(TEXT("file"), FramePath);
		FrameObj->SetBoolField(TEXT("success"), bOk);
		FrameResults.Add(MakeShared<FJsonValueObject>(FrameObj));
	}

	// Readback of the live anim node (cheap identity echo).
	FString LiveAnimClass;
	if (UClass* LiveClass = SkelComp->GetAnimClass()) { LiveAnimClass = LiveClass->GetPathName(); }

	PreviewScene->RemoveComponent(SkelComp);

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_kind"), AssetKind);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : FString());
	Result->SetStringField(TEXT("preview_mesh"), PreviewMesh->GetPathName());
	if (!LiveAnimClass.IsEmpty()) { Result->SetStringField(TEXT("live_anim_class"), LiveAnimClass); }
	Result->SetStringField(TEXT("output_dir"), OutputDir);            // resolved absolute on-disk dir
	Result->SetStringField(TEXT("resolved_output_dir"), OutputDir);   // explicit alias for callers
	Result->SetArrayField(TEXT("frames"), FrameResults);
	Result->SetNumberField(TEXT("total_capture_time_ms"), ElapsedMs);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleImportTexture(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("source_path is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	FString Destination;
	if (!Params->TryGetStringField(TEXT("destination"), Destination) || Destination.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("destination is required and must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}

	// Verify source file exists
	if (!FPaths::FileExists(SourcePath))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Source file not found: %s"), *SourcePath));
	}

	// Parse + type-validate optional settings up front. ImportAssetsAutomated
	// runs with bReplaceExisting and can create or replace the destination
	// asset, so malformed settings must fail before the import mutates anything.
	TSharedPtr<FJsonObject> ParsedSettings;
	if (Params->HasField(TEXT("settings")))
	{
		const TSharedPtr<FJsonObject>* SettingsObj;
		// Handle string-serialized params (Claude Code quirk)
		if (Params->TryGetObjectField(TEXT("settings"), SettingsObj))
		{
			ParsedSettings = *SettingsObj;
		}
		else
		{
			FString SettingsStr;
			if (!Params->TryGetStringField(TEXT("settings"), SettingsStr))
			{
				return FMonolithActionResult::Error(TEXT("settings must be an object or a JSON string"), FMonolithJsonUtils::ErrInvalidParams);
			}
			if (!SettingsStr.IsEmpty())
			{
				ParsedSettings = FMonolithJsonUtils::Parse(SettingsStr);
			}
		}

		if (ParsedSettings.IsValid())
		{
			FString SettingsStrValue;
			bool bSettingsBoolValue = false;
			double SettingsNumberValue = 0.0;
			if (ParsedSettings->HasField(TEXT("compression")) && !ParsedSettings->TryGetStringField(TEXT("compression"), SettingsStrValue))
			{
				return FMonolithActionResult::Error(TEXT("settings.compression must be a string"), FMonolithJsonUtils::ErrInvalidParams);
			}
			if (ParsedSettings->HasField(TEXT("srgb")) && !ParsedSettings->TryGetBoolField(TEXT("srgb"), bSettingsBoolValue))
			{
				return FMonolithActionResult::Error(TEXT("settings.srgb must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
			}
			if (ParsedSettings->HasField(TEXT("tiling")) && !ParsedSettings->TryGetBoolField(TEXT("tiling"), bSettingsBoolValue))
			{
				return FMonolithActionResult::Error(TEXT("settings.tiling must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
			}
			if (ParsedSettings->HasField(TEXT("max_size")) && !ParsedSettings->TryGetNumberField(TEXT("max_size"), SettingsNumberValue))
			{
				return FMonolithActionResult::Error(TEXT("settings.max_size must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
			if (ParsedSettings->HasField(TEXT("lod_group")) && !ParsedSettings->TryGetStringField(TEXT("lod_group"), SettingsStrValue))
			{
				return FMonolithActionResult::Error(TEXT("settings.lod_group must be a string"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	// Import using AssetTools
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourcePath);
	ImportData->DestinationPath = FPackageName::GetLongPackagePath(Destination);
	ImportData->bReplaceExisting = true;

	FAssetToolsModule& AssetToolsModule =
		FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (ImportedAssets.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Import failed — no assets imported"));
	}

	UTexture2D* Texture = Cast<UTexture2D>(ImportedAssets[0]);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Imported asset is not a Texture2D"));
	}

	// Apply optional settings (parsed + type-validated before the import ran)
	{
		if (ParsedSettings.IsValid())
		{
			// Compression
			if (ParsedSettings->HasField(TEXT("compression")))
			{
				FString Comp;
				if (!ParsedSettings->TryGetStringField(TEXT("compression"), Comp))
				{
					return FMonolithActionResult::Error(TEXT("settings.compression must be a string"), FMonolithJsonUtils::ErrInvalidParams);
				}
				if (Comp == TEXT("TC_Normalmap")) Texture->CompressionSettings = TC_Normalmap;
				else if (Comp == TEXT("TC_Masks")) Texture->CompressionSettings = TC_Masks;
				else if (Comp == TEXT("TC_HDR")) Texture->CompressionSettings = TC_HDR;
				else if (Comp == TEXT("TC_VectorDisplacementmap")) Texture->CompressionSettings = TC_VectorDisplacementmap;
				else Texture->CompressionSettings = TC_Default;
			}

			// sRGB
			if (ParsedSettings->HasField(TEXT("srgb")))
			{
				bool bSrgb = false;
				if (!ParsedSettings->TryGetBoolField(TEXT("srgb"), bSrgb))
				{
					return FMonolithActionResult::Error(TEXT("settings.srgb must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
				}
				Texture->SRGB = bSrgb;
			}

			// Tiling
			if (ParsedSettings->HasField(TEXT("tiling")))
			{
				bool bTiling = false;
				if (!ParsedSettings->TryGetBoolField(TEXT("tiling"), bTiling))
				{
					return FMonolithActionResult::Error(TEXT("settings.tiling must be a boolean"), FMonolithJsonUtils::ErrInvalidParams);
				}
				if (bTiling)
				{
					Texture->AddressX = TA_Wrap;
					Texture->AddressY = TA_Wrap;
				}
			}

			// Max size
			if (ParsedSettings->HasField(TEXT("max_size")))
			{
				double MaxSizeDbl = 0;
				if (!ParsedSettings->TryGetNumberField(TEXT("max_size"), MaxSizeDbl))
				{
					return FMonolithActionResult::Error(TEXT("settings.max_size must be a number"), FMonolithJsonUtils::ErrInvalidParams);
				}
				int32 MaxSize = (int32)MaxSizeDbl;
				if (MaxSize > 0)
				{
					Texture->MaxTextureSize = MaxSize;
				}
			}

			// LOD group
			if (ParsedSettings->HasField(TEXT("lod_group")))
			{
				FString LODGroup;
				if (!ParsedSettings->TryGetStringField(TEXT("lod_group"), LODGroup))
				{
					return FMonolithActionResult::Error(TEXT("lod_group must be a string"), FMonolithJsonUtils::ErrInvalidParams);
				}
				if (LODGroup == TEXT("TEXTUREGROUP_WorldNormalMap")) Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
				else if (LODGroup == TEXT("TEXTUREGROUP_Effects")) Texture->LODGroup = TEXTUREGROUP_Effects;
				else if (LODGroup == TEXT("TEXTUREGROUP_EffectsNotFiltered")) Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
				// Default: TEXTUREGROUP_World (already default)
			}
		}
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save the package
	UPackage* Package = Texture->GetOutermost();
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	// Return result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("asset_path"), Destination);
	Result->SetNumberField(TEXT("size_x"), Texture->GetSizeX());
	Result->SetNumberField(TEXT("size_y"), Texture->GetSizeY());
	Result->SetStringField(TEXT("format"), GPixelFormats[Texture->GetPixelFormat()].Name);

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleDeleteAssets(
	const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("asset_paths array is required and must not be empty"));
	}

	TArray<FString> AssetPaths;
	for (const auto& Val : *AssetPathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			AssetPaths.Add(Path);
		}
	}

	if (AssetPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid paths in asset_paths"));
	}

	TArray<FString> AllowedPrefixes;
	const TArray<TSharedPtr<FJsonValue>>* PrefixArray = nullptr;
	if (Params->TryGetArrayField(TEXT("allowed_prefixes"), PrefixArray) && PrefixArray)
	{
		for (const auto& PVal : *PrefixArray)
		{
			FString Prefix;
			if (PVal->TryGetString(Prefix) && !Prefix.IsEmpty())
			{
				AllowedPrefixes.Add(Prefix);
			}
		}
	}

	if (AllowedPrefixes.Num() > 0)
	{
		for (const FString& Path : AssetPaths)
		{
			bool bAllowed = false;
			for (const FString& Prefix : AllowedPrefixes)
			{
				if (Path.StartsWith(Prefix))
				{
					bAllowed = true;
					break;
				}
			}
			if (!bAllowed)
			{
				return FMonolithActionResult::Error(FString::Printf(
					TEXT("Refusing to delete '%s' — not under any allowed prefix. Allowed: %s"),
					*Path, *FString::Join(AllowedPrefixes, TEXT(", "))));
			}
		}
	}

	bool bForce = false;
	Params->TryGetBoolField(TEXT("force"), bForce);

	TArray<UObject*> ObjectsToDelete;
	TArray<FString> NotFound;

	for (const FString& Path : AssetPaths)
	{
		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(Path);
		if (Asset)
		{
			ObjectsToDelete.Add(Asset);
		}
		else
		{
			NotFound.Add(Path);
		}
	}

	for (UObject* Asset : ObjectsToDelete)
	{
		if (UPackage* Pkg = Asset->GetOutermost())
		{
			Pkg->SetDirtyFlag(false);
		}
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AES = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AES->CloseAllEditorsForAsset(Asset);
			}
		}
	}

	TArray<FString> AttemptedPaths;
	AttemptedPaths.Reserve(ObjectsToDelete.Num());
	for (const UObject* Obj : ObjectsToDelete)
	{
		AttemptedPaths.Add(Obj->GetPathName());
	}

	int32 NumDeleted = 0;
	if (ObjectsToDelete.Num() > 0)
	{
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		NumDeleted = bForce
			? ObjectTools::ForceDeleteObjects(ObjectsToDelete, /*ShowConfirmation=*/false)
			: ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), NumDeleted == ObjectsToDelete.Num() && NotFound.Num() == 0);
	Result->SetNumberField(TEXT("deleted"), NumDeleted);
	Result->SetNumberField(TEXT("requested"), AssetPaths.Num());
	Result->SetNumberField(TEXT("found"), ObjectsToDelete.Num());

	if (NumDeleted < ObjectsToDelete.Num())
	{
		TArray<TSharedPtr<FJsonValue>> FailedArr;
		for (const FString& P : AttemptedPaths)
		{
			FailedArr.Add(MakeShared<FJsonValueString>(P));
		}
		Result->SetArrayField(TEXT("failed_to_delete"), FailedArr);
	}

	if (NotFound.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> NotFoundArr;
		for (const FString& P : NotFound)
		{
			NotFoundArr.Add(MakeShared<FJsonValueString>(P));
		}
		Result->SetArrayField(TEXT("not_found"), NotFoundArr);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleStitchFlipbook(
	const TSharedPtr<FJsonObject>& Params)
{
	// --- Extract required params ---
	FString DestPath;
	Params->TryGetStringField(TEXT("dest_path"), DestPath);
	if (DestPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("dest_path is required"));
	}

	if (const FString ValidationError = MonolithCore::ValidatePackagePath(DestPath); !ValidationError.IsEmpty())
	{
		return FMonolithActionResult::Error(ValidationError);
	}

	// Parse frame_paths array
	const TArray<TSharedPtr<FJsonValue>>* FramePathsArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("frame_paths"), FramePathsArray) || !FramePathsArray || FramePathsArray->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("frame_paths array is required and must not be empty"));
	}

	TArray<FString> FramePaths;
	for (const auto& Val : *FramePathsArray)
	{
		FString Path;
		if (Val->TryGetString(Path) && !Path.IsEmpty())
		{
			FramePaths.Add(Path);
		}
	}

	if (FramePaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("No valid file paths in frame_paths"));
	}

	// Parse grid [cols, rows]
	const TArray<TSharedPtr<FJsonValue>>* GridArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("grid"), GridArray) || !GridArray || GridArray->Num() != 2)
	{
		return FMonolithActionResult::Error(TEXT("grid must be an array of [columns, rows]"));
	}
	int32 GridCols = static_cast<int32>((*GridArray)[0]->AsNumber());
	int32 GridRows = static_cast<int32>((*GridArray)[1]->AsNumber());

	if (GridCols <= 0 || GridRows <= 0)
	{
		return FMonolithActionResult::Error(TEXT("grid columns and rows must be positive"));
	}

	int32 ExpectedFrames = GridCols * GridRows;
	if (FramePaths.Num() != ExpectedFrames)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("frame_paths has %d entries but grid %dx%d expects %d"),
			FramePaths.Num(), GridCols, GridRows, ExpectedFrames));
	}

	// Optional params
	bool bSRGB = true;
	Params->TryGetBoolField(TEXT("srgb"), bSRGB);
	bool bNoMipmaps = true;
	Params->TryGetBoolField(TEXT("no_mipmaps"), bNoMipmaps);
	bool bDeleteSources = true;
	Params->TryGetBoolField(TEXT("delete_sources"), bDeleteSources);

	FString LODGroupStr = TEXT("TEXTUREGROUP_Effects");
	Params->TryGetStringField(TEXT("lod_group"), LODGroupStr);

	// --- Load all frame images ---
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	int32 FrameWidth = 0;
	int32 FrameHeight = 0;
	TArray<TArray<FColor>> FramePixels;
	FramePixels.SetNum(FramePaths.Num());

	for (int32 i = 0; i < FramePaths.Num(); i++)
	{
		const FString& FilePath = FramePaths[i];

		if (!FPaths::FileExists(FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame file not found: %s"), *FilePath));
		}

		TArray<uint8> FileData;
		if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to read frame file: %s"), *FilePath));
		}

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(FileData.GetData(), FileData.Num()))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decode PNG: %s"), *FilePath));
		}

		int32 W = ImageWrapper->GetWidth();
		int32 H = ImageWrapper->GetHeight();

		// Validate all frames are same size
		if (i == 0)
		{
			FrameWidth = W;
			FrameHeight = H;
		}
		else if (W != FrameWidth || H != FrameHeight)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Frame %d (%s) is %dx%d but frame 0 is %dx%d — all frames must be the same size"),
				i, *FilePath, W, H, FrameWidth, FrameHeight));
		}

		TArray<uint8> RawData;
		if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to decompress frame %d: %s"), i, *FilePath));
		}

		// Convert raw bytes to FColor array
		FramePixels[i].SetNum(W * H);
		FMemory::Memcpy(FramePixels[i].GetData(), RawData.GetData(), W * H * sizeof(FColor));
	}

	// --- Compose atlas ---
	int32 AtlasWidth = FrameWidth * GridCols;
	int32 AtlasHeight = FrameHeight * GridRows;
	TArray<FColor> AtlasPixels;
	AtlasPixels.SetNumZeroed(AtlasWidth * AtlasHeight);

	for (int32 FrameIdx = 0; FrameIdx < FramePaths.Num(); FrameIdx++)
	{
		int32 Col = FrameIdx % GridCols;
		int32 Row = FrameIdx / GridCols;
		int32 OffsetX = Col * FrameWidth;
		int32 OffsetY = Row * FrameHeight;

		const TArray<FColor>& Src = FramePixels[FrameIdx];
		for (int32 Y = 0; Y < FrameHeight; Y++)
		{
			for (int32 X = 0; X < FrameWidth; X++)
			{
				int32 SrcIdx = Y * FrameWidth + X;
				int32 DstIdx = (OffsetY + Y) * AtlasWidth + (OffsetX + X);
				AtlasPixels[DstIdx] = Src[SrcIdx];
			}
		}
	}

	// --- Create UTexture2D ---
	FString PackagePath = FPackageName::GetLongPackagePath(DestPath);
	FString AssetName = FPackageName::GetLongPackageAssetName(DestPath);

	// Ensure unique package name
	FString PackageName = PackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to create package: %s"), *PackageName));
	}
	Package->FullyLoad();

	UTexture2D* Texture = NewObject<UTexture2D>(Package, *AssetName, RF_Public | RF_Standalone);
	if (!Texture)
	{
		return FMonolithActionResult::Error(TEXT("Failed to create UTexture2D"));
	}

	// Configure platform data
	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = AtlasWidth;
	PlatformData->SizeY = AtlasHeight;
	PlatformData->PixelFormat = PF_B8G8R8A8;
	PlatformData->SetNumSlices(1);

	FTexture2DMipMap* Mip = new FTexture2DMipMap();
	Mip->SizeX = AtlasWidth;
	Mip->SizeY = AtlasHeight;
	PlatformData->Mips.Add(Mip);

	// Copy pixel data into mip
	Mip->BulkData.Lock(LOCK_READ_WRITE);
	void* MipData = Mip->BulkData.Realloc(AtlasWidth * AtlasHeight * sizeof(FColor));
	FMemory::Memcpy(MipData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
	Mip->BulkData.Unlock();

	Texture->SetPlatformData(PlatformData);

	// Apply texture settings
	Texture->Source.Init(AtlasWidth, AtlasHeight, 1, 1, TSF_BGRA8, nullptr);
	{
		uint8* SourceData = Texture->Source.LockMip(0);
		FMemory::Memcpy(SourceData, AtlasPixels.GetData(), AtlasWidth * AtlasHeight * sizeof(FColor));
		Texture->Source.UnlockMip(0);
	}

	Texture->SRGB = bSRGB;
	Texture->CompressionSettings = TC_Default;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;

	if (bNoMipmaps)
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
	}

	// LOD group
	if (LODGroupStr == TEXT("TEXTUREGROUP_Effects"))
	{
		Texture->LODGroup = TEXTUREGROUP_Effects;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_EffectsNotFiltered"))
	{
		Texture->LODGroup = TEXTUREGROUP_EffectsNotFiltered;
	}
	else if (LODGroupStr == TEXT("TEXTUREGROUP_World"))
	{
		Texture->LODGroup = TEXTUREGROUP_World;
	}

	Texture->UpdateResource();
	Texture->PostEditChange();
	Texture->MarkPackageDirty();

	// Save
	FString PackageFilename = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	bool bSaved = UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs);

	if (!bSaved)
	{
		return FMonolithActionResult::Error(TEXT("Failed to save flipbook texture package"));
	}

	// --- Delete source files if requested ---
	int32 DeletedCount = 0;
	if (bDeleteSources)
	{
		for (const FString& FilePath : FramePaths)
		{
			if (IFileManager::Get().Delete(*FilePath))
			{
				DeletedCount++;
			}
		}
	}

	// --- Return result ---
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("texture_path"), DestPath);

	TArray<TSharedPtr<FJsonValue>> ResArray;
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasWidth));
	ResArray.Add(MakeShared<FJsonValueNumber>(AtlasHeight));
	Result->SetArrayField(TEXT("resolution"), ResArray);

	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("frame_width"), FrameWidth);
	Result->SetNumberField(TEXT("frame_height"), FrameHeight);

	TArray<TSharedPtr<FJsonValue>> GridResult;
	GridResult.Add(MakeShared<FJsonValueNumber>(GridCols));
	GridResult.Add(MakeShared<FJsonValueNumber>(GridRows));
	Result->SetArrayField(TEXT("grid"), GridResult);

	if (bDeleteSources)
	{
		Result->SetNumberField(TEXT("sources_deleted"), DeletedCount);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleGetViewportInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	// Get the active level editor viewport
	FLevelEditorViewportClient* ViewportClient = nullptr;
	if (GEditor && GEditor->GetLevelViewportClients().Num() > 0)
	{
		ViewportClient = GEditor->GetLevelViewportClients()[0];
	}

	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(TEXT("No active viewport found"));
	}

	FVector CamLocation = ViewportClient->GetViewLocation();
	FRotator CamRotation = ViewportClient->GetViewRotation();
	float FOV = ViewportClient->ViewFOV;

	FIntPoint ViewportSize = ViewportClient->Viewport->GetSizeXY();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("active_viewport"), 0);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), ViewportSize.X);
	ResObj->SetNumberField(TEXT("height"), ViewportSize.Y);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.X));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Y));
	LocArr.Add(MakeShared<FJsonValueNumber>(CamLocation.Z));
	Result->SetArrayField(TEXT("camera_location"), LocArr);

	TArray<TSharedPtr<FJsonValue>> RotArr;
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Pitch));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Yaw));
	RotArr.Add(MakeShared<FJsonValueNumber>(CamRotation.Roll));
	Result->SetArrayField(TEXT("camera_rotation"), RotArr);

	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());

	return FMonolithActionResult::Success(Result);
}

namespace
{
	TArray<TSharedPtr<FJsonValue>> EditorVectorToJson(const FVector& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Value.X));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Z));
		return Arr;
	}

	TArray<TSharedPtr<FJsonValue>> EditorRotatorToJson(const FRotator& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Pitch));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Yaw));
		Arr.Add(MakeShared<FJsonValueNumber>(Value.Roll));
		return Arr;
	}

	bool ReadVectorArray(const TSharedPtr<FJsonObject>& Object, const FString& Field, FVector& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Object->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() != 3)
		{
			return false;
		}
		OutValue = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	bool ReadRotatorArray(const TSharedPtr<FJsonObject>& Object, const FString& Field, FRotator& OutValue)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Object->TryGetArrayField(Field, Arr) || !Arr || Arr->Num() != 3)
		{
			return false;
		}
		OutValue = FRotator((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	FLevelEditorViewportClient* GetLevelViewportClientByIndex(int32 ViewportIndex, FString& OutError)
	{
		if (!GEditor)
		{
			OutError = TEXT("GEditor is not available");
			return nullptr;
		}
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		if (!Clients.IsValidIndex(ViewportIndex) || !Clients[ViewportIndex])
		{
			OutError = FString::Printf(TEXT("Level viewport index %d is not available; call editor.list_open_viewports first"), ViewportIndex);
			return nullptr;
		}
		if (!Clients[ViewportIndex]->Viewport)
		{
			OutError = FString::Printf(TEXT("Level viewport index %d has no FViewport"), ViewportIndex);
			return nullptr;
		}
		return Clients[ViewportIndex];
	}

	TSharedPtr<FJsonObject> MakeViewportRow(FLevelEditorViewportClient* Client, int32 Index)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Index);
		Row->SetStringField(TEXT("source"), TEXT("level_editor"));
		if (!Client || !Client->Viewport)
		{
			Row->SetBoolField(TEXT("available"), false);
			return Row;
		}

		const FIntPoint Size = Client->Viewport->GetSizeXY();
		Row->SetBoolField(TEXT("available"), Size.X > 0 && Size.Y > 0);
		Row->SetNumberField(TEXT("width"), Size.X);
		Row->SetNumberField(TEXT("height"), Size.Y);
		Row->SetArrayField(TEXT("camera_location"), EditorVectorToJson(Client->GetViewLocation()));
		Row->SetArrayField(TEXT("camera_rotation"), EditorRotatorToJson(Client->GetViewRotation()));
		Row->SetNumberField(TEXT("fov"), Client->ViewFOV);
		Row->SetBoolField(TEXT("realtime"), Client->IsRealtime());
		return Row;
	}

	FMonolithActionResult MakeViewportCaptureUnavailable(const FString& Action, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("action"), Action);
		Result->SetStringField(TEXT("status"), TEXT("unavailable"));
		Result->SetBoolField(TEXT("fallback_used"), false);
		Result->SetStringField(TEXT("reason"), Reason);
		return FMonolithActionResult::Success(Result);
	}

	FString MakeCaptureSafeFileName(const FString& Value)
	{
		FString Safe = FPaths::MakeValidFileName(Value);
		return Safe.IsEmpty() ? TEXT("Asset") : Safe;
	}

	bool ReadThumbnailSize(const TSharedPtr<FJsonObject>& Params, int32& OutSize, FString& OutError)
	{
		OutSize = 256;
		if (!Params.IsValid() || !Params->HasField(TEXT("thumbnail_size")))
		{
			return true;
		}

		double SizeNumber = 0.0;
		if (!Params->TryGetNumberField(TEXT("thumbnail_size"), SizeNumber)
			|| SizeNumber < 16.0
			|| SizeNumber > 2048.0
			|| static_cast<double>(static_cast<int64>(SizeNumber)) != SizeNumber)
		{
			OutError = TEXT("thumbnail_size must be an integer between 16 and 2048.");
			return false;
		}

		OutSize = static_cast<int32>(SizeNumber);
		return true;
	}

	FString ResolveCaptureOutputPath(
		const TSharedPtr<FJsonObject>& Params,
		const FString& Prefix,
		const FString& AssetName)
	{
		FString OutputPath;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("output_path"), OutputPath);
		}

		if (OutputPath.IsEmpty())
		{
			OutputPath = FPaths::ProjectSavedDir() / TEXT("Screenshots/Monolith/")
				+ Prefix
				+ TEXT("_")
				+ MakeCaptureSafeFileName(AssetName)
				+ TEXT("_")
				+ FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"))
				+ TEXT(".png");
		}
		else if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputPath);
		}

		return OutputPath;
	}

	bool IsSupportedCaptureImageExtension(const FString& Extension)
	{
		return Extension == TEXT("png")
			|| Extension == TEXT("jpg")
			|| Extension == TEXT("jpeg")
			|| Extension == TEXT("bmp")
			|| Extension == TEXT("exr")
			|| Extension == TEXT("tga")
			|| Extension == TEXT("hdr");
	}

	FString NormalizeCaptureImageOutputPath(const FString& OutputPath)
	{
		const FString Extension = FPaths::GetExtension(OutputPath).ToLower();
		return IsSupportedCaptureImageExtension(Extension)
			? OutputPath
			: FPaths::SetExtension(OutputPath, TEXT("png"));
	}

	FString ResolveCapturedImageFormat(const FString& OutputPath)
	{
		const FString Extension = FPaths::GetExtension(OutputPath).ToLower();
		if (Extension == TEXT("jpg") || Extension == TEXT("jpeg"))
		{
			return TEXT("jpg");
		}
		if (IsSupportedCaptureImageExtension(Extension))
		{
			return Extension;
		}
		return TEXT("png");
	}

	constexpr int32 GifMinFPS = 20;
	constexpr int32 GifDefaultFPS = 60;
	constexpr int32 GifMaxPreferredFPS = 60;
	constexpr int32 GifHardFrameCap = 1000;

	const TCHAR* GetGifFpsPolicyText()
	{
		return TEXT("60 fps default target when affordable; 30 fps and 20 fps fallback tiers under resource limits; 20 fps quality minimum unless the hard frame cap forces degradation");
	}

	int32 GetAdaptiveGifFrameBudget(int32 Width)
	{
		int32 FrameBudget = 240; // 60 fps for 4s, 30 fps for 8s at small preview sizes.
		if (Width >= 1024)
		{
			FrameBudget = 120;
		}
		else if (Width >= 512)
		{
			FrameBudget = 180;
		}

		const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
		constexpr uint64 OneGiB = 1024ull * 1024ull * 1024ull;
		if (MemoryStats.AvailablePhysical > 0 && MemoryStats.AvailablePhysical < 2ull * OneGiB)
		{
			FrameBudget = FMath::Min(FrameBudget, 120);
		}
		if (MemoryStats.AvailablePhysical > 0 && MemoryStats.AvailablePhysical < OneGiB)
		{
			FrameBudget = FMath::Min(FrameBudget, 80);
		}

		return FMath::Clamp(FrameBudget, 1, GifHardFrameCap);
	}

	bool ReadIntegerParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		int32& InOutValue,
		FString& OutError,
		int32 MinValue,
		int32 MaxValue)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}

		double Number = 0.0;
		if (!Params->TryGetNumberField(FieldName, Number)
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue)
			|| static_cast<double>(static_cast<int64>(Number)) != Number)
		{
			OutError = FString::Printf(TEXT("%s must be an integer between %d and %d."), FieldName, MinValue, MaxValue);
			return false;
		}

		InOutValue = static_cast<int32>(Number);
		return true;
	}

	FString NormalizeFrameDiskPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		if (FPaths::IsRelative(Path))
		{
			Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Path);
		}
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	bool AddUniquePngFramePath(
		const FString& RawPath,
		TArray<FString>& OutFramePaths,
		TSet<FString>& InOutSeen,
		FString& OutError)
	{
		const FString FramePath = NormalizeFrameDiskPath(RawPath);
		if (FramePath.IsEmpty())
		{
			OutError = TEXT("frame_paths contains an empty path.");
			return false;
		}

		if (!FPaths::GetExtension(FramePath).Equals(TEXT("png"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("Only PNG frames are supported: %s"), *FramePath);
			return false;
		}

		if (!IFileManager::Get().FileExists(*FramePath))
		{
			OutError = FString::Printf(TEXT("Frame PNG does not exist: %s"), *FramePath);
			return false;
		}

		if (!InOutSeen.Contains(FramePath))
		{
			InOutSeen.Add(FramePath);
			OutFramePaths.Add(FramePath);
		}
		return true;
	}

	bool ResolveFrameSequencePaths(
		const TSharedPtr<FJsonObject>& Params,
		TArray<FString>& OutFramePaths,
		FString& OutFrameDir,
		FString& OutError)
	{
		TSet<FString> SeenPaths;
		const TArray<TSharedPtr<FJsonValue>>* FrameValues = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("frame_paths"), FrameValues))
		{
			for (int32 Index = 0; Index < FrameValues->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*FrameValues)[Index];
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					OutError = FString::Printf(TEXT("frame_paths[%d] must be a string."), Index);
					return false;
				}
				if (!AddUniquePngFramePath(Value->AsString(), OutFramePaths, SeenPaths, OutError))
				{
					return false;
				}
			}
		}

		FString FrameDir;
		if (Params.IsValid() && Params->TryGetStringField(TEXT("frame_dir"), FrameDir) && !FrameDir.IsEmpty())
		{
			FrameDir = NormalizeFrameDiskPath(FrameDir);
			if (!IFileManager::Get().DirectoryExists(*FrameDir))
			{
				OutError = FString::Printf(TEXT("frame_dir does not exist: %s"), *FrameDir);
				return false;
			}

			FString Pattern = TEXT("*.png");
			Params->TryGetStringField(TEXT("pattern"), Pattern);
			Pattern.TrimStartAndEndInline();
			if (Pattern.IsEmpty())
			{
				Pattern = TEXT("*.png");
			}
			if (!FPaths::GetPath(Pattern).IsEmpty())
			{
				OutError = TEXT("pattern must be a filename glob, not a path.");
				return false;
			}

			TArray<FString> FoundNames;
			IFileManager::Get().FindFiles(FoundNames, *(FrameDir / Pattern), true, false);
			FoundNames.Sort();
			for (const FString& Name : FoundNames)
			{
				if (!AddUniquePngFramePath(FrameDir / Name, OutFramePaths, SeenPaths, OutError))
				{
					return false;
				}
			}
			OutFrameDir = FrameDir;
		}
		else if (Params.IsValid() && Params->HasField(TEXT("frame_dir")))
		{
			OutError = TEXT("frame_dir must be a non-empty string when provided.");
			return false;
		}

		if (OutFramePaths.Num() == 0)
		{
			OutError = TEXT("Provide frame_paths or frame_dir containing at least one PNG frame.");
			return false;
		}

		if (OutFrameDir.IsEmpty())
		{
			OutFrameDir = FPaths::GetPath(OutFramePaths[0]);
		}

		return true;
	}

	bool ReadPngFrameDimensions(const FString& FramePath, FIntPoint& OutSize)
	{
		OutSize = FIntPoint::ZeroValue;

		TArray<uint8> CompressedData;
		if (!FFileHelper::LoadFileToArray(CompressedData, *FramePath) || CompressedData.Num() == 0)
		{
			return false;
		}

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName(TEXT("ImageWrapper")));
		const TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
		{
			return false;
		}

		OutSize = FIntPoint(ImageWrapper->GetWidth(), ImageWrapper->GetHeight());
		return OutSize.X > 0 && OutSize.Y > 0;
	}

	FString ResolveGifOutputPath(
		const TSharedPtr<FJsonObject>& Params,
		const FString& BaseDir,
		FString& OutError)
	{
		FString OutputPath;
		if (Params.IsValid() && Params->HasField(TEXT("output_path")))
		{
			if (!Params->TryGetStringField(TEXT("output_path"), OutputPath))
			{
				OutError = TEXT("output_path must be a string.");
				return FString();
			}
		}

		if (OutputPath.IsEmpty())
		{
			const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
			const FString OutputDir = BaseDir.IsEmpty()
				? (FPaths::ProjectSavedDir() / TEXT("Screenshots/Monolith"))
				: BaseDir;
			OutputPath = OutputDir / FString::Printf(TEXT("FrameSequence_%s.gif"), *Timestamp);
		}
		else if (FPaths::IsRelative(OutputPath))
		{
			OutputPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), OutputPath);
		}

		FPaths::NormalizeFilename(OutputPath);
		if (!FPaths::GetExtension(OutputPath).Equals(TEXT("gif"), ESearchCase::IgnoreCase))
		{
			OutputPath = FPaths::SetExtension(OutputPath, TEXT("gif"));
		}

		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!IFileManager::Get().MakeDirectory(*OutputDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create output directory: %s"), *OutputDir);
			return FString();
		}

		return OutputPath;
	}

	void BuildSampledFrameList(const TArray<FString>& InputFrames, int32 DesiredCount, TArray<FString>& OutFrames)
	{
		OutFrames.Reset();
		if (InputFrames.Num() == 0 || DesiredCount <= 0)
		{
			return;
		}

		if (DesiredCount >= InputFrames.Num())
		{
			OutFrames = InputFrames;
			return;
		}

		OutFrames.Reserve(DesiredCount);
		if (DesiredCount == 1)
		{
			OutFrames.Add(InputFrames[0]);
			return;
		}

		const double LastInputIndex = static_cast<double>(InputFrames.Num() - 1);
		const double LastOutputIndex = static_cast<double>(DesiredCount - 1);
		int32 PreviousIndex = -1;
		for (int32 Index = 0; Index < DesiredCount; ++Index)
		{
			int32 SourceIndex = FMath::RoundToInt((static_cast<double>(Index) / LastOutputIndex) * LastInputIndex);
			SourceIndex = FMath::Clamp(SourceIndex, 0, InputFrames.Num() - 1);
			if (SourceIndex <= PreviousIndex)
			{
				SourceIndex = FMath::Min(PreviousIndex + 1, InputFrames.Num() - 1);
			}
			OutFrames.Add(InputFrames[SourceIndex]);
			PreviousIndex = SourceIndex;
		}
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Array;
		Array.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Array.Add(MakeShared<FJsonValueString>(Value));
		}
		return Array;
	}

	FString EscapePythonSingleQuotedString(FString Value)
	{
		Value.ReplaceInline(TEXT("\\"), TEXT("/"));
		Value.ReplaceInline(TEXT("'"), TEXT("\\'"));
		Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		return Value;
	}

	FString MakePythonIntegerList(const TArray<int32>& Values)
	{
		FString Result;
		for (const int32 Value : Values)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT(",");
			}
			Result += FString::FromInt(Value);
		}
		return Result;
	}

	struct FMonolithGifEncodeResult
	{
		bool bEncoded = false;
		FString EncoderUsed = TEXT("frames_only");
		FString Error;
	};

	bool TryEncodeGifWithFFmpeg(
		const TArray<FString>& FramePaths,
		const FString& GifPath,
		int32 FPS,
		int32 ScaleWidth,
		FString& OutError)
	{
		const FString OutputDir = FPaths::GetPath(GifPath);
		const FString SafeBaseName = MakeCaptureSafeFileName(FPaths::GetBaseFilename(GifPath));
		const FString FrameListPath = OutputDir / FString::Printf(TEXT("%s_ffmpeg_frames.txt"), *SafeBaseName);
		FString FrameList;
		const double FrameDuration = 1.0 / static_cast<double>(FPS);
		for (const FString& Path : FramePaths)
		{
			FString Escaped = Path.Replace(TEXT("\\"), TEXT("/"));
			Escaped.ReplaceInline(TEXT("'"), TEXT("'\\''"));
			FrameList += FString::Printf(TEXT("file '%s'\n"), *Escaped);
			FrameList += FString::Printf(TEXT("duration %.8f\n"), FrameDuration);
		}

		if (FramePaths.Num() > 0)
		{
			FString LastFrame = FramePaths.Last().Replace(TEXT("\\"), TEXT("/"));
			LastFrame.ReplaceInline(TEXT("'"), TEXT("'\\''"));
			FrameList += FString::Printf(TEXT("file '%s'\n"), *LastFrame);
		}

		if (!FFileHelper::SaveStringToFile(FrameList, *FrameListPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("failed to write ffmpeg concat list: %s"), *FrameListPath);
			return false;
		}

		// Temporal proof GIFs are opaque review artifacts, so preserve captured RGB while discarding Slate back-buffer alpha.
		const FString Filter = ScaleWidth > 0
			? FString::Printf(TEXT("fps=%d,scale=%d:-1:flags=lanczos,format=rgb24"), FPS, ScaleWidth)
			: FString::Printf(TEXT("fps=%d,format=rgb24"), FPS);
		const FString FFmpegArgs = FString::Printf(
			TEXT("-y -f concat -safe 0 -i \"%s\" -vf \"%s\" -loop 0 \"%s\""),
			*FrameListPath, *Filter, *GifPath);

		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		const bool bLaunched = FPlatformProcess::ExecProcess(TEXT("ffmpeg"), *FFmpegArgs, &ReturnCode, &StdOut, &StdErr);
		if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("ffmpeg failed (code %d). Ensure ffmpeg is in PATH. stderr: %s"),
			ReturnCode, *StdErr.Left(500));
		return false;
	}

	bool TryEncodeGifWithPython(
		const TArray<FString>& FramePaths,
		const FString& GifPath,
		int32 FPS,
		int32 ScaleWidth,
		FString& OutError)
	{
		if (ScaleWidth > 0)
		{
			OutError = TEXT("python imageio fallback does not support scale_width; use ffmpeg or omit scale_width.");
			return false;
		}

		FString FrameListStr;
		for (const FString& Path : FramePaths)
		{
			if (!FrameListStr.IsEmpty())
			{
				FrameListStr += TEXT(",");
			}
			FrameListStr += FString::Printf(TEXT("'%s'"), *EscapePythonSingleQuotedString(Path));
		}

		const TArray<int32> FrameDelaysMilliseconds = MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(
			FramePaths.Num(),
			FPS);
		if (FrameDelaysMilliseconds.Num() != FramePaths.Num())
		{
			OutError = TEXT("failed to build the GIF frame-delay schedule.");
			return false;
		}

		const FString NormalizedGifPath = EscapePythonSingleQuotedString(GifPath);
		const FString FrameDelaysStr = MakePythonIntegerList(FrameDelaysMilliseconds);
		// Pillow consumes millisecond delays but GIF stores centiseconds. The per-frame
		// cumulative-rounding schedule preserves the selected cadence without drift.
		// RGB input keeps the Python path aligned with ffmpeg's opaque contract.
		const FString PyScript = FString::Printf(
			TEXT("import imageio.v3 as iio; frames=[iio.imread(p,mode='RGB') for p in [%s]]; iio.imwrite('%s',frames,extension='.gif',plugin='pillow',duration=[%s],loop=0)"),
			*FrameListStr,
			*NormalizedGifPath,
			*FrameDelaysStr);

		const FString PythonArgs = FString::Printf(TEXT("-c \"%s\""), *PyScript);

		int32 ReturnCode = -1;
		FString StdOut;
		FString StdErr;
		const bool bLaunched = FPlatformProcess::ExecProcess(TEXT("python"), *PythonArgs, &ReturnCode, &StdOut, &StdErr);
		if (bLaunched && ReturnCode == 0 && IFileManager::Get().FileExists(*GifPath))
		{
			return true;
		}

		OutError = FString::Printf(TEXT("python imageio failed (code %d). Ensure python + imageio + Pillow are installed. stderr: %s"),
			ReturnCode, *StdErr.Left(500));
		return false;
	}

	FMonolithGifEncodeResult EncodeGifFromPngFrames(
		const TArray<FString>& FramePaths,
		const FString& GifPath,
		int32 FPS,
		int32 ScaleWidth,
		const FString& Encoder)
	{
		FMonolithGifEncodeResult Result;
		if (Encoder == TEXT("frames_only"))
		{
			return Result;
		}
		if (FramePaths.Num() == 0)
		{
			Result.Error = TEXT("No PNG frames were provided for GIF encoding.");
			return Result;
		}

		FString FfmpegError;
		FString PythonError;
		if (Encoder == TEXT("ffmpeg"))
		{
			Result.bEncoded = TryEncodeGifWithFFmpeg(FramePaths, GifPath, FPS, ScaleWidth, FfmpegError);
			Result.EncoderUsed = Result.bEncoded ? TEXT("ffmpeg") : TEXT("frames_only");
			Result.Error = FfmpegError;
			return Result;
		}

		if (Encoder == TEXT("python"))
		{
			Result.bEncoded = TryEncodeGifWithPython(FramePaths, GifPath, FPS, ScaleWidth, PythonError);
			Result.EncoderUsed = Result.bEncoded ? TEXT("python") : TEXT("frames_only");
			Result.Error = PythonError;
			return Result;
		}

		if (Encoder == TEXT("auto"))
		{
			if (TryEncodeGifWithFFmpeg(FramePaths, GifPath, FPS, ScaleWidth, FfmpegError))
			{
				Result.bEncoded = true;
				Result.EncoderUsed = TEXT("ffmpeg");
				return Result;
			}
			if (TryEncodeGifWithPython(FramePaths, GifPath, FPS, ScaleWidth, PythonError))
			{
				Result.bEncoded = true;
				Result.EncoderUsed = TEXT("python");
				return Result;
			}

			Result.Error = FString::Printf(TEXT("auto encoding failed. ffmpeg: %s | python: %s"), *FfmpegError, *PythonError);
			return Result;
		}

		Result.Error = FString::Printf(TEXT("Unknown encoder '%s'. Valid: auto, frames_only, ffmpeg, python"), *Encoder);
		return Result;
	}
}

FMonolithActionResult FMonolithEditorActions::HandleListOpenViewports(const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	if (GEditor)
	{
		const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
		for (int32 Index = 0; Index < Clients.Num(); ++Index)
		{
			Rows.Add(MakeShared<FJsonValueObject>(MakeViewportRow(Clients[Index], Index)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("viewport_count"), Rows.Num());
	Result->SetArrayField(TEXT("viewports"), Rows);
	Result->SetStringField(TEXT("asset_editor_capture_status"), TEXT("unavailable"));
	Result->SetStringField(TEXT("widget_designer_capture_status"), TEXT("unavailable"));
	Result->SetStringField(TEXT("asset_thumbnail_capture_status"), TEXT("available_with_explicit_thumbnail_fallback"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureLevelViewport(const TSharedPtr<FJsonObject>& Params)
{
	int32 ViewportIndex = 0;
	double ViewportIndexNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("viewport_index"), ViewportIndexNumber))
	{
		if (ViewportIndexNumber < 0.0
			|| ViewportIndexNumber > static_cast<double>(MAX_int32)
			|| static_cast<double>(static_cast<int64>(ViewportIndexNumber)) != ViewportIndexNumber)
		{
			return FMonolithActionResult::Error(TEXT("viewport_index must be a non-negative integer"), FMonolithJsonUtils::ErrInvalidParams);
		}
		ViewportIndex = static_cast<int32>(ViewportIndexNumber);
	}
	else if (Params->HasField(TEXT("viewport_index")))
	{
		return FMonolithActionResult::Error(TEXT("viewport_index must be a non-negative integer"), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Error;
	FLevelEditorViewportClient* Client = GetLevelViewportClientByIndex(ViewportIndex, Error);
	if (!Client)
	{
		return FMonolithActionResult::Error(Error);
	}

	const TSharedPtr<FJsonObject>* CameraObject = nullptr;
	TSharedPtr<FJsonObject> ParsedCamera;
	if (Params->HasField(TEXT("camera")))
	{
		if (!Params->TryGetObjectField(TEXT("camera"), CameraObject))
		{
			FString CameraStr;
			if (Params->TryGetStringField(TEXT("camera"), CameraStr) && !CameraStr.IsEmpty())
			{
				ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
				if (!ParsedCamera.IsValid())
				{
					return FMonolithActionResult::Error(TEXT("Invalid param: 'camera' JSON string could not be parsed"), FMonolithJsonUtils::ErrInvalidParams);
				}
				CameraObject = &ParsedCamera;
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'camera' must be an object or JSON string"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	if (CameraObject && CameraObject->IsValid())
	{
		FVector Location;
		if (ReadVectorArray(*CameraObject, TEXT("location"), Location))
		{
			Client->SetViewLocation(Location);
		}
		else if ((*CameraObject)->HasField(TEXT("location")))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.location' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
		}

		FRotator Rotation;
		if (ReadRotatorArray(*CameraObject, TEXT("rotation"), Rotation))
		{
			Client->SetViewRotation(Rotation);
		}
		else if ((*CameraObject)->HasField(TEXT("rotation")))
		{
			return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.rotation' must be an array"), FMonolithJsonUtils::ErrInvalidParams);
		}

		double Fov = 0.0;
		if ((*CameraObject)->HasField(TEXT("fov")))
		{
			if ((*CameraObject)->TryGetNumberField(TEXT("fov"), Fov))
			{
				if (Fov > 0.0)
				{
					Client->ViewFOV = FMath::Clamp(static_cast<float>(Fov), 5.0f, 170.0f);
				}
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid param: 'camera.fov' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		Client->Invalidate();
	}

	const FIntPoint Size = Client->Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return FMonolithActionResult::Error(TEXT("Selected level viewport has zero size"));
	}

	TArray<FColor> Pixels;
	if (!Client->Viewport->ReadPixels(Pixels) || Pixels.Num() != Size.X * Size.Y)
	{
		return FMonolithActionResult::Error(TEXT("Failed to read pixels from selected level viewport"));
	}

	FString OutputPath;
	if (Params->HasField(TEXT("output_path")) && !Params->TryGetStringField(TEXT("output_path"), OutputPath))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'output_path' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (OutputPath.IsEmpty())
	{
		OutputPath = FPaths::ProjectSavedDir() / TEXT("Screenshots/Monolith/LevelViewport_") + FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")) + TEXT(".png");
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);

	FImage Image;
	Image.Init(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), Pixels.Num() * sizeof(FColor));
	if (!FImageUtils::SaveImageAutoFormat(*OutputPath, Image))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save viewport capture: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Result = MakeViewportRow(Client, ViewportIndex);
	Result->SetStringField(TEXT("output_path"), OutputPath);
	Result->SetBoolField(TEXT("fallback_used"), false);
	Result->SetStringField(TEXT("format"), TEXT("png"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureAssetEditorViewport(const TSharedPtr<FJsonObject>& Params)
{
	return MakeViewportCaptureUnavailable(TEXT("editor.capture_asset_editor_viewport"),
		TEXT("Asset editor viewport discovery is not implemented yet. Monolith refuses to fall back to level viewport capture."));
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureWidgetDesigner(const TSharedPtr<FJsonObject>& Params)
{
	return MakeViewportCaptureUnavailable(TEXT("editor.capture_widget_designer"),
		TEXT("Widget designer viewport discovery is not implemented yet. Monolith refuses to fall back to level viewport capture."));
}

FMonolithActionResult FMonolithEditorActions::HandleCaptureAssetThumbnail(const TSharedPtr<FJsonObject>& Params)
{
	bool bThumbnailFallback = false;
	if (Params->HasField(TEXT("thumbnail_fallback"))
		&& !Params->TryGetBoolField(TEXT("thumbnail_fallback"), bThumbnailFallback))
	{
		return FMonolithActionResult::Error(
			TEXT("thumbnail_fallback must be a boolean."),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (!bThumbnailFallback)
	{
		return FMonolithActionResult::Error(
			TEXT("thumbnail_fallback=true is required for thumbnail capture; Monolith will not silently substitute thumbnails for viewport captures."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("asset_path is required."), FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 ThumbnailSize = 256;
	FString SizeError;
	if (!ReadThumbnailSize(Params, ThumbnailSize, SizeError))
	{
		return FMonolithActionResult::Error(SizeError, FMonolithJsonUtils::ErrInvalidParams);
	}

	const FString ResolvedAssetPath = FMonolithAssetUtils::ResolveAssetPath(AssetPath);
	UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(ResolvedAssetPath);
	if (!Asset)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Asset not found or could not be loaded: %s"), *ResolvedAssetPath),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FObjectThumbnail Thumbnail;
	ThumbnailTools::RenderThumbnail(
		Asset,
		static_cast<uint32>(ThumbnailSize),
		static_cast<uint32>(ThumbnailSize),
		ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush,
		nullptr,
		&Thumbnail);

	const int32 Width = Thumbnail.GetImageWidth();
	const int32 Height = Thumbnail.GetImageHeight();
	const TArray<uint8>& ImageData = Thumbnail.GetUncompressedImageData();
	const int64 ExpectedBytes = static_cast<int64>(Width) * static_cast<int64>(Height) * 4;
	if (Width <= 0 || Height <= 0 || ExpectedBytes <= 0 || ImageData.Num() < ExpectedBytes)
	{
		TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
		ErrorData->SetStringField(TEXT("status"), TEXT("thumbnail_unavailable"));
		ErrorData->SetStringField(TEXT("source"), TEXT("asset_thumbnail"));
		ErrorData->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
		ErrorData->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT(""));
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("No renderable thumbnail pixels were produced for asset: %s"), *ResolvedAssetPath))
			.WithErrorData(ErrorData);
	}

	const FString OutputPath = NormalizeCaptureImageOutputPath(ResolveCaptureOutputPath(Params, TEXT("AssetThumbnail"), Asset->GetName()));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);

	FImage Image;
	Image.Init(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), ImageData.GetData(), ExpectedBytes);
	if (!FImageUtils::SaveImageByExtension(*OutputPath, Image))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to save asset thumbnail capture: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("captured"));
	Result->SetStringField(TEXT("source"), TEXT("asset_thumbnail"));
	Result->SetStringField(TEXT("asset_path"), ResolvedAssetPath);
	Result->SetStringField(TEXT("asset_name"), Asset->GetName());
	Result->SetStringField(TEXT("asset_class"), Asset->GetClass() ? Asset->GetClass()->GetName() : TEXT(""));
	Result->SetStringField(TEXT("output_path"), OutputPath);
	Result->SetStringField(TEXT("format"), ResolveCapturedImageFormat(OutputPath));
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	Result->SetBoolField(TEXT("fallback_used"), true);
	Result->SetBoolField(TEXT("thumbnail_fallback"), true);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Action: capture_system_gif
// Captures a Niagara system as a sequence of PNG frames with optional GIF encoding.
// Default mode: auto — returns PNG frame paths and tries ffmpeg, then python imageio.
// FPS policy: 60 by default when affordable, 30/20 fallback tiers unless
// the hard 1000-frame capture_sequence_frames cap forces a lower degraded rate.
// ============================================================================
FMonolithActionResult FMonolithEditorActions::HandleCaptureSystemGif(
	const TSharedPtr<FJsonObject>& Params)
{
	constexpr int32 MinGifFPS = GifMinFPS;
	constexpr int32 DefaultGifFPS = GifDefaultFPS;
	constexpr int32 MaxPreferredGifFPS = GifMaxPreferredFPS;
	constexpr int32 HardFrameCap = GifHardFrameCap;

	FString AssetPath;
	if (Params->HasField(TEXT("asset_path")) && !Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'asset_path' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	if (AssetPath.IsEmpty())
		return FMonolithActionResult::Error(TEXT("asset_path is required"));

	double DurationSeconds = 2.0;
	if (Params->HasField(TEXT("duration_seconds")) && !Params->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'duration_seconds' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
	}
	double FpsDouble = static_cast<double>(DefaultGifFPS);
	if (Params->HasField(TEXT("fps")) && !Params->TryGetNumberField(TEXT("fps"), FpsDouble))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'fps' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
	}
	int32 RequestedFPS = FMath::RoundToInt(FpsDouble);
	double ResDouble = 256.0;
	if (Params->HasField(TEXT("resolution")) && !Params->TryGetNumberField(TEXT("resolution"), ResDouble))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'resolution' must be a number"), FMonolithJsonUtils::ErrInvalidParams);
	}
	int32 Resolution = static_cast<int32>(ResDouble);
	FString Encoder = TEXT("auto");
	if (Params->HasField(TEXT("encoder")) && !Params->TryGetStringField(TEXT("encoder"), Encoder))
	{
		return FMonolithActionResult::Error(TEXT("Invalid param: 'encoder' must be a string"), FMonolithJsonUtils::ErrInvalidParams);
	}
	Encoder = Encoder.ToLower();

	if (RequestedFPS <= 0) RequestedFPS = DefaultGifFPS;
	int32 FPS = FMath::Clamp(RequestedFPS, MinGifFPS, MaxPreferredGifFPS);
	if (Resolution <= 0) Resolution = 256;
	if (DurationSeconds <= 0) DurationSeconds = 2.0;

	FString FpsAdjustment;
	if (FPS != RequestedFPS)
	{
		FpsAdjustment = FString::Printf(TEXT("clamped requested fps %d to %d (allowed quality range %d-%d)"),
			RequestedFPS, FPS, MinGifFPS, MaxPreferredGifFPS);
	}

	int32 FrameBudget = 240; // 60 fps for 4s, 30 fps for 8s at small preview sizes.
	if (Resolution >= 1024)
	{
		FrameBudget = 120;
	}
	else if (Resolution >= 512)
	{
		FrameBudget = 180;
	}

	const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
	constexpr uint64 OneGiB = 1024ull * 1024ull * 1024ull;
	if (MemoryStats.AvailablePhysical > 0 && MemoryStats.AvailablePhysical < 2ull * OneGiB)
	{
		FrameBudget = FMath::Min(FrameBudget, 120);
	}
	if (MemoryStats.AvailablePhysical > 0 && MemoryStats.AvailablePhysical < OneGiB)
	{
		FrameBudget = FMath::Min(FrameBudget, 80);
	}

	int32 FrameCount = FMath::Max(1, FMath::FloorToInt(DurationSeconds * static_cast<double>(FPS)));
	if (FrameCount > FrameBudget)
	{
		const int32 BudgetFPS = FMath::FloorToInt(static_cast<double>(FrameBudget) / DurationSeconds);
		const int32 AdjustedFPS = FMath::Clamp(BudgetFPS, MinGifFPS, FPS);
		if (AdjustedFPS < FPS)
		{
			if (!FpsAdjustment.IsEmpty())
			{
				FpsAdjustment += TEXT("; ");
			}
			FpsAdjustment += FString::Printf(TEXT("reduced fps %d to %d for frame budget %d at %dpx"),
				FPS, AdjustedFPS, FrameBudget, Resolution);
			FPS = AdjustedFPS;
			FrameCount = FMath::Max(1, FMath::FloorToInt(DurationSeconds * static_cast<double>(FPS)));
		}
	}

	bool bBelowMinimumDueToHardCap = false;
	if (FrameCount > HardFrameCap)
	{
		const int32 HardCapFPS = FMath::Max(1, FMath::FloorToInt(static_cast<double>(HardFrameCap) / DurationSeconds));
		const int32 AdjustedFPS = FMath::Min(FPS, HardCapFPS);
		if (AdjustedFPS < FPS)
		{
			if (!FpsAdjustment.IsEmpty())
			{
				FpsAdjustment += TEXT("; ");
			}
			if (AdjustedFPS < MinGifFPS)
			{
				bBelowMinimumDueToHardCap = true;
				FpsAdjustment += FString::Printf(TEXT("degraded below %d fps to %d because duration %.2fs would exceed the %d-frame action cap"),
					MinGifFPS, AdjustedFPS, DurationSeconds, HardFrameCap);
			}
			else
			{
				FpsAdjustment += FString::Printf(TEXT("reduced fps %d to %d to stay within the %d-frame action cap"),
					FPS, AdjustedFPS, HardFrameCap);
			}
			FPS = AdjustedFPS;
			FrameCount = FMath::Max(1, FMath::FloorToInt(DurationSeconds * static_cast<double>(FPS)));
		}
	}

	if (FrameCount > HardFrameCap)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("capture_system_gif would require %d frames, exceeding the hard %d-frame cap. Reduce duration_seconds."),
			FrameCount, HardFrameCap));
	}

	// Output directory
	FString OutputDir;
	if (Params->TryGetStringField(TEXT("output_path"), OutputDir))
	{
		if (FPaths::IsRelative(OutputDir))
		{
			OutputDir = FPaths::ProjectDir() / OutputDir;
		}
	}
	else
	{
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		FString SafeName = FPaths::GetBaseFilename(AssetPath);
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith") /
			FString::Printf(TEXT("GIF_%s_%s"), *Timestamp, *SafeName);
	}
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	// Load system
	UNiagaraSystem* System = FMonolithAssetUtils::LoadAssetByPath<UNiagaraSystem>(AssetPath);
	if (!System)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to load Niagara system: %s"), *AssetPath));

	// Generate timestamps
	TArray<float> Timestamps;
	for (int32 i = 0; i < FrameCount; i++)
	{
		Timestamps.Add(static_cast<float>(i) / static_cast<float>(FPS));
	}

	// Build params for capture_sequence_frames (persistent mode)
	TArray<TSharedPtr<FJsonValue>> TimestampValues;
	for (float T : Timestamps)
	{
		TimestampValues.Add(MakeShared<FJsonValueNumber>(T));
	}

	TSharedRef<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
	CaptureParams->SetStringField(TEXT("asset_path"), AssetPath);
	CaptureParams->SetStringField(TEXT("asset_type"), TEXT("niagara"));
	CaptureParams->SetArrayField(TEXT("timestamps"), TimestampValues);
	CaptureParams->SetStringField(TEXT("output_dir"), OutputDir);
	CaptureParams->SetStringField(TEXT("filename_prefix"), TEXT("gif_frame"));
	CaptureParams->SetBoolField(TEXT("persistent"), true);

	// Set resolution
	TArray<TSharedPtr<FJsonValue>> ResArr;
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	ResArr.Add(MakeShared<FJsonValueNumber>(Resolution));
	CaptureParams->SetArrayField(TEXT("resolution"), ResArr);

	// Capture frames
	FMonolithActionResult CaptureResult = HandleCaptureSequenceFrames(CaptureParams);
	if (!CaptureResult.bSuccess)
		return FMonolithActionResult::Error(FString::Printf(TEXT("Frame capture failed: %s"), *CaptureResult.ErrorMessage));

	// Collect frame paths from the capture result
	TArray<FString> FramePaths;
	const TArray<TSharedPtr<FJsonValue>>* FramesArr = nullptr;
	if (CaptureResult.Result.IsValid() && CaptureResult.Result->TryGetArrayField(TEXT("frames"), FramesArr))
	{
		for (const auto& FV : *FramesArr)
		{
			const TSharedPtr<FJsonObject> FrameObj = FV->AsObject();
			if (FrameObj.IsValid())
			{
				FString FilePath;
				FrameObj->TryGetStringField(TEXT("file"), FilePath);
				if (!FilePath.IsEmpty())
					FramePaths.Add(FilePath);
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetNumberField(TEXT("frame_count"), FramePaths.Num());
	Result->SetNumberField(TEXT("duration"), DurationSeconds);
	Result->SetNumberField(TEXT("fps"), FPS);
	Result->SetNumberField(TEXT("requested_fps"), RequestedFPS);
	Result->SetNumberField(TEXT("min_quality_fps"), MinGifFPS);
	Result->SetNumberField(TEXT("max_preferred_fps"), MaxPreferredGifFPS);
	Result->SetNumberField(TEXT("frame_budget"), FrameBudget);
	Result->SetBoolField(TEXT("fps_adjusted"), !FpsAdjustment.IsEmpty());
	Result->SetBoolField(TEXT("fps_below_quality_minimum"), bBelowMinimumDueToHardCap);
	Result->SetStringField(TEXT("fps_policy"), GetGifFpsPolicyText());
	if (!FpsAdjustment.IsEmpty())
	{
		Result->SetStringField(TEXT("fps_adjustment"), FpsAdjustment);
	}
	Result->SetNumberField(TEXT("resolution"), Resolution);
	Result->SetStringField(TEXT("output_dir"), OutputDir);

	// Always include frame paths
	Result->SetArrayField(TEXT("frame_paths"), MakeStringArray(FramePaths));

	// Optional GIF encoding
	if (Encoder != TEXT("frames_only") && FramePaths.Num() > 0)
	{
		const FString GifPath = OutputDir / TEXT("output.gif");
		const FMonolithGifEncodeResult EncodeResult = EncodeGifFromPngFrames(FramePaths, GifPath, FPS, 0, Encoder);
		Result->SetStringField(TEXT("encoder_used"), EncodeResult.EncoderUsed);
		if (EncodeResult.bEncoded)
		{
			Result->SetStringField(TEXT("gif_path"), GifPath);
			const double NominalGifDurationSeconds =
				static_cast<double>(FramePaths.Num()) / static_cast<double>(FPS);
			Result->SetNumberField(TEXT("gif_duration_seconds"), NominalGifDurationSeconds);
			Result->SetNumberField(TEXT("nominal_gif_duration_seconds"), NominalGifDurationSeconds);
			Result->SetStringField(TEXT("gif_duration_kind"), TEXT("nominal_frame_count_over_fps"));
			if (EncodeResult.EncoderUsed == TEXT("python"))
			{
				const TArray<int32> FrameDelaysMilliseconds = MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(
					FramePaths.Num(),
					FPS);
				const double EncodedGifDurationSeconds =
					static_cast<double>(MonolithEditorGifTiming::SumFrameDelaysMilliseconds(FrameDelaysMilliseconds)) / 1000.0;
				Result->SetStringField(TEXT("gif_timing_mode"), TEXT("cumulative_centisecond_rounding"));
				Result->SetNumberField(TEXT("gif_delay_unit_ms"), 10);
				Result->SetNumberField(TEXT("encoded_gif_duration_seconds"), EncodedGifDurationSeconds);
				Result->SetNumberField(
					TEXT("gif_duration_quantization_error_seconds"),
					EncodedGifDurationSeconds - NominalGifDurationSeconds);
			}
			else if (EncodeResult.EncoderUsed == TEXT("ffmpeg"))
			{
				Result->SetStringField(TEXT("gif_timing_mode"), TEXT("ffmpeg_fps_filter"));
			}
		}
		else if (!EncodeResult.Error.IsEmpty())
		{
			Result->SetStringField(TEXT("encoder_error"), EncodeResult.Error);
		}
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Action: encode_frame_sequence_gif
// Encodes already-captured PNG frames to GIF without recapturing editor content.
// ============================================================================
FMonolithActionResult FMonolithEditorActions::HandleEncodeFrameSequenceGif(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> InputFramePaths;
	FString FrameDir;
	FString Error;
	if (!ResolveFrameSequencePaths(Params, InputFramePaths, FrameDir, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	FString Encoder = TEXT("auto");
	if (Params.IsValid() && Params->HasField(TEXT("encoder")) && !Params->TryGetStringField(TEXT("encoder"), Encoder))
	{
		return FMonolithActionResult::Error(TEXT("encoder must be a string."), FMonolithJsonUtils::ErrInvalidParams);
	}
	Encoder = Encoder.ToLower();
	if (Encoder != TEXT("auto") && Encoder != TEXT("frames_only") && Encoder != TEXT("ffmpeg") && Encoder != TEXT("python"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Unknown encoder '%s'. Valid: auto, frames_only, ffmpeg, python"), *Encoder),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 RequestedFPS = GifDefaultFPS;
	if (!ReadIntegerParam(Params, TEXT("fps"), RequestedFPS, Error, 1, GifHardFrameCap))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 SourceFPS = RequestedFPS;
	if (!ReadIntegerParam(Params, TEXT("source_fps"), SourceFPS, Error, 1, GifHardFrameCap))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	int32 ScaleWidth = 0;
	if (!ReadIntegerParam(Params, TEXT("scale_width"), ScaleWidth, Error, 0, 8192))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	FIntPoint SourceFrameSize = FIntPoint::ZeroValue;
	ReadPngFrameDimensions(InputFramePaths[0], SourceFrameSize);
	const int32 BudgetWidth = ScaleWidth > 0 ? ScaleWidth : (SourceFrameSize.X > 0 ? SourceFrameSize.X : 256);
	int32 FrameBudget = GetAdaptiveGifFrameBudget(BudgetWidth);
	int32 CallerFrameCap = GifHardFrameCap;
	if (!ReadIntegerParam(Params, TEXT("max_frames"), CallerFrameCap, Error, 1, GifHardFrameCap))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}
	FrameBudget = FMath::Clamp(FMath::Min(FrameBudget, CallerFrameCap), 1, GifHardFrameCap);

	int32 FPS = FMath::Clamp(RequestedFPS, GifMinFPS, GifMaxPreferredFPS);
	FString FpsAdjustment;
	if (FPS != RequestedFPS)
	{
		FpsAdjustment = FString::Printf(TEXT("clamped requested fps %d to %d (allowed quality range %d-%d)"),
			RequestedFPS, FPS, GifMinFPS, GifMaxPreferredFPS);
	}

	const double SourceDurationSeconds = FMath::Max(
		1.0 / static_cast<double>(SourceFPS),
		static_cast<double>(InputFramePaths.Num()) / static_cast<double>(SourceFPS));
	int32 DesiredFrameCount = FMath::Clamp(
		FMath::RoundToInt(SourceDurationSeconds * static_cast<double>(FPS)),
		1,
		InputFramePaths.Num());

	bool bBelowMinimumDueToHardCap = false;
	if (DesiredFrameCount > FrameBudget)
	{
		const int32 BudgetFPS = FMath::FloorToInt(static_cast<double>(FrameBudget) / SourceDurationSeconds);
		const int32 AdjustedFPS = FMath::Min(FPS, FMath::Max(1, BudgetFPS));
		if (AdjustedFPS < FPS)
		{
			if (!FpsAdjustment.IsEmpty())
			{
				FpsAdjustment += TEXT("; ");
			}
			if (AdjustedFPS < GifMinFPS)
			{
				bBelowMinimumDueToHardCap = true;
				FpsAdjustment += FString::Printf(TEXT("degraded below %d fps to %d because %.2fs of source frames would exceed the %d-frame budget"),
					GifMinFPS, AdjustedFPS, SourceDurationSeconds, FrameBudget);
			}
			else
			{
				FpsAdjustment += FString::Printf(TEXT("reduced fps %d to %d for frame budget %d"),
					FPS, AdjustedFPS, FrameBudget);
			}

			FPS = AdjustedFPS;
			DesiredFrameCount = FMath::Clamp(
				FMath::FloorToInt(SourceDurationSeconds * static_cast<double>(FPS)),
				1,
				InputFramePaths.Num());
		}
	}

	if (DesiredFrameCount > FrameBudget)
	{
		bBelowMinimumDueToHardCap = true;
		if (!FpsAdjustment.IsEmpty())
		{
			FpsAdjustment += TEXT("; ");
		}
		FpsAdjustment += FString::Printf(TEXT("sampled encoded frames from %d to %d because 1 fps would still exceed the frame budget"),
			DesiredFrameCount, FrameBudget);
		DesiredFrameCount = FrameBudget;
	}

	TArray<FString> EncodedFramePaths;
	BuildSampledFrameList(InputFramePaths, DesiredFrameCount, EncodedFramePaths);

	FString OutputPath = ResolveGifOutputPath(Params, FrameDir, Error);
	if (OutputPath.IsEmpty())
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	FMonolithGifEncodeResult EncodeResult;
	if (Encoder != TEXT("frames_only"))
	{
		EncodeResult = EncodeGifFromPngFrames(EncodedFramePaths, OutputPath, FPS, ScaleWidth, Encoder);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), Encoder == TEXT("frames_only") || EncodeResult.bEncoded);
	Result->SetStringField(TEXT("status"), EncodeResult.bEncoded ? TEXT("encoded") : (Encoder == TEXT("frames_only") ? TEXT("frames_only") : TEXT("encode_failed")));
	Result->SetStringField(TEXT("output_path"), OutputPath);
	Result->SetStringField(TEXT("gif_path"), EncodeResult.bEncoded ? OutputPath : FString());
	Result->SetStringField(TEXT("encoder_requested"), Encoder);
	Result->SetStringField(TEXT("encoder_used"), EncodeResult.EncoderUsed);
	Result->SetNumberField(TEXT("input_frame_count"), InputFramePaths.Num());
	Result->SetNumberField(TEXT("encoded_frame_count"), EncodedFramePaths.Num());
	Result->SetNumberField(TEXT("frame_count"), EncodedFramePaths.Num());
	Result->SetNumberField(TEXT("source_fps"), SourceFPS);
	Result->SetNumberField(TEXT("requested_fps"), RequestedFPS);
	Result->SetNumberField(TEXT("fps"), FPS);
	Result->SetNumberField(TEXT("min_quality_fps"), GifMinFPS);
	Result->SetNumberField(TEXT("max_preferred_fps"), GifMaxPreferredFPS);
	Result->SetNumberField(TEXT("source_duration_seconds"), SourceDurationSeconds);
	const double NominalGifDurationSeconds =
		static_cast<double>(EncodedFramePaths.Num()) / static_cast<double>(FPS);
	Result->SetNumberField(TEXT("gif_duration_seconds"), NominalGifDurationSeconds);
	Result->SetNumberField(TEXT("nominal_gif_duration_seconds"), NominalGifDurationSeconds);
	Result->SetStringField(TEXT("gif_duration_kind"), TEXT("nominal_frame_count_over_fps"));
	if (EncodeResult.bEncoded && EncodeResult.EncoderUsed == TEXT("python"))
	{
		const TArray<int32> FrameDelaysMilliseconds = MonolithEditorGifTiming::BuildFrameDelaysMilliseconds(
			EncodedFramePaths.Num(),
			FPS);
		const double EncodedGifDurationSeconds =
			static_cast<double>(MonolithEditorGifTiming::SumFrameDelaysMilliseconds(FrameDelaysMilliseconds)) / 1000.0;
		Result->SetStringField(TEXT("gif_timing_mode"), TEXT("cumulative_centisecond_rounding"));
		Result->SetNumberField(TEXT("gif_delay_unit_ms"), 10);
		Result->SetNumberField(TEXT("encoded_gif_duration_seconds"), EncodedGifDurationSeconds);
		Result->SetNumberField(
			TEXT("gif_duration_quantization_error_seconds"),
			EncodedGifDurationSeconds - NominalGifDurationSeconds);
	}
	else if (EncodeResult.bEncoded && EncodeResult.EncoderUsed == TEXT("ffmpeg"))
	{
		Result->SetStringField(TEXT("gif_timing_mode"), TEXT("ffmpeg_fps_filter"));
	}
	Result->SetNumberField(TEXT("frame_budget"), FrameBudget);
	Result->SetBoolField(TEXT("fps_adjusted"), !FpsAdjustment.IsEmpty());
	Result->SetBoolField(TEXT("fps_below_quality_minimum"), bBelowMinimumDueToHardCap);
	Result->SetStringField(TEXT("fps_policy"), GetGifFpsPolicyText());
	Result->SetStringField(TEXT("frame_dir"), FrameDir);
	Result->SetNumberField(TEXT("source_width"), SourceFrameSize.X);
	Result->SetNumberField(TEXT("source_height"), SourceFrameSize.Y);
	Result->SetNumberField(TEXT("scale_width"), ScaleWidth);
	Result->SetArrayField(TEXT("frame_paths"), MakeStringArray(InputFramePaths));
	Result->SetArrayField(TEXT("encoded_frame_paths"), MakeStringArray(EncodedFramePaths));
	if (!FpsAdjustment.IsEmpty())
	{
		Result->SetStringField(TEXT("fps_adjustment"), FpsAdjustment);
	}
	if (!EncodeResult.Error.IsEmpty())
	{
		Result->SetStringField(TEXT("encoder_error"), EncodeResult.Error);
	}

	if (Encoder != TEXT("frames_only") && !EncodeResult.bEncoded)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("GIF encoding failed: %s"), *EncodeResult.Error))
			.WithErrorData(Result);
	}

	return FMonolithActionResult::Success(Result);
}

// --- Automation tests ---
//
// `list_automation_tests` and `run_automation_tests` use the engine's automation
// framework (`FAutomationTestFramework`) to enumerate and execute tests inside the
// already-running editor process. No PIE, no commandlet, no second editor instance.
//
// `run_automation_tests` only handles tests that complete synchronously inside
// `StartTestByName + StopTest` (which is the case for SimpleAutomationTest macros).
// Latent / async tests (TickTests-driven) are skipped with a clear note so the
// caller knows they were not exercised.

namespace MonolithAutomationDetail
{
	static constexpr int32 AutomationHistoryCapacity = 20;

	static FString MakeAutomationRunId()
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
		return FString::Printf(TEXT("automation-%s-%s"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")), *ShortGuid);
	}

	static void CopyStringField(const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, TSharedPtr<FJsonObject>& Target)
	{
		FString Value;
		if (Source.IsValid() && Source->TryGetStringField(FieldName, Value))
		{
			Target->SetStringField(FieldName, Value);
		}
	}

	static void CopyNumberField(const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, TSharedPtr<FJsonObject>& Target)
	{
		double Value = 0.0;
		if (Source.IsValid() && Source->TryGetNumberField(FieldName, Value))
		{
			Target->SetNumberField(FieldName, Value);
		}
	}

	static void CopyBoolField(const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, TSharedPtr<FJsonObject>& Target)
	{
		bool bValue = false;
		if (Source.IsValid() && Source->TryGetBoolField(FieldName, bValue))
		{
			Target->SetBoolField(FieldName, bValue);
		}
	}

	static TSharedPtr<FJsonObject> BuildAutomationRunSummary(const TSharedPtr<FJsonObject>& Run)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		if (!Run.IsValid())
		{
			return Summary;
		}

		CopyStringField(Run, TEXT("run_id"), Summary);
		CopyStringField(Run, TEXT("prefix"), Summary);
		CopyStringField(Run, TEXT("state"), Summary);
		CopyStringField(Run, TEXT("completion_reason"), Summary);
		CopyStringField(Run, TEXT("started_at"), Summary);
		CopyStringField(Run, TEXT("completed_at"), Summary);
		CopyStringField(Run, TEXT("message"), Summary);
		CopyStringField(Run, TEXT("stop_status"), Summary);
		CopyBoolField(Run, TEXT("success"), Summary);
		CopyBoolField(Run, TEXT("can_stop"), Summary);
		CopyNumberField(Run, TEXT("matched"), Summary);
		CopyNumberField(Run, TEXT("max_tests"), Summary);
		CopyNumberField(Run, TEXT("requested_tests"), Summary);
		CopyNumberField(Run, TEXT("completed_tests"), Summary);
		CopyNumberField(Run, TEXT("total"), Summary);
		CopyNumberField(Run, TEXT("passed"), Summary);
		CopyNumberField(Run, TEXT("failed"), Summary);
		CopyNumberField(Run, TEXT("skipped"), Summary);
		CopyNumberField(Run, TEXT("progress"), Summary);
		CopyNumberField(Run, TEXT("truncated_remaining"), Summary);

		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (Run->TryGetArrayField(TEXT("results"), Results))
		{
			Summary->SetNumberField(TEXT("result_count"), Results->Num());
		}

		return Summary;
	}

	static FString GetTestFullPath(const FAutomationTestInfo& Info)
	{
#if ENGINE_MAJOR_VERSION >= 5
		return Info.GetFullTestPath();
#else
		return Info.GetTestName();
#endif
	}

	static TSharedPtr<FJsonObject> TestInfoToJson(const FAutomationTestInfo& Info)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("full_path"), GetTestFullPath(Info));
		Obj->SetStringField(TEXT("display_name"), Info.GetDisplayName());
		Obj->SetStringField(TEXT("test_name"), Info.GetTestName());
		Obj->SetNumberField(TEXT("flags"), static_cast<double>(static_cast<uint32>(Info.GetTestFlags())));
		return Obj;
	}

	static bool MatchesQuery(const FAutomationTestInfo& Info, const FString& Query)
	{
		if (Query.IsEmpty())
		{
			return true;
		}

		const FString QueryLower = Query.ToLower();
		return GetTestFullPath(Info).ToLower().Contains(QueryLower)
			|| Info.GetDisplayName().ToLower().Contains(QueryLower)
			|| Info.GetTestName().ToLower().Contains(QueryLower);
	}

	static void IncrementFlagBucket(const FAutomationTestInfo& Info, const EAutomationTestFlags Flag, const TCHAR* FieldName, TSharedPtr<FJsonObject>& Counts)
	{
		const uint32 Flags = static_cast<uint32>(Info.GetTestFlags());
		if ((Flags & static_cast<uint32>(Flag)) != 0)
		{
			double Existing = 0.0;
			Counts->TryGetNumberField(FieldName, Existing);
			Counts->SetNumberField(FieldName, Existing + 1.0);
		}
	}

	static TSharedPtr<FJsonObject> BuildFlagSummary(const TArray<FAutomationTestInfo>& Tests)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("smoke"), 0);
		Counts->SetNumberField(TEXT("engine"), 0);
		Counts->SetNumberField(TEXT("product"), 0);
		Counts->SetNumberField(TEXT("performance"), 0);
		Counts->SetNumberField(TEXT("stress"), 0);
		Counts->SetNumberField(TEXT("negative"), 0);

		for (const FAutomationTestInfo& Info : Tests)
		{
			IncrementFlagBucket(Info, EAutomationTestFlags::SmokeFilter, TEXT("smoke"), Counts);
			IncrementFlagBucket(Info, EAutomationTestFlags::EngineFilter, TEXT("engine"), Counts);
			IncrementFlagBucket(Info, EAutomationTestFlags::ProductFilter, TEXT("product"), Counts);
			IncrementFlagBucket(Info, EAutomationTestFlags::PerfFilter, TEXT("performance"), Counts);
			IncrementFlagBucket(Info, EAutomationTestFlags::StressFilter, TEXT("stress"), Counts);
			IncrementFlagBucket(Info, EAutomationTestFlags::NegativeFilter, TEXT("negative"), Counts);
		}

		return Counts;
	}

	static bool ResolveAutomationReportDirectory(const FString& OutputDirParam, FString& OutDir, FString& OutError)
	{
		FString SavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		FPaths::NormalizeDirectoryName(SavedDir);

		FString RequestedDir = OutputDirParam.TrimStartAndEnd();
		if (RequestedDir.IsEmpty())
		{
			RequestedDir = FPaths::Combine(SavedDir, TEXT("Monolith"), TEXT("AutomationReports"));
		}
		else if (FPaths::IsRelative(RequestedDir))
		{
			RequestedDir = FPaths::Combine(SavedDir, RequestedDir);
		}

		RequestedDir = FPaths::ConvertRelativePathToFull(RequestedDir);
		FPaths::NormalizeDirectoryName(RequestedDir);

		if (!RequestedDir.Equals(SavedDir, ESearchCase::IgnoreCase)
			&& !FPaths::IsUnderDirectory(RequestedDir, SavedDir))
		{
			OutError = FString::Printf(TEXT("output_dir must resolve under Project/Saved. Requested: %s"), *RequestedDir);
			return false;
		}

		if (!IFileManager::Get().MakeDirectory(*RequestedDir, true))
		{
			OutError = FString::Printf(TEXT("Failed to create output_dir: %s"), *RequestedDir);
			return false;
		}

		OutDir = RequestedDir;
		return true;
	}

	static bool JsonObjectToString(const TSharedPtr<FJsonObject>& Object, FString& OutString)
	{
		if (!Object.IsValid())
		{
			return false;
		}

		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
		return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}

	static void CollectMatchingTests(const FString& Prefix, TArray<FAutomationTestInfo>& OutTests)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

		// Force-load latest test list (covers tests added since the editor started).
		Framework.LoadTestModules();

		// Default RequestedTestFilter is SmokeFilter only (UE constructor default), which
		// excludes most game-module tests. Widen to all filter buckets so any registered
		// test the caller's prefix points at is eligible. Restore on scope exit.
		const EAutomationTestFlags AllFilters = static_cast<EAutomationTestFlags>(
			static_cast<uint32>(EAutomationTestFlags::SmokeFilter) |
			static_cast<uint32>(EAutomationTestFlags::EngineFilter) |
			static_cast<uint32>(EAutomationTestFlags::ProductFilter) |
			static_cast<uint32>(EAutomationTestFlags::PerfFilter) |
			static_cast<uint32>(EAutomationTestFlags::StressFilter) |
			static_cast<uint32>(EAutomationTestFlags::NegativeFilter));
		// No public getter for the previous filter, so just set ours and leave it.
		// Subsequent test runs in the same session pick up this widened filter, which
		// is harmless (other tools will set their own when they need it).
		Framework.SetRequestedTestFilter(AllFilters);

		TArray<FAutomationTestInfo> AllTests;
		Framework.GetValidTestNames(AllTests);

		for (const FAutomationTestInfo& Info : AllTests)
		{
			const FString FullPath = GetTestFullPath(Info);
			if (Prefix.IsEmpty() || FullPath.StartsWith(Prefix))
			{
				OutTests.Add(Info);
			}
		}
	}
}

// --- Scripting actions (HOFF 7) ---

namespace
{
	const TCHAR* PythonLogTypeToString(EPythonLogOutputType T)
	{
		switch (T)
		{
		case EPythonLogOutputType::Info:    return TEXT("info");
		case EPythonLogOutputType::Warning: return TEXT("warning");
		case EPythonLogOutputType::Error:   return TEXT("error");
		default:                            return TEXT("info");
		}
	}
}

FMonolithActionResult FMonolithEditorActions::HandleListAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	int32 Limit = 500;
	int32 Offset = 0;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("prefix"), Prefix);

		double LimitNum = static_cast<double>(Limit);
		if (Params->HasField(TEXT("limit")))
		{
			if (!Params->TryGetNumberField(TEXT("limit"), LimitNum))
			{
				return FMonolithActionResult::Error(TEXT("limit must be a number"));
			}
			Limit = FMath::Clamp(FMath::FloorToInt(LimitNum), 1, 5000);
		}

		double OffsetNum = 0.0;
		if (Params->TryGetNumberField(TEXT("offset"), OffsetNum) && OffsetNum > 0)
		{
			Offset = FMath::FloorToInt(OffsetNum);
		}
		int32 CursorOffset = 0;
		FString CursorError;
		if (!FMonolithProjectionUtils::ReadCursorOffset(Params, CursorOffset, CursorError))
		{
			return FMonolithActionResult::Error(CursorError);
		}
		if (CursorOffset > 0)
		{
			Offset = CursorOffset;
		}
	}

	TArray<FAutomationTestInfo> Tests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, Tests);

	const int32 Total = Tests.Num();
	const int32 EffectiveOffset = FMath::Min(Offset, Total);
	const int32 Count = FMath::Min(Limit, Total - EffectiveOffset);
	TArray<TSharedPtr<FJsonValue>> TestsJson;
	TestsJson.Reserve(Count);
	for (int32 Index = EffectiveOffset; Index < EffectiveOffset + Count; ++Index)
	{
		TestsJson.Add(MakeShared<FJsonValueObject>(MonolithAutomationDetail::TestInfoToJson(Tests[Index])));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("count"), Total);
	Result->SetArrayField(TEXT("tests"), TestsJson);
	// Common list-projection contract (additive next to the legacy count field).
	Result->SetNumberField(TEXT("total"), Total);
	Result->SetNumberField(TEXT("returned"), TestsJson.Num());
	Result->SetBoolField(TEXT("truncated"), Total > EffectiveOffset + TestsJson.Num());
	if (EffectiveOffset + TestsJson.Num() < Total)
	{
		Result->SetStringField(TEXT("next_cursor"), FString::FromInt(EffectiveOffset + TestsJson.Num()));
	}
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("default_limit"), 500);
	Limits->SetNumberField(TEXT("max_limit"), 5000);
	Limits->SetNumberField(TEXT("offset"), EffectiveOffset);
	Result->SetObjectField(TEXT("limits"), Limits);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleFindAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	FString Query;
	int32 MaxResults = 100;

	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("prefix"), Prefix);
		Params->TryGetStringField(TEXT("query"), Query);

		double MaxNum = MaxResults;
		if (Params->HasField(TEXT("max_results")))
		{
			if (!Params->TryGetNumberField(TEXT("max_results"), MaxNum))
			{
				return FMonolithActionResult::Error(TEXT("max_results must be a number"));
			}
			MaxResults = FMath::Clamp(FMath::FloorToInt(MaxNum), 1, 1000);
		}
	}

	TArray<FAutomationTestInfo> Tests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, Tests);

	TArray<TSharedPtr<FJsonValue>> MatchesJson;
	int32 MatchedCount = 0;
	for (const FAutomationTestInfo& Info : Tests)
	{
		if (!MonolithAutomationDetail::MatchesQuery(Info, Query))
		{
			continue;
		}

		++MatchedCount;
		if (MatchesJson.Num() < MaxResults)
		{
			MatchesJson.Add(MakeShared<FJsonValueObject>(MonolithAutomationDetail::TestInfoToJson(Info)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetStringField(TEXT("query"), Query);
	Result->SetNumberField(TEXT("matched_count"), MatchedCount);
	Result->SetNumberField(TEXT("returned_count"), MatchesJson.Num());
	Result->SetArrayField(TEXT("tests"), MatchesJson);
	if (MatchedCount > MatchesJson.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), MatchedCount - MatchesJson.Num());
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleGetAutomationRunStatus(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bActive = bAutomationRunActive && CurrentAutomationRun.IsValid();

	Result->SetBoolField(TEXT("active"), bActive);
	Result->SetBoolField(TEXT("can_stop"), false);
	Result->SetStringField(TEXT("stop_status"), TEXT("unsupported_cancel"));
	Result->SetStringField(TEXT("runner_mode"), TEXT("synchronous"));
	Result->SetNumberField(TEXT("history_count"), AutomationRunHistory.Num());
	Result->SetNumberField(TEXT("history_capacity"), MonolithAutomationDetail::AutomationHistoryCapacity);

	if (bActive)
	{
		Result->SetObjectField(TEXT("current_run"), MonolithAutomationDetail::BuildAutomationRunSummary(CurrentAutomationRun));
	}

	Result->SetBoolField(TEXT("has_last_run"), LastAutomationRun.IsValid());
	if (LastAutomationRun.IsValid())
	{
		Result->SetStringField(TEXT("last_run_recorded_at"), TimestampToIso(LastAutomationRunTimestamp));
		Result->SetObjectField(TEXT("last_run"), MonolithAutomationDetail::BuildAutomationRunSummary(LastAutomationRun));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleStopAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	const bool bActive = bAutomationRunActive && CurrentAutomationRun.IsValid();

	Result->SetBoolField(TEXT("stopped"), false);
	Result->SetBoolField(TEXT("can_stop"), false);
	Result->SetStringField(TEXT("stop_status"), TEXT("unsupported_cancel"));
	Result->SetStringField(TEXT("runner_mode"), TEXT("synchronous"));
	Result->SetStringField(TEXT("state"), bActive ? TEXT("running") : TEXT("idle"));
	Result->SetStringField(TEXT("message"),
		TEXT("Monolith automation currently runs synchronously with StartTestByName + StopTest; cancellation is not supported for in-flight runs."));

	if (bActive)
	{
		Result->SetObjectField(TEXT("current_run"), MonolithAutomationDetail::BuildAutomationRunSummary(CurrentAutomationRun));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleListAutomationHistory(const TSharedPtr<FJsonObject>& Params)
{
	int32 MaxResults = MonolithAutomationDetail::AutomationHistoryCapacity;
	if (Params.IsValid())
	{
		double MaxNum = MaxResults;
		if (Params->HasField(TEXT("max_results")))
		{
			if (!Params->TryGetNumberField(TEXT("max_results"), MaxNum))
			{
				return FMonolithActionResult::Error(TEXT("max_results must be a number"));
			}
			MaxResults = FMath::Clamp(FMath::FloorToInt(MaxNum), 1, MonolithAutomationDetail::AutomationHistoryCapacity);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	const int32 Count = FMath::Min(MaxResults, AutomationRunHistory.Num());
	Rows.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Rows.Add(MakeShared<FJsonValueObject>(AutomationRunHistory[Index]));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetNumberField(TEXT("history_count"), AutomationRunHistory.Num());
	Result->SetNumberField(TEXT("history_capacity"), MonolithAutomationDetail::AutomationHistoryCapacity);
	Result->SetArrayField(TEXT("runs"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleGetAutomationSummary(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("prefix"), Prefix);
	}

	TArray<FAutomationTestInfo> Tests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, Tests);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("discovered_total"), Tests.Num());
	Result->SetObjectField(TEXT("flag_counts"), MonolithAutomationDetail::BuildFlagSummary(Tests));
	Result->SetBoolField(TEXT("active"), bAutomationRunActive && CurrentAutomationRun.IsValid());
	Result->SetNumberField(TEXT("history_count"), AutomationRunHistory.Num());
	Result->SetNumberField(TEXT("history_capacity"), MonolithAutomationDetail::AutomationHistoryCapacity);

	Result->SetBoolField(TEXT("has_last_run"), LastAutomationRun.IsValid());
	if (LastAutomationRun.IsValid())
	{
		Result->SetStringField(TEXT("last_run_recorded_at"), TimestampToIso(LastAutomationRunTimestamp));
		Result->SetObjectField(TEXT("last_run"), LastAutomationRun);
		Result->SetObjectField(TEXT("last_run_status"), MonolithAutomationDetail::BuildAutomationRunSummary(LastAutomationRun));
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleExportAutomationReport(const TSharedPtr<FJsonObject>& Params)
{
	if (!LastAutomationRun.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("No automation run has been recorded by editor.run_automation_tests in this editor session."));
	}

	FString OutputDirParam;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("output_dir"), OutputDirParam);
	}

	FString OutputDir;
	FString ResolveError;
	if (!MonolithAutomationDetail::ResolveAutomationReportDirectory(OutputDirParam, OutputDir, ResolveError))
	{
		return FMonolithActionResult::Error(ResolveError);
	}

	TSharedPtr<FJsonObject> ReportRoot = MakeShared<FJsonObject>();
	ReportRoot->SetStringField(TEXT("kind"), TEXT("monolith.automation_report"));
	ReportRoot->SetStringField(TEXT("exported_at"), FDateTime::UtcNow().ToIso8601());
	ReportRoot->SetStringField(TEXT("last_run_recorded_at"), TimestampToIso(LastAutomationRunTimestamp));
	ReportRoot->SetObjectField(TEXT("last_run"), LastAutomationRun);

	FString ReportJson;
	if (!MonolithAutomationDetail::JsonObjectToString(ReportRoot, ReportJson))
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize automation report JSON."));
	}

	const FString FileName = FString::Printf(TEXT("AutomationReport_%s.json"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")));
	const FString OutputPath = FPaths::Combine(OutputDir, FileName);
	if (!FFileHelper::SaveStringToFile(ReportJson, *OutputPath))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to write automation report: %s"), *OutputPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), OutputPath);
	Result->SetNumberField(TEXT("bytes"), ReportJson.Len());
	Result->SetStringField(TEXT("kind"), TEXT("monolith.automation_report"));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleRunAutomationTests(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("prefix"), Prefix) || Prefix.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required parameter: prefix (string, e.g. 'MazeLegends.Bow')"));
	}

	int32 MaxTests = 200;
	if (Params.IsValid())
	{
		double MaxNum = MaxTests;
		if (Params->HasField(TEXT("max_tests")))
		{
			if (!Params->TryGetNumberField(TEXT("max_tests"), MaxNum))
			{
				return FMonolithActionResult::Error(TEXT("max_tests must be a number"));
			}
			MaxTests = FMath::Clamp(FMath::FloorToInt(MaxNum), 1, 1000);
		}
	}

	TArray<FAutomationTestInfo> MatchingTests;
	MonolithAutomationDetail::CollectMatchingTests(Prefix, MatchingTests);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("run_id"), MonolithAutomationDetail::MakeAutomationRunId());
	Result->SetStringField(TEXT("started_at"), FDateTime::UtcNow().ToIso8601());
	Result->SetStringField(TEXT("prefix"), Prefix);
	Result->SetNumberField(TEXT("matched"), MatchingTests.Num());
	Result->SetNumberField(TEXT("max_tests"), MaxTests);
	Result->SetBoolField(TEXT("can_stop"), false);
	Result->SetStringField(TEXT("stop_status"), TEXT("unsupported_cancel"));
	Result->SetStringField(TEXT("runner_mode"), TEXT("synchronous"));
	Result->SetNumberField(TEXT("history_capacity"), MonolithAutomationDetail::AutomationHistoryCapacity);
	Result->SetStringField(TEXT("state"), TEXT("pending"));
	Result->SetNumberField(TEXT("requested_tests"), 0);
	Result->SetNumberField(TEXT("completed_tests"), 0);
	Result->SetNumberField(TEXT("progress"), 0.0);

	auto RecordFinishedRun = [Result]()
	{
		LastAutomationRun = Result;
		LastAutomationRunTimestamp = FPlatformTime::Seconds();

		TSharedPtr<FJsonObject> HistoryEntry = MonolithAutomationDetail::BuildAutomationRunSummary(Result);
		HistoryEntry->SetStringField(TEXT("recorded_at"), TimestampToIso(LastAutomationRunTimestamp));
		AutomationRunHistory.Insert(HistoryEntry, 0);
		while (AutomationRunHistory.Num() > MonolithAutomationDetail::AutomationHistoryCapacity)
		{
			AutomationRunHistory.RemoveAt(AutomationRunHistory.Num() - 1);
		}

		CurrentAutomationRun.Reset();
		bAutomationRunActive = false;
	};

	if (MatchingTests.Num() == 0)
	{
		Result->SetStringField(TEXT("state"), TEXT("completed"));
		Result->SetStringField(TEXT("completion_reason"), TEXT("no_matching_tests"));
		Result->SetBoolField(TEXT("success"), false);
		Result->SetNumberField(TEXT("total"), 0);
		Result->SetNumberField(TEXT("passed"), 0);
		Result->SetNumberField(TEXT("failed"), 0);
		Result->SetNumberField(TEXT("skipped"), 0);
		Result->SetNumberField(TEXT("progress"), 1.0);
		Result->SetStringField(TEXT("message"),
			FString::Printf(TEXT("No tests matching prefix '%s' (call list_automation_tests for available tests)"), *Prefix));
		Result->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
		RecordFinishedRun();
		return FMonolithActionResult::Success(Result);
	}

	const int32 TestsToRun = FMath::Min(MaxTests, MatchingTests.Num());
	Result->SetStringField(TEXT("state"), TEXT("running"));
	Result->SetNumberField(TEXT("requested_tests"), TestsToRun);
	Result->SetNumberField(TEXT("total"), TestsToRun);
	Result->SetNumberField(TEXT("passed"), 0);
	Result->SetNumberField(TEXT("failed"), 0);
	Result->SetNumberField(TEXT("skipped"), 0);
	CurrentAutomationRun = Result;
	bAutomationRunActive = true;

	FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

	TArray<TSharedPtr<FJsonValue>> ResultsJson;
	int32 Passed = 0;
	int32 Failed = 0;
	int32 Skipped = 0;

	for (int32 i = 0; i < TestsToRun; ++i)
	{
		const FAutomationTestInfo& Info = MatchingTests[i];
		const FString FullPath = MonolithAutomationDetail::GetTestFullPath(Info);
		// StartTestByName looks up by the class-name registry key (e.g. FBowDataAssetTest),
		// NOT the human-readable full path. Passing FullPath fails silently and leaves
		// GIsAutomationTesting=false, which trips an assertion when StopTest is called.
		const FString TestKey = Info.GetTestName();

		TSharedPtr<FJsonObject> TestResult = MakeShared<FJsonObject>();
		TestResult->SetStringField(TEXT("full_path"), FullPath);
		TestResult->SetStringField(TEXT("test_name"), TestKey);
		TestResult->SetStringField(TEXT("display_name"), Info.GetDisplayName());
		TestResult->SetNumberField(TEXT("flags"), static_cast<double>(static_cast<uint32>(Info.GetTestFlags())));

		if (!Framework.ContainsTest(TestKey))
		{
			TestResult->SetStringField(TEXT("status"), TEXT("skipped"));
			TestResult->SetStringField(TEXT("reason"),
				FString::Printf(TEXT("ContainsTest('%s') returned false (registry lookup failed)"), *TestKey));
			Skipped++;
			ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
			Result->SetNumberField(TEXT("completed_tests"), i + 1);
			Result->SetNumberField(TEXT("skipped"), Skipped);
			Result->SetNumberField(TEXT("progress"), static_cast<double>(i + 1) / static_cast<double>(TestsToRun));
			continue;
		}

		Framework.StartTestByName(TestKey, /*RoleIndex=*/0, FullPath);

		FAutomationTestExecutionInfo ExecInfo;
		const bool bCompleted = Framework.StopTest(ExecInfo);
		const bool bSuccess = bCompleted && (ExecInfo.GetErrorTotal() == 0);

		TestResult->SetStringField(TEXT("status"), bSuccess ? TEXT("passed") : TEXT("failed"));
		TestResult->SetNumberField(TEXT("duration_seconds"), ExecInfo.Duration);
		TestResult->SetNumberField(TEXT("error_count"), ExecInfo.GetErrorTotal());
		TestResult->SetNumberField(TEXT("warning_count"), ExecInfo.GetWarningTotal());

		TArray<TSharedPtr<FJsonValue>> ErrorsJson;
		TArray<TSharedPtr<FJsonValue>> WarningsJson;
		TArray<TSharedPtr<FJsonValue>> LogSnippetsJson;
		const int32 NumEntries = ExecInfo.GetEntries().Num();
		ErrorsJson.Reserve(NumEntries);
		WarningsJson.Reserve(NumEntries);
		LogSnippetsJson.Reserve(NumEntries);
		for (const FAutomationExecutionEntry& Entry : ExecInfo.GetEntries())
		{
			if (Entry.Event.Type == EAutomationEventType::Error)
			{
				ErrorsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
			}
			else if (Entry.Event.Type == EAutomationEventType::Warning)
			{
				WarningsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
			}
			else if (LogSnippetsJson.Num() < 20)
			{
				LogSnippetsJson.Add(MakeShared<FJsonValueString>(Entry.Event.Message));
			}
		}
		if (ErrorsJson.Num() > 0)
		{
			TestResult->SetArrayField(TEXT("errors"), ErrorsJson);
		}
		if (WarningsJson.Num() > 0)
		{
			TestResult->SetArrayField(TEXT("warnings"), WarningsJson);
		}
		if (LogSnippetsJson.Num() > 0)
		{
			TestResult->SetArrayField(TEXT("log_snippets"), LogSnippetsJson);
		}

		if (bSuccess) Passed++; else Failed++;
		ResultsJson.Add(MakeShared<FJsonValueObject>(TestResult));
		Result->SetNumberField(TEXT("completed_tests"), i + 1);
		Result->SetNumberField(TEXT("passed"), Passed);
		Result->SetNumberField(TEXT("failed"), Failed);
		Result->SetNumberField(TEXT("skipped"), Skipped);
		Result->SetNumberField(TEXT("progress"), static_cast<double>(i + 1) / static_cast<double>(TestsToRun));
	}

	Result->SetStringField(TEXT("completed_at"), FDateTime::UtcNow().ToIso8601());
	Result->SetStringField(TEXT("state"), TEXT("completed"));
	Result->SetStringField(TEXT("completion_reason"), TEXT("finished"));
	Result->SetBoolField(TEXT("success"), Failed == 0);
	Result->SetNumberField(TEXT("total"), TestsToRun);
	Result->SetNumberField(TEXT("passed"), Passed);
	Result->SetNumberField(TEXT("failed"), Failed);
	Result->SetNumberField(TEXT("skipped"), Skipped);
	Result->SetNumberField(TEXT("completed_tests"), TestsToRun);
	Result->SetNumberField(TEXT("progress"), 1.0);
	Result->SetArrayField(TEXT("results"), ResultsJson);

	if (MatchingTests.Num() > TestsToRun)
	{
		Result->SetNumberField(TEXT("truncated_remaining"), MatchingTests.Num() - TestsToRun);
	}

	RecordFinishedRun();
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleRunPython(const TSharedPtr<FJsonObject>& Params)
{
	FString Command;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
	{
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("code"), Command);
			if (Command.IsEmpty())
			{
				Params->TryGetStringField(TEXT("script"), Command);
			}
		}
	}
	if (Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: command (aliases: code, script)"));
	}

	FString ModeStr = TEXT("execute_file");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	EPythonCommandExecutionMode Mode = EPythonCommandExecutionMode::ExecuteFile;
	if (ModeStr.Equals(TEXT("execute_file"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::ExecuteFile;
	}
	else if (ModeStr.Equals(TEXT("execute_statement"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::ExecuteStatement;
	}
	else if (ModeStr.Equals(TEXT("evaluate_statement"), ESearchCase::IgnoreCase))
	{
		Mode = EPythonCommandExecutionMode::EvaluateStatement;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid mode '%s'. Valid: execute_file, execute_statement, evaluate_statement."), *ModeStr));
	}

	bool bUnattended = false;
	Params->TryGetBoolField(TEXT("unattended"), bUnattended);

	FString ScopeStr = TEXT("private");
	Params->TryGetStringField(TEXT("file_scope"), ScopeStr);
	EPythonFileExecutionScope FileScope = EPythonFileExecutionScope::Private;
	if (ScopeStr.Equals(TEXT("private"), ESearchCase::IgnoreCase))
	{
		FileScope = EPythonFileExecutionScope::Private;
	}
	else if (ScopeStr.Equals(TEXT("public"), ESearchCase::IgnoreCase))
	{
		FileScope = EPythonFileExecutionScope::Public;
	}
	else
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Invalid file_scope '%s'. Valid: private, public."), *ScopeStr));
	}

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python)
	{
		// Fallback: load PythonScriptPlugin if it has not been brought up yet.
		FModuleManager::Get().LoadModule(TEXT("PythonScriptPlugin"));
		Python = IPythonScriptPlugin::Get();
	}
	if (!Python)
	{
		return FMonolithActionResult::Error(
			TEXT("PythonScriptPlugin module is not available. Enable PythonScriptPlugin in the project's plugins list."));
	}
	if (!Python->IsPythonAvailable())
	{
		return FMonolithActionResult::Error(
			TEXT("Python is not available in this build (IPythonScriptPlugin::IsPythonAvailable() returned false)."));
	}

	FPythonCommandEx Cmd;
	Cmd.Command = Command;
	Cmd.ExecutionMode = Mode;
	Cmd.FileExecutionScope = FileScope;
	Cmd.Flags = bUnattended ? EPythonCommandFlags::Unattended : EPythonCommandFlags::None;

	const bool bOk = Python->ExecPythonCommandEx(Cmd);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bOk);
	Result->SetBoolField(TEXT("success"), bOk);
	Result->SetStringField(TEXT("mode"), ModeStr);
	Result->SetStringField(TEXT("result"), Cmd.CommandResult);

	TArray<TSharedPtr<FJsonValue>> OutputRows;
	OutputRows.Reserve(Cmd.LogOutput.Num());
	for (const FPythonLogOutputEntry& Entry : Cmd.LogOutput)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("type"), PythonLogTypeToString(Entry.Type));
		Row->SetStringField(TEXT("output"), Entry.Output);
		OutputRows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("output"), OutputRows);

	if (!bOk)
	{
		// On failure CommandResult typically holds the Python exception trace.
		// Surface as message so callers don't have to special-case it.
		Result->SetStringField(TEXT("message"), Cmd.CommandResult);
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleLoadLevel(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("level_path"), Path);
		}
	}
	if (Path.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("GEditor is null — load_level requires editor context."));
	}

	ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEd)
	{
		return FMonolithActionResult::Error(TEXT("ULevelEditorSubsystem is unavailable."));
	}

	// #6 world-leak guard: never LoadLevel while a PIE world is still resident — the
	// deferred EndPlayMap teardown would assert "World Memory Leaks" in EditorDestroyWorld.
	FString PieGuardError;
	if (!EnsureNoResidentPieWorldBeforeMapLoad(PieGuardError))
	{
		return FMonolithActionResult::Error(PieGuardError);
	}

	const bool bLoaded = LevelEd->LoadLevel(Path);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), bLoaded);
	Result->SetBoolField(TEXT("loaded"), bLoaded);
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("message"),
		bLoaded
			? FString::Printf(TEXT("Loaded level '%s'."), *Path)
			: FString::Printf(TEXT("ULevelEditorSubsystem::LoadLevel returned false for '%s'. Verify the asset exists and is a UWorld."), *Path));

	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F1: list_dirty_packages / save_packages — scoped dirty report + scoped saver
// (PIE/profiling harness plan 2026-06-04)
// ---------------------------------------------------------------------------

namespace MonolithEditorPackages
{
	// Read scope_paths into a prefix list. Returns true if any prefixes were given.
	static bool ParseScopePaths(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutPrefixes)
	{
		OutPrefixes.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("scope_paths"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString Prefix;
				if (Val.IsValid() && Val->TryGetString(Prefix) && !Prefix.IsEmpty())
				{
					OutPrefixes.Add(Prefix);
				}
			}
		}
		return OutPrefixes.Num() > 0;
	}

	static bool MatchesScope(const FString& PackageName, const TArray<FString>& Prefixes)
	{
		if (Prefixes.Num() == 0)
		{
			return true;
		}
		for (const FString& Prefix : Prefixes)
		{
			if (PackageName.StartsWith(Prefix))
			{
				return true;
			}
		}
		return false;
	}

	// A package backs disk content if it has a real /Game (or other mount) name and
	// is not the transient package. Filters out /Engine/Transient and GC-only objects.
	static bool IsDiskPackage(const UPackage* Package)
	{
		if (!Package || Package == GetTransientPackage())
		{
			return false;
		}
		if (Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_PlayInEditor))
		{
			return false;
		}
		const FString Name = Package->GetName();
		return Name.StartsWith(TEXT("/")) && !Name.StartsWith(TEXT("/Temp/")) &&
			FPackageName::IsValidLongPackageName(Name);
	}

	// Collect dirty, on-disk packages matching the scope (and include_transient flag).
	static void CollectDirtyPackages(const TArray<FString>& ScopePrefixes, bool bIncludeTransient,
		TArray<UPackage*>& OutPackages)
	{
		OutPackages.Reset();
		ForEachObjectOfClass(UPackage::StaticClass(), [&](UObject* Obj)
		{
			UPackage* Package = Cast<UPackage>(Obj);
			if (!Package || !Package->IsDirty())
			{
				return;
			}
			if (!bIncludeTransient && !IsDiskPackage(Package))
			{
				return;
			}
			if (!MatchesScope(Package->GetName(), ScopePrefixes))
			{
				return;
			}
			OutPackages.Add(Package);
		}, /*bIncludeDerivedClasses=*/false);
	}
}

FMonolithActionResult FMonolithEditorActions::HandleListDirtyPackages(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPackages;

	TArray<FString> ScopePrefixes;
	ParseScopePaths(Params, ScopePrefixes);

	bool bIncludeTransient = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("include_transient"), bIncludeTransient); }

	// Map-vs-content filters default to true (report both kinds) per the F1 plan.
	bool bIncludeMaps = true;
	bool bIncludeContent = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_maps"), bIncludeMaps);
		Params->TryGetBoolField(TEXT("include_content"), bIncludeContent);
	}

	TArray<UPackage*> DirtyPackages;
	CollectDirtyPackages(ScopePrefixes, bIncludeTransient, DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (UPackage* Package : DirtyPackages)
	{
		const FString PackageName = Package->GetName();
		const bool bIsMap = Package->ContainsMap();
		const bool bIsDisk = IsDiskPackage(Package);

		// Apply the map/content filters before emitting the row.
		if (bIsMap && !bIncludeMaps)
		{
			continue;
		}
		if (!bIsMap && !bIncludeContent)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);
		Row->SetBoolField(TEXT("is_map"), bIsMap);
		Row->SetBoolField(TEXT("transient"), !bIsDisk);
		if (bIsDisk)
		{
			const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension()
									   : FPackageName::GetAssetPackageExtension();
			FString Filename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Ext))
			{
				Row->SetStringField(TEXT("disk_path"), Filename);
			}
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("dirty_packages"), Rows);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleSavePackages(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPackages;

	const TArray<TSharedPtr<FJsonValue>>* PkgArr = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("packages"), PkgArr) || !PkgArr || PkgArr->Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: packages (non-empty array of long package names)"));
	}

	TArray<FString> RequestedNames;
	for (const TSharedPtr<FJsonValue>& Val : *PkgArr)
	{
		FString Name;
		if (Val.IsValid() && Val->TryGetString(Name) && !Name.IsEmpty())
		{
			RequestedNames.AddUnique(Name);
		}
	}
	if (RequestedNames.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("packages array contained no valid package names"));
	}

	bool bFailOnUnrequested = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("fail_on_unrequested_dirty"), bFailOnUnrequested); }

	bool bDryRun = false;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("dry_run"), bDryRun); }

	// Pre-scan: when requested, abort before saving anything if a dirty package
	// exists outside the request set (bounded by scope_paths if given).
	if (bFailOnUnrequested)
	{
		TArray<FString> ScopePrefixes;
		ParseScopePaths(Params, ScopePrefixes);

		TArray<UPackage*> DirtyPackages;
		CollectDirtyPackages(ScopePrefixes, /*bIncludeTransient=*/false, DirtyPackages);

		TArray<FString> Unrequested;
		for (UPackage* Package : DirtyPackages)
		{
			if (!RequestedNames.Contains(Package->GetName()))
			{
				Unrequested.Add(Package->GetName());
			}
		}
		if (Unrequested.Num() > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("fail_on_unrequested_dirty: %d dirty package(s) outside the request set: %s"),
				Unrequested.Num(), *FString::Join(Unrequested, TEXT(", "))));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 SavedCount = 0;
	for (const FString& PackageName : RequestedNames)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("package"), PackageName);

		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		}
		if (!Package)
		{
			Row->SetBoolField(TEXT("saved"), false);
			Row->SetStringField(TEXT("error"), TEXT("package not found / could not be loaded"));
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		const bool bIsMap = Package->ContainsMap();
		const FString Ext = bIsMap ? FPackageName::GetMapPackageExtension()
								   : FPackageName::GetAssetPackageExtension();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, Ext);

		Row->SetBoolField(TEXT("is_map"), bIsMap);
		Row->SetStringField(TEXT("disk_path"), Filename);

		if (bDryRun)
		{
			// Report intent only — nothing is written. A package "would save" if it is
			// currently dirty; clean packages are reported as no-op.
			const bool bWouldSave = Package->IsDirty();
			Row->SetBoolField(TEXT("would_save"), bWouldSave);
			Row->SetBoolField(TEXT("dirty"), bWouldSave);
			if (bWouldSave) { ++SavedCount; }
			Rows.Add(MakeShared<FJsonValueObject>(Row));
			continue;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);

		Row->SetBoolField(TEXT("saved"), bSaved);
		if (bSaved) { ++SavedCount; }
		else { Row->SetStringField(TEXT("error"), TEXT("UPackage::SavePackage returned false")); }
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), SavedCount == RequestedNames.Num());
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(bDryRun ? TEXT("would_save") : TEXT("saved"), SavedCount);
	Result->SetNumberField(TEXT("requested"), RequestedNames.Num());
	Result->SetArrayField(TEXT("results"), Rows);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F2/F3 shared PIE-smoke scaffolding (PIE/profiling harness plan 2026-06-04)
// ---------------------------------------------------------------------------

namespace MonolithEditorPieSmoke
{
	// Default post-marker patterns every smoke counts (case-insensitive substring).
	static const TCHAR* DefaultPatterns[] =
	{
		TEXT("Blueprint Runtime Error"),
		TEXT("Accessed None"),
		TEXT("LogChooser"),
	};

	// Read a JSON array field into a string list (skips empty / non-string entries).
	static void ReadStringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field,
		TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj.IsValid() && Obj->TryGetArrayField(Field, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString P;
				if (Val.IsValid() && Val->TryGetString(P) && !P.IsEmpty())
				{
					Out.AddUnique(P);
				}
			}
		}
	}

	// #3 Resolve grouped log patterns. The DefaultPatterns always seed must_absent.
	// BACK-COMPAT: log_patterns may be a flat ARRAY (legacy) — treated as must_absent —
	// OR an OBJECT { must_absent, must_present, observe_only, warn } (new).
	static FPieSmokeLogGroups ResolveLogGroups(const TSharedPtr<FJsonObject>& Params)
	{
		FPieSmokeLogGroups Groups;
		for (const TCHAR* P : DefaultPatterns)
		{
			Groups.MustAbsent.AddUnique(P);
		}

		if (!Params.IsValid())
		{
			return Groups;
		}

		// Grouped object form takes precedence when log_patterns is an object.
		const TSharedPtr<FJsonObject>* GroupObj = nullptr;
		if (Params->TryGetObjectField(TEXT("log_patterns"), GroupObj) && GroupObj && GroupObj->IsValid())
		{
			ReadStringArrayField(*GroupObj, TEXT("must_absent"),  Groups.MustAbsent);
			ReadStringArrayField(*GroupObj, TEXT("must_present"), Groups.MustPresent);
			ReadStringArrayField(*GroupObj, TEXT("observe_only"), Groups.ObserveOnly);
			ReadStringArrayField(*GroupObj, TEXT("warn"),         Groups.Warn);
			return Groups;
		}

		// Legacy flat array form: every pattern is a must_absent.
		ReadStringArrayField(Params, TEXT("log_patterns"), Groups.MustAbsent);
		return Groups;
	}

	// Flatten the grouped patterns into the legacy LogPatterns list (used by the
	// legacy post_marker_counts / total_matches report fields for back-compat).
	static TArray<FString> FlattenGroups(const FPieSmokeLogGroups& Groups)
	{
		TArray<FString> All;
		for (const FString& P : Groups.MustAbsent)  { All.AddUnique(P); }
		for (const FString& P : Groups.MustPresent) { All.AddUnique(P); }
		for (const FString& P : Groups.ObserveOnly) { All.AddUnique(P); }
		for (const FString& P : Groups.Warn)        { All.AddUnique(P); }
		return All;
	}

	// True if a captured log entry matches the substring pattern (message OR category).
	static bool EntryMatches(const FMonolithLogEntry& Entry, const FString& Pattern)
	{
		return Entry.Message.Contains(Pattern, ESearchCase::IgnoreCase) ||
			Entry.Category.ToString().Contains(Pattern, ESearchCase::IgnoreCase);
	}

	// Per-pattern match counts within one bucket of entries. Returns the JSON
	// {pattern: count} object and accumulates the group total.
	static TSharedPtr<FJsonObject> CountGroupInBucket(const TArray<FMonolithLogEntry>& Bucket,
		const TArray<FString>& Patterns, int32& OutGroupTotal, int32& OutDistinctMatched)
	{
		OutGroupTotal = 0;
		OutDistinctMatched = 0;
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		for (const FString& Pattern : Patterns)
		{
			int32 Count = 0;
			for (const FMonolithLogEntry& Entry : Bucket)
			{
				if (EntryMatches(Entry, Pattern)) { ++Count; }
			}
			Counts->SetNumberField(Pattern, Count);
			OutGroupTotal += Count;
			if (Count > 0) { ++OutDistinctMatched; }
		}
		return Counts;
	}

	// #3 + #10 analyse post-marker log entries against the grouped patterns, split into
	// active-runtime vs teardown buckets at the first IgnoreAfterPattern hit. Fills the
	// report object's grouped count fields and returns the computed ok value (also
	// surfacing any warn-group hits + the legacy total_matches for back-compat).
	static bool AnalysePostMarkerGrouped(FMonolithLogCapture* LogCapture, double MarkerTimestamp,
		const FPieSmokeSession& S, TSharedPtr<FJsonObject>& Report)
	{
		TArray<FMonolithLogEntry> ActiveBucket;
		TArray<FMonolithLogEntry> TeardownBucket;

		if (LogCapture)
		{
			const TArray<FMonolithLogEntry> Entries = LogCapture->GetEntriesSince(
				MarkerTimestamp, /*CategoryFilter*/{}, ELogVerbosity::VeryVerbose, FMonolithLogCapture::MaxEntries);

			ActiveBucket.Reserve(Entries.Num());
			TeardownBucket.Reserve(Entries.Num());

			// #10 first entry containing the teardown marker splits the buckets.
			bool bInTeardown = false;
			for (const FMonolithLogEntry& Entry : Entries)
			{
				if (!bInTeardown && !S.IgnoreAfterPattern.IsEmpty() &&
					EntryMatches(Entry, S.IgnoreAfterPattern))
				{
					bInTeardown = true;
				}
				if (bInTeardown) { TeardownBucket.Add(Entry); }
				else { ActiveBucket.Add(Entry); }
			}
		}

		const FPieSmokeLogGroups& G = S.LogGroups;

		// Active-runtime bucket: the ok-bearing bucket.
		int32 AbsentTotal = 0, AbsentDistinct = 0;
		int32 PresentTotal = 0, PresentDistinct = 0;
		int32 ObserveTotal = 0, ObserveDistinct = 0;
		int32 WarnTotal = 0, WarnDistinct = 0;
		TSharedPtr<FJsonObject> AbsentCounts  = CountGroupInBucket(ActiveBucket, G.MustAbsent,  AbsentTotal,  AbsentDistinct);
		TSharedPtr<FJsonObject> PresentCounts = CountGroupInBucket(ActiveBucket, G.MustPresent, PresentTotal, PresentDistinct);
		TSharedPtr<FJsonObject> ObserveCounts = CountGroupInBucket(ActiveBucket, G.ObserveOnly, ObserveTotal, ObserveDistinct);
		TSharedPtr<FJsonObject> WarnCounts    = CountGroupInBucket(ActiveBucket, G.Warn,        WarnTotal,    WarnDistinct);

		// Teardown bucket: reported for visibility; affects ok only when !bTeardownAllowed.
		int32 TdAbsentTotal = 0, TdAbsentDistinct = 0;
		TSharedPtr<FJsonObject> TdAbsentCounts = CountGroupInBucket(TeardownBucket, G.MustAbsent, TdAbsentTotal, TdAbsentDistinct);

		// Grouped report.
		TSharedPtr<FJsonObject> Active = MakeShared<FJsonObject>();
		Active->SetObjectField(TEXT("must_absent"),  AbsentCounts);
		Active->SetObjectField(TEXT("must_present"), PresentCounts);
		Active->SetObjectField(TEXT("observe_only"), ObserveCounts);
		Active->SetObjectField(TEXT("warn"),         WarnCounts);
		Active->SetNumberField(TEXT("entry_count"),  ActiveBucket.Num());

		TSharedPtr<FJsonObject> Teardown = MakeShared<FJsonObject>();
		Teardown->SetObjectField(TEXT("must_absent"), TdAbsentCounts);
		Teardown->SetNumberField(TEXT("entry_count"), TeardownBucket.Num());
		Teardown->SetBoolField(TEXT("allowed"),       S.bTeardownAllowed);

		TSharedPtr<FJsonObject> Grouped = MakeShared<FJsonObject>();
		Grouped->SetObjectField(TEXT("active_runtime"), Active);
		Grouped->SetObjectField(TEXT("teardown"),       Teardown);
		Grouped->SetStringField(TEXT("ignore_after_pattern"), S.IgnoreAfterPattern);
		Report->SetObjectField(TEXT("grouped_counts"), Grouped);

		// Warn list (active-runtime hits only — never affects ok).
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Pattern : G.Warn)
		{
			int32 Count = 0;
			for (const FMonolithLogEntry& Entry : ActiveBucket)
			{
				if (EntryMatches(Entry, Pattern)) { ++Count; }
			}
			if (Count > 0)
			{
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("%s (x%d)"), *Pattern, Count)));
			}
		}
		Report->SetArrayField(TEXT("warnings"), Warnings);

		// Missing must_present patterns (the ones that failed the present check).
		TArray<TSharedPtr<FJsonValue>> MissingPresent;
		for (const FString& Pattern : G.MustPresent)
		{
			bool bFound = false;
			for (const FMonolithLogEntry& Entry : ActiveBucket)
			{
				if (EntryMatches(Entry, Pattern)) { bFound = true; break; }
			}
			if (!bFound) { MissingPresent.Add(MakeShared<FJsonValueString>(Pattern)); }
		}
		Report->SetArrayField(TEXT("missing_must_present"), MissingPresent);

		// Legacy back-compat fields: post_marker_counts (flat, active bucket) +
		// total_matches (active must_absent total). Existing callers still read these.
		TSharedPtr<FJsonObject> LegacyCounts = MakeShared<FJsonObject>();
		for (const FString& Pattern : S.LogPatterns)
		{
			int32 Count = 0;
			for (const FMonolithLogEntry& Entry : ActiveBucket)
			{
				if (EntryMatches(Entry, Pattern)) { ++Count; }
			}
			LegacyCounts->SetNumberField(Pattern, Count);
		}
		Report->SetObjectField(TEXT("post_marker_counts"), LegacyCounts);
		Report->SetNumberField(TEXT("total_matches"), AbsentTotal);

		// #3 ok rule: every must_absent absent (active bucket) AND every must_present
		// matched. #10: teardown-bucket must_absent hits only count when !bTeardownAllowed.
		const bool bAbsentOk  = (AbsentTotal == 0) && (S.bTeardownAllowed || TdAbsentTotal == 0);
		const bool bPresentOk = (PresentDistinct == G.MustPresent.Num());
		return bAbsentOk && bPresentOk;
	}

	// Run the caller's optional console commands on the ready PIE world.
	static void RunScripts(const TSharedPtr<FJsonObject>& Params, UWorld* PieWorld)
	{
		const TArray<TSharedPtr<FJsonValue>>* ConsoleArr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("console_script"), ConsoleArr) && ConsoleArr && PieWorld)
		{
			APlayerController* PC = PieWorld->GetFirstPlayerController();
			for (const TSharedPtr<FJsonValue>& Val : *ConsoleArr)
			{
				FString Command;
				if (Val.IsValid() && Val->TryGetString(Command) && !Command.IsEmpty())
				{
					if (PC) { PC->ConsoleCommand(Command, /*bWriteToLog=*/true); }
					else if (GEngine) { GEngine->Exec(PieWorld, *Command); }
				}
			}
		}
	}

	// Load the requested map into the editor before PIE (optional). Returns false +
	// OutError when a map path was given but failed to load.
	static bool LoadMapIfRequested(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString MapPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("map"), MapPath) || MapPath.IsEmpty())
		{
			return true; // No map requested — use the current editor level.
		}
		if (!GEditor)
		{
			OutError = TEXT("GEditor unavailable — cannot load map for PIE smoke.");
			return false;
		}
		// #5 world-leak guard (same policy as load_level): refuse / drive-teardown +
		// GC before LoadLevel so the resident PIE world is gone first.
		if (!EnsureNoResidentPieWorldBeforeMapLoad(OutError))
		{
			return false;
		}
		ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEd || !LevelEd->LoadLevel(MapPath))
		{
			OutError = FString::Printf(TEXT("Failed to load map '%s' for PIE smoke."), *MapPath);
			return false;
		}
		return true;
	}

	// Resolve the AnimInstance variable names to sample: caller-supplied sample_vars,
	// or the default GroundSpeed / bShouldMove / DesiredYawDelta set.
	static TArray<FString> ResolveSampleVars(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Vars;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("sample_vars"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				FString V;
				if (Val.IsValid() && Val->TryGetString(V) && !V.IsEmpty())
				{
					Vars.AddUnique(V);
				}
			}
		}
		if (Vars.Num() == 0)
		{
			Vars = { TEXT("GroundSpeed"), TEXT("bShouldMove"), TEXT("DesiredYawDelta") };
		}
		return Vars;
	}

	// #4 parse optional probe_scripts: [{ at_seconds, console?:[...] }, ...].
	// Probes fire ONCE against the LIVE PIE world from the per-frame observer when the
	// session elapsed reaches at_seconds (avoids the start-time RunScripts teardown race).
	static TArray<FPieSmokeProbe> ResolveProbes(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FPieSmokeProbe> Probes;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params.IsValid() && Params->TryGetArrayField(TEXT("probe_scripts"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *Arr)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}
				FPieSmokeProbe Probe;
				double At = 0.0;
				(*Obj)->TryGetNumberField(TEXT("at_seconds"), At);
				Probe.AtSeconds = FMath::Max(0.0, At);
				ReadStringArrayField(*Obj, TEXT("console"), Probe.Console);
				if (Probe.Console.Num() > 0)
				{
					Probes.Add(MoveTemp(Probe));
				}
			}
		}
		return Probes;
	}

	// Phase 8: parse a [x,y,z] JSON array value into an FVector. Returns false when the
	// value is absent or has < 3 numeric elements.
	static bool ParseVec3Array(const TSharedPtr<FJsonValue>& Val, FVector& OutVec)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Val.IsValid() || !Val->TryGetArray(Arr) || !Arr || Arr->Num() < 3)
		{
			return false;
		}
		OutVec = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	static bool ValidateSmokeParams(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		if (Params->HasField(TEXT("discard_first_frames")))
		{
			double DiscardFrames = 0.0;
			if (!Params->TryGetNumberField(TEXT("discard_first_frames"), DiscardFrames))
			{
				OutError = TEXT("discard_first_frames must be a number");
				return false;
			}
		}

		if (Params->HasField(TEXT("actor_setup")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (!Params->TryGetArrayField(TEXT("actor_setup"), Arr) || !Arr)
			{
				OutError = TEXT("actor_setup must be an array");
				return false;
			}

			for (int32 Index = 0; Index < Arr->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Val = (*Arr)[Index];
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj || !Obj->IsValid())
				{
					continue;
				}

				if ((*Obj)->HasField(TEXT("count")))
				{
					double CountVal = 0.0;
					if (!(*Obj)->TryGetNumberField(TEXT("count"), CountVal))
					{
						OutError = FString::Printf(TEXT("actor_setup[%d].count must be a number"), Index);
						return false;
					}
				}
			}
		}

		return true;
	}

	// Phase 8 (OG-E2/E5): parse the optional generic `actor_setup` block:
	//   actor_setup: [
	//     { class:"/Game/.../BP_Foo", count:3, locations:[[x,y,z],...],
	//       apply_data_asset:"/Game/.../DA_Bar", move_to:[x,y,z] }, ...
	//   ]
	// Fully general-purpose — `class` is any BP/native actor class, `apply_data_asset` is
	// any DataAsset path. Entries without a `class` are skipped (nothing to spawn).
	static TArray<FPieSmokeActorSetupEntry> ResolveActorSetups(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FPieSmokeActorSetupEntry> Setups;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("actor_setup"), Arr) || !Arr)
		{
			return Setups;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FPieSmokeActorSetupEntry Entry;
			if (!(*Obj)->TryGetStringField(TEXT("class"), Entry.ClassPath) || Entry.ClassPath.IsEmpty())
			{
				continue; // no class => nothing to spawn
			}

			int32 Count = 1;
			if ((*Obj)->HasField(TEXT("count")))
			{
				double CountVal = 0.0;
				if ((*Obj)->TryGetNumberField(TEXT("count"), CountVal))
				{
					Count = static_cast<int32>(CountVal);
				}
			}
			Entry.Count = FMath::Max(1, Count);

			const TArray<TSharedPtr<FJsonValue>>* LocsArr = nullptr;
			if ((*Obj)->TryGetArrayField(TEXT("locations"), LocsArr) && LocsArr)
			{
				for (const TSharedPtr<FJsonValue>& LocVal : *LocsArr)
				{
					FVector Loc = FVector::ZeroVector;
					if (ParseVec3Array(LocVal, Loc))
					{
						Entry.Locations.Add(Loc);
					}
				}
			}

			(*Obj)->TryGetStringField(TEXT("apply_data_asset"), Entry.ApplyDataAssetPath);

			const TArray<TSharedPtr<FJsonValue>>* MoveArr = nullptr;
			if ((*Obj)->TryGetArrayField(TEXT("move_to"), MoveArr) && MoveArr && MoveArr->Num() >= 3)
			{
				Entry.bHasMoveTo = true;
				Entry.MoveTo = FVector((*MoveArr)[0]->AsNumber(),
					(*MoveArr)[1]->AsNumber(), (*MoveArr)[2]->AsNumber());
			}

			Setups.Add(MoveTemp(Entry));
		}
		return Setups;
	}

	// #8 parse one stage payload object {console?:[...]} into a stage. The
	// caller supplies any extra parsing (e.g. after_n_ticks 'n'). Returns true if the
	// stage carries a runnable payload.
	static bool ParseStagePayload(const TSharedPtr<FJsonObject>& Obj, FPieSmokeStage& Stage)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		ReadStringArrayField(Obj, TEXT("console"), Stage.Console);
		return Stage.Console.Num() > 0;
	}

	// #8 resolve the optional `stages` object onto a session.
	//   stages = {
	//     pre_pie:        {console?:[...]},   // fired synchronously BEFORE PIE
	//     on_begin_play:  {console?:[...]},   // first ready observer tick
	//     after_n_ticks:  {n:int, console?:[...]},
	//     before_capture: {console?:[...]}    // clip variant: before first frame
	//   }
	static void ResolveStages(const TSharedPtr<FJsonObject>& Params, FPieSmokeStages& Stages)
	{
		if (Stages.bAny)
		{
			return; // already resolved (e.g. pre_pie fired before PIE start) — never re-parse.
		}
		const TSharedPtr<FJsonObject>* StagesObj = nullptr;
		if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("stages"), StagesObj) ||
			!StagesObj || !StagesObj->IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if ((*StagesObj)->TryGetObjectField(TEXT("pre_pie"), Sub) && Sub)
		{
			if (ParseStagePayload(*Sub, Stages.PrePie)) { Stages.bAny = true; }
		}
		if ((*StagesObj)->TryGetObjectField(TEXT("on_begin_play"), Sub) && Sub)
		{
			if (ParseStagePayload(*Sub, Stages.OnBeginPlay)) { Stages.bAny = true; }
		}
		if ((*StagesObj)->TryGetObjectField(TEXT("after_n_ticks"), Sub) && Sub)
		{
			double N = 0.0;
			(*Sub)->TryGetNumberField(TEXT("n"), N);
			Stages.AfterNTicks.FireAfterTicks = FMath::Max(1, (int32)N);
			if (ParseStagePayload(*Sub, Stages.AfterNTicks)) { Stages.bAny = true; }
		}
		if ((*StagesObj)->TryGetObjectField(TEXT("before_capture"), Sub) && Sub)
		{
			if (ParseStagePayload(*Sub, Stages.BeforeCapture)) { Stages.bAny = true; }
		}
	}

	// Apply grouped log patterns + teardown-bucket params + probes onto a session being
	// registered. Shared by run_pie_smoke and capture_pie_movement_clip.
	static void ApplySmokeParams(const TSharedPtr<FJsonObject>& Params, FPieSmokeSession& Session)
	{
		Session.LogGroups = ResolveLogGroups(Params);
		Session.LogPatterns = FlattenGroups(Session.LogGroups); // legacy mirror
		Session.SampleVarNames = ResolveSampleVars(Params);
		Params->TryGetStringField(TEXT("pawn_class"), Session.PawnClassFilter);

		// #10 teardown bucketing knobs.
		FString IgnoreAfter;
		if (Params->TryGetStringField(TEXT("ignore_after_pattern"), IgnoreAfter))
		{
			Session.IgnoreAfterPattern = IgnoreAfter; // empty disables the split
		}
		bool bTeardownAllowed = Session.bTeardownAllowed;
		if (Params->TryGetBoolField(TEXT("teardown_allowed"), bTeardownAllowed))
		{
			Session.bTeardownAllowed = bTeardownAllowed;
		}

		// #4 delayed probes.
		Session.Probes = ResolveProbes(Params);

		// Phase 8 (OG-E2/E5): declarative actor_setup spec, executed once on the first ready
		// observer tick against the live PIE world (spawn / apply DataAsset / AI move).
		Session.ActorSetups = ResolveActorSetups(Params);

		// #8 staged startup hooks (pre_pie fired by the handler before PIE start; the rest
		// fired by the observer at their lifecycle moments).
		ResolveStages(Params, Session.Stages);

		// #9 optional expected-anim-class assert (clip variant primarily, harmless elsewhere).
		Params->TryGetStringField(TEXT("expected_anim_class"), Session.Identity.ExpectedAnimClass);

		// #7 optional view-target subject (clip variant): resolved + SetViewTarget'd on the
		// first ready observer tick so frames render the intended actor.
		Params->TryGetStringField(TEXT("view_target_actor"), Session.ViewTargetActorRequest);

		// #7 optional first-frame warm-up (clip variant): the first N captured frames are saved
		// but excluded from valid/invalid accounting so an un-warmed first frame can't false-fail.
		if (Params->HasField(TEXT("discard_first_frames")))
		{
			double DiscardFrames = 0.0;
			if (Params->TryGetNumberField(TEXT("discard_first_frames"), DiscardFrames))
			{
				Session.DiscardFirstFrames = FMath::Clamp(
					static_cast<int32>(DiscardFrames), 0, 16);
			}
		}

		// Phase 9 (OG-E3): session-scoped profiling. csv_profile starts the CSV profiler and
		// trace_channels starts an Unreal Insights trace — both bracketed to EXACTLY the PIE
		// window (started on the first post-BeginPlay observer tick, stopped on every end path).
		Params->TryGetBoolField(TEXT("csv_profile"), Session.bCsvProfile);
		ReadStringArrayField(Params, TEXT("trace_channels"), Session.TraceChannels);
	}

	// #8 fire the pre_pie stage synchronously, BEFORE StartPieInternal. No PIE world yet,
	// so console is best-effort (no PIE PC). Stamps outcome.
	static void FirePrePieStage(FPieSmokeStages& Stages)
	{
		if (!Stages.bAny || Stages.PrePie.bFired)
		{
			return;
		}
		if (Stages.PrePie.Console.Num() == 0)
		{
			return;
		}
		Stages.PrePie.bFired = true;
		Stages.PrePie.FiredAtSeconds = 0.0;

		// Console (no PIE PC yet) routes through GEngine->Exec against the editor world.
		if (Stages.PrePie.Console.Num() > 0 && GEngine && GEditor)
		{
			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			for (const FString& Command : Stages.PrePie.Console)
			{
				if (!Command.IsEmpty()) { GEngine->Exec(EditorWorld, *Command); }
			}
		}	}

	static const TCHAR* StatusToString(EPieSmokeStatus Status)
	{
		switch (Status)
		{
		case EPieSmokeStatus::Running:  return TEXT("running");
		case EPieSmokeStatus::Complete: return TEXT("complete");
		case EPieSmokeStatus::Stopped:  return TEXT("stopped");
		case EPieSmokeStatus::Error:    return TEXT("error");
		default:                        return TEXT("unknown");
		}
	}

	// #11 derive the explicit lifecycle string from (Status, bPieActive, resident PIE
	// world). Status alone conflates "capture done but PIE still open" with "running".
	//   running                   : session actively sampling, PIE world live.
	//   capture-complete-pie-open : capture finished (Complete) but a PIE world lingers
	//                               and teardown has not been driven yet.
	//   teardown-started          : RequestEndPlayMap driven, PIE world not yet gone.
	//   teardown-complete         : finished + no resident PIE world remains.
	//   stopped-by-tool           : force-stopped via stop_pie_smoke.
	static const TCHAR* DeriveLifecycle(const FPieSmokeSession& S)
	{
		const bool bPieResident = (FMonolithEditorActions::FindActivePieWorld() != nullptr);

		if (S.bStoppedByTool)
		{
			return TEXT("stopped-by-tool");
		}
		if (S.Status == EPieSmokeStatus::Running)
		{
			return TEXT("running");
		}
		// Status is terminal (Complete / Stopped / Error) from here.
		if (S.bTeardownStarted)
		{
			return bPieResident ? TEXT("teardown-started") : TEXT("teardown-complete");
		}
		if (bPieResident)
		{
			return TEXT("capture-complete-pie-open");
		}
		return TEXT("teardown-complete");
	}

	// Build a poll/stop report for a session. When bFull, emit every per-frame sample
	// (and any captured frame paths); always emit the per-var min/max/last summary and
	// the post-marker pattern counts.
	static TSharedPtr<FJsonObject> BuildSessionReport(const FPieSmokeSession& S, bool bFull,
		FMonolithLogCapture* LogCapture)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("session_id"), S.Id);
		Root->SetStringField(TEXT("status"), StatusToString(S.Status));
		Root->SetStringField(TEXT("marker"), S.Marker);
		Root->SetStringField(TEXT("map"), S.MapName);
		Root->SetNumberField(TEXT("duration"), S.DurationSeconds);
		Root->SetBoolField(TEXT("pie_active"), S.bPieActive);
		Root->SetBoolField(TEXT("pie_ready"), S.bReady);
		Root->SetStringField(TEXT("lifecycle"), DeriveLifecycle(S)); // #11 explicit lifecycle
		Root->SetNumberField(TEXT("sample_count"), S.Samples.Num());

		const double EndTime = (S.Status == EPieSmokeStatus::Running)
			? FPlatformTime::Seconds() : S.LastObservedSeconds;
		Root->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, EndTime - S.StartTimeSeconds));

		if (!S.ErrorReason.IsEmpty())
		{
			Root->SetStringField(TEXT("error"), S.ErrorReason);
		}

		// Per-var summary: min / max / last across all samples.
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		for (const FString& VarName : S.SampleVarNames)
		{
			bool bSeen = false;
			bool bBool = false;
			double MinV = 0.0, MaxV = 0.0, LastV = 0.0;
			bool LastBool = false;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				for (const FPieSmokeSampleVar& Var : Sample.Vars)
				{
					if (Var.Name != VarName) { continue; }
					if (Var.bIsBool)
					{
						bBool = true;
						LastBool = Var.BoolValue;
						bSeen = true;
					}
					else
					{
						const double Num = Var.NumberValue;
						if (!bSeen) { MinV = MaxV = Num; }
						else { MinV = FMath::Min(MinV, Num); MaxV = FMath::Max(MaxV, Num); }
						LastV = Num;
						bSeen = true;
					}
				}
			}
			if (!bSeen) { continue; }
			TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
			if (bBool)
			{
				VarObj->SetStringField(TEXT("type"), TEXT("bool"));
				VarObj->SetBoolField(TEXT("last"), LastBool);
			}
			else
			{
				VarObj->SetStringField(TEXT("type"), TEXT("number"));
				VarObj->SetNumberField(TEXT("min"), MinV);
				VarObj->SetNumberField(TEXT("max"), MaxV);
				VarObj->SetNumberField(TEXT("last"), LastV);
			}
			Summary->SetObjectField(VarName, VarObj);
		}
		Root->SetObjectField(TEXT("var_summary"), Summary);

		// Clip-variant fields.
		if (S.bCaptureFrames)
		{
			Root->SetStringField(TEXT("output_dir"), S.OutputDir);
			Root->SetNumberField(TEXT("frame_count"), S.CaptureFrameIndex);
			if (S.bCaptureDeferred)
			{
				Root->SetStringField(TEXT("capture_status"), TEXT("deferred"));
			}
			TArray<TSharedPtr<FJsonValue>> Frames;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				if (!Sample.FramePath.IsEmpty())
				{
					Frames.Add(MakeShared<FJsonValueString>(Sample.FramePath));
				}
			}
			Root->SetArrayField(TEXT("frame_paths"), Frames);

			// #7 capture-validity report. Uniform / all-black frames (the hidden-editor /
			// no-active-level-viewport failure mode) are flagged invalid; captured_ok is
			// true only when at least one valid frame landed and none were invalid.
			TSharedPtr<FJsonObject> Validity = MakeShared<FJsonObject>();
			Validity->SetNumberField(TEXT("valid_frames"), S.ValidFrames);
			Validity->SetNumberField(TEXT("invalid_frames"), S.InvalidFrames);
			const bool bCapturedOk = (S.ValidFrames > 0) && (S.InvalidFrames == 0);
			Validity->SetBoolField(TEXT("captured_ok"), bCapturedOk);
			if (S.InvalidFrames > 0 && S.ValidFrames == 0)
			{
				Validity->SetStringField(TEXT("hint"),
					TEXT("All frames were uniform/black — the PIE viewport is likely not rendered ")
					TEXT("(hidden editor / no active level viewport). Pass view_target_actor or run ")
					TEXT("with a visible editor viewport."));
			}
			Root->SetObjectField(TEXT("capture_validity"), Validity);

			// #7 view-target diagnostics.
			TSharedPtr<FJsonObject> ViewTarget = MakeShared<FJsonObject>();
			ViewTarget->SetStringField(TEXT("active_actor"), S.ActiveViewTargetName);
			ViewTarget->SetStringField(TEXT("active_class"), S.ActiveViewTargetClass);
			if (!S.ViewTargetActorRequest.IsEmpty())
			{
				ViewTarget->SetStringField(TEXT("requested"), S.ViewTargetActorRequest);
				ViewTarget->SetStringField(TEXT("resolved_actor"), S.ViewTargetActorResolved);
				ViewTarget->SetStringField(TEXT("resolved_class"), S.ViewTargetActorClass);
				ViewTarget->SetBoolField(TEXT("resolved"), !S.ViewTargetActorResolved.IsEmpty());
			}
			Root->SetObjectField(TEXT("view_target"), ViewTarget);
		}

		// #9 clip runtime-identity report (also useful for non-clip smoke). Emitted once a
		// target actor + skel comp were resolved at least one sampled tick.
		if (S.Identity.bResolved)
		{
			TSharedPtr<FJsonObject> Id = MakeShared<FJsonObject>();
			Id->SetStringField(TEXT("actor"), S.Identity.ActorName);
			Id->SetStringField(TEXT("actor_class"), S.Identity.ActorClass);
			Id->SetStringField(TEXT("skel_comp"), S.Identity.SkelCompName);
			Id->SetStringField(TEXT("anim_instance_class"), S.Identity.AnimInstanceClassPath);
			Id->SetStringField(TEXT("mesh_anim_class"), S.Identity.MeshAnimClassPath);
			Id->SetStringField(TEXT("animation_mode"), S.Identity.AnimationMode);
			Id->SetBoolField(TEXT("anim_class_changed"), S.Identity.bAnimClassChanged);
			if (S.Identity.bExpectedChecked)
			{
				Id->SetStringField(TEXT("expected_anim_class"), S.Identity.ExpectedAnimClass);
				Id->SetBoolField(TEXT("expected_mismatch"), S.Identity.bExpectedMismatch);
			}
			Root->SetObjectField(TEXT("runtime_identity"), Id);
		}

		// #8 staged-hook outcome report.
		if (S.Stages.bAny)
		{
			auto StageJson = [](const FPieSmokeStage& St) -> TSharedPtr<FJsonObject>
			{
				TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
				O->SetBoolField(TEXT("fired"), St.bFired);
				O->SetNumberField(TEXT("fired_at_seconds"), St.FiredAtSeconds);
				if (St.Console.Num() > 0)
				{
					O->SetNumberField(TEXT("console_command_count"), St.Console.Num());
				}
				return O;
			};
			TSharedPtr<FJsonObject> StagesObj = MakeShared<FJsonObject>();
			auto AddStage = [&](const TCHAR* Key, const FPieSmokeStage& St)
			{
				if (St.Console.Num() > 0)
				{
					StagesObj->SetObjectField(Key, StageJson(St));
				}
			};
			AddStage(TEXT("pre_pie"),        S.Stages.PrePie);
			AddStage(TEXT("on_begin_play"),  S.Stages.OnBeginPlay);
			AddStage(TEXT("after_n_ticks"),  S.Stages.AfterNTicks);
			AddStage(TEXT("before_capture"), S.Stages.BeforeCapture);
			Root->SetObjectField(TEXT("stages"), StagesObj);
		}

		// Full per-frame sample array (on completion or when explicitly requested).
		if (bFull)
		{
			TArray<TSharedPtr<FJsonValue>> SampleArr;
			for (const FPieSmokeSample& Sample : S.Samples)
			{
				TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
				SObj->SetNumberField(TEXT("t"), Sample.TimeSeconds);
				if (!Sample.FramePath.IsEmpty())
				{
					SObj->SetStringField(TEXT("frame_path"), Sample.FramePath);
				}
				for (const FPieSmokeSampleVar& Var : Sample.Vars)
				{
					if (Var.bIsBool) { SObj->SetBoolField(Var.Name, Var.BoolValue); }
					else { SObj->SetNumberField(Var.Name, Var.NumberValue); }
				}
				SampleArr.Add(MakeShared<FJsonValueObject>(SObj));
			}
			Root->SetArrayField(TEXT("samples"), SampleArr);
		}

		// #3 + #10 grouped post-marker analysis (also writes legacy post_marker_counts /
		// total_matches for back-compat, plus warnings + missing_must_present).
		const bool bLogOk = AnalysePostMarkerGrouped(LogCapture, S.StartTimeSeconds, S, Root);
		Root->SetBoolField(TEXT("ok"),
			(S.Status == EPieSmokeStatus::Complete) && S.bReady && bLogOk);

		// #4 delayed in-session probe results.
		if (S.Probes.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ProbeArr;
			for (const FPieSmokeProbe& Probe : S.Probes)
			{
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetNumberField(TEXT("at_seconds"), Probe.AtSeconds);
				PObj->SetBoolField(TEXT("fired"), Probe.bFired);
				PObj->SetNumberField(TEXT("fired_at_seconds"), Probe.FiredAtSeconds);
				if (Probe.Console.Num() > 0)
				{
					PObj->SetNumberField(TEXT("console_command_count"), Probe.Console.Num());
				}
				ProbeArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Root->SetArrayField(TEXT("probes"), ProbeArr);
		}

		// Gap 9: time-series sampling + typed-provocation report. Emitted for a timeseries
		// session (bTimeseries). The full per-sample {t, vars:{...}} array is gated behind
		// bFull (completion or include_samples) like the smoke samples; the provocation fire
		// log is always emitted so a caller sees what fired without pulling the whole series.
		if (S.bTimeseries)
		{
			Root->SetNumberField(TEXT("timeseries_sample_count"), S.TimeseriesSamples.Num());

			// Provocation fire log (always emitted).
			TArray<TSharedPtr<FJsonValue>> ProvArr;
			for (const FPieProvocation& Prov : S.Provocations)
			{
				TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
				PObj->SetStringField(TEXT("action"), Prov.RawAction);
				PObj->SetNumberField(TEXT("time"), Prov.AtSeconds);
				PObj->SetBoolField(TEXT("fired"), Prov.bFired);
				PObj->SetNumberField(TEXT("fired_at_seconds"), Prov.FiredAtSeconds);
				PObj->SetBoolField(TEXT("dispatched"), Prov.bDispatched);
				if (!Prov.Result.IsEmpty()) { PObj->SetStringField(TEXT("result"), Prov.Result); }
				ProvArr.Add(MakeShared<FJsonValueObject>(PObj));
			}
			Root->SetArrayField(TEXT("provocations"), ProvArr);

			// Full per-sample time-series (gated by bFull).
			if (bFull)
			{
				TArray<TSharedPtr<FJsonValue>> SeriesArr;
				for (const FPieTimeseriesSample& TS : S.TimeseriesSamples)
				{
					TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
					SObj->SetNumberField(TEXT("t"), TS.TimeSeconds);
					TSharedPtr<FJsonObject> VarsObj = MakeShared<FJsonObject>();
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Var : TS.Vars)
					{
						if (Var.Value.IsValid()) { VarsObj->SetField(Var.Key, Var.Value); }
					}
					SObj->SetObjectField(TEXT("vars"), VarsObj);
					SeriesArr.Add(MakeShared<FJsonValueObject>(SObj));
				}
				Root->SetArrayField(TEXT("timeseries"), SeriesArr);
			}
		}

		// Phase 8 (OG-E2/E5): structured actor_setup outcome. Per-entry class/DataAsset
		// resolution + per-actor spawn ok/fail, applied/unmatched DataAsset fields, and the
		// AI move-request result — so callers distinguish partial from full apply
		// programmatically (not via a log line).
		if (S.ActorSetupResults.Num() > 0 || S.ActorSetups.Num() > 0)
		{
			auto FieldsToJson = [](const TArray<FString>& Fields) -> TArray<TSharedPtr<FJsonValue>>
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				Out.Reserve(Fields.Num());
				for (const FString& F : Fields) { Out.Add(MakeShared<FJsonValueString>(F)); }
				return Out;
			};

			TArray<TSharedPtr<FJsonValue>> SetupArr;
			SetupArr.Reserve(S.ActorSetupResults.Num());
			for (const FPieSmokeActorSetupResult& Entry : S.ActorSetupResults)
			{
				TSharedPtr<FJsonObject> EObj = MakeShared<FJsonObject>();
				EObj->SetStringField(TEXT("class"), Entry.ClassPath);
				EObj->SetBoolField(TEXT("class_resolved"), Entry.bClassResolved);
				EObj->SetNumberField(TEXT("requested_count"), Entry.RequestedCount);
				EObj->SetNumberField(TEXT("spawned_count"), Entry.SpawnedCount);
				if (!Entry.ApplyDataAssetPath.IsEmpty())
				{
					EObj->SetStringField(TEXT("apply_data_asset"), Entry.ApplyDataAssetPath);
					EObj->SetBoolField(TEXT("data_asset_loaded"), Entry.bDataAssetLoaded);
					if (!Entry.DataAssetError.IsEmpty())
					{
						EObj->SetStringField(TEXT("data_asset_error"), Entry.DataAssetError);
					}
				}

				TArray<TSharedPtr<FJsonValue>> ActorArr;
				ActorArr.Reserve(Entry.Actors.Num());
				for (const FPieSmokeSpawnedActorResult& A : Entry.Actors)
				{
					TSharedPtr<FJsonObject> AObj = MakeShared<FJsonObject>();
					AObj->SetBoolField(TEXT("spawned"), A.bSpawned);
					if (A.bSpawned)
					{
						AObj->SetStringField(TEXT("name"), A.ActorName);
						AObj->SetStringField(TEXT("runtime_class"), A.RuntimeClassPath);
						// Structured apply verdict — partial-vs-full is computable by the caller.
						AObj->SetArrayField(TEXT("applied"), FieldsToJson(A.AppliedFields));
						AObj->SetArrayField(TEXT("unmatched"), FieldsToJson(A.UnmatchedFields));
						if (A.bMoveRequested)
						{
							TSharedPtr<FJsonObject> MoveObj = MakeShared<FJsonObject>();
							MoveObj->SetBoolField(TEXT("issued"), A.bMoveIssued);
							MoveObj->SetStringField(TEXT("result"), A.MoveResult);
							AObj->SetObjectField(TEXT("move_to"), MoveObj);
						}
					}
					else
					{
						AObj->SetStringField(TEXT("error"), A.SpawnError);
					}
					ActorArr.Add(MakeShared<FJsonValueObject>(AObj));
				}
				EObj->SetArrayField(TEXT("actors"), ActorArr);
				SetupArr.Add(MakeShared<FJsonValueObject>(EObj));
			}
			Root->SetArrayField(TEXT("actor_setup"), SetupArr);
		}

		// Phase 9 (OG-E3): session-scoped profiling outcome. Reported whenever profiling was
		// requested so the caller can retrieve csv_path / trace_path and confirm the capture
		// was stopped (started_* flip false at session end via StopSessionProfiling). When CSV
		// is compiled out, available=false + a clear status rather than a missing field.
		if (S.bCsvProfile || S.TraceChannels.Num() > 0)
		{
			TSharedPtr<FJsonObject> Prof = MakeShared<FJsonObject>();

			if (S.bCsvProfile)
			{
				TSharedPtr<FJsonObject> Csv = MakeShared<FJsonObject>();
				Csv->SetBoolField(TEXT("requested"), true);
				Csv->SetBoolField(TEXT("available"), S.bCsvAvailable);
				Csv->SetBoolField(TEXT("capturing"), S.bCsvStarted);
				if (!S.CsvStatus.IsEmpty()) { Csv->SetStringField(TEXT("status"), S.CsvStatus); }
				if (!S.CsvPath.IsEmpty())   { Csv->SetStringField(TEXT("csv_path"), S.CsvPath); }
				Prof->SetObjectField(TEXT("csv"), Csv);
				// Convenience top-level mirror of the artifact path.
				if (!S.CsvPath.IsEmpty()) { Prof->SetStringField(TEXT("csv_path"), S.CsvPath); }
			}

			if (S.TraceChannels.Num() > 0)
			{
				TSharedPtr<FJsonObject> Trace = MakeShared<FJsonObject>();
				Trace->SetBoolField(TEXT("requested"), true);
				Trace->SetBoolField(TEXT("tracing"), S.bTraceStarted);
				TArray<TSharedPtr<FJsonValue>> ChArr;
				ChArr.Reserve(S.TraceChannels.Num());
				for (const FString& Ch : S.TraceChannels) { ChArr.Add(MakeShared<FJsonValueString>(Ch)); }
				Trace->SetArrayField(TEXT("channels"), ChArr);
				if (!S.TraceStatus.IsEmpty()) { Trace->SetStringField(TEXT("status"), S.TraceStatus); }
				if (!S.TracePath.IsEmpty())   { Trace->SetStringField(TEXT("trace_path"), S.TracePath); }
				Prof->SetObjectField(TEXT("trace"), Trace);
				if (!S.TracePath.IsEmpty()) { Prof->SetStringField(TEXT("trace_path"), S.TracePath); }
			}

			Root->SetObjectField(TEXT("profiling"), Prof);
		}

		return Root;
	}
}

FMonolithActionResult FMonolithEditorActions::HandleRunPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	if (!GEditor || !GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("run_pie_smoke requires editor context (GEditor/GUnrealEd)."));
	}
	if (FindActivePieWorld())
	{
		return FMonolithActionResult::Error(TEXT("A PIE session is already running — stop it before run_pie_smoke."));
	}

	FString Marker = TEXT("MONOLITH_SMOKE");
	Params->TryGetStringField(TEXT("marker"), Marker);

	double Duration = 5.0;
	if (Params->HasField(TEXT("duration")))
	{
		if (!Params->TryGetNumberField(TEXT("duration"), Duration))
		{
			return FMonolithActionResult::Error(TEXT("duration must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Duration = FMath::Clamp(Duration, 0.0, 120.0);

	FString SmokeParamError;
	if (!ValidateSmokeParams(Params, SmokeParamError))
	{
		return FMonolithActionResult::Error(SmokeParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	// Compile-error policy: "refuse" (default, safe) returns an error + the offending
	// Blueprints and never starts PIE; "suppress" proceeds and silences the engine's
	// blocking compile-error prompt via the StartPieInternal unattended guard.
	FString CompileMode = TEXT("refuse");
	Params->TryGetStringField(TEXT("on_compile_errors"), CompileMode);
	const bool bSuppressModals = CompileMode.Equals(TEXT("suppress"), ESearchCase::IgnoreCase);

	// Optional map load before PIE.
	FString LoadError;
	if (!LoadMapIfRequested(Params, LoadError))
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// PIE pre-flight: scan for errored Blueprints AFTER any map load (the loaded level
	// brings its own Blueprints into memory). In refuse mode, never PIE a broken world —
	// a compile-error modal would run a nested game-thread loop and strangle the MCP server.
	{
		TArray<FErroredBlueprintEntry> Errored;
		ScanErroredBlueprints(Errored);
		if (Errored.Num() > 0 && !bSuppressModals)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("errored_blueprint_count"), Errored.Num());
			ErrObj->SetArrayField(TEXT("errored_blueprints"), ErroredBlueprintsToJson(Errored));
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("run_pie_smoke refused: %d Blueprint(s) have unresolved compile errors. ")
					TEXT("Starting PIE would raise a blocking modal that freezes the editor + MCP server. ")
					TEXT("Fix the Blueprints, or pass on_compile_errors=\"suppress\" to PIE anyway."),
					Errored.Num()))
				.WithErrorData(ErrObj);
		}
	}

	// #8 stage hooks: resolve them now and fire the pre_pie stage SYNCHRONOUSLY before
	// PIE starts (the rest fire from the observer at their lifecycle moments). Stored on
	// the session up-front so ApplySmokeParams' ResolveStages is a no-op (idempotent guard).
	FPieSmokeSession Session;
	ResolveStages(Params, Session.Stages);
	FirePrePieStage(Session.Stages);

	// Start PIE synchronously (the start request itself is safe inside the handler —
	// the re-entrancy crash was the OLD pump driving UWorld/GEditor::Tick afterwards;
	// that work now happens on the editor's real frames via the session observer).
	// bSuppressModals wraps the request in the unattended guard so a compile-error
	// prompt resolves to its default instead of blocking.
	FString StartError;
	if (!StartPieInternal(StartError, bSuppressModals))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to start PIE: %s"), *StartError));
	}

	// Emit the marker now; the observer counts only post-marker log lines. PIE may not
	// have run BeginPlay yet — the observer waits for HasBegunPlay before sampling.
	UWorld* PieWorld = FindActivePieWorld();
	UE_LOG(LogMonolith, Display, TEXT("%s begin (map=%s)"), *Marker,
		PieWorld ? *PieWorld->GetMapName() : TEXT("<current>"));

	// Optional console commands run once at start (best-effort).
	if (PieWorld)
	{
		RunScripts(Params, PieWorld);
	}

	// Register the async session — the editor frame loop advances it from here.
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Session.DurationSeconds = Duration;
	Session.Marker = Marker;
	Session.MapName = PieWorld ? PieWorld->GetMapName() : TEXT("<current>");
	ApplySmokeParams(Params, Session); // #3 groups + #10 bucketing + #4 probes + #8/#9/#7

	const FString SessionId = FPieSmokeSessionManager::Get().CreateSession(MoveTemp(Session));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetBoolField(TEXT("started"), true);
	Result->SetStringField(TEXT("marker"), Marker);
	Result->SetNumberField(TEXT("duration"), Duration);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Gap 9: sample_pie_timeseries — async time-series PIE sampling + typed provocation.
// Same async lifecycle as run_pie_smoke (returns {session_id, status:'running'}; polled
// via poll_pie_smoke, stopped via stop_pie_smoke). Reuses the in-TU PIE-start / map-load /
// compile-gate helpers + the shared FPieSmokeSessionManager — no parallel ticker.
// ---------------------------------------------------------------------------

namespace MonolithEditorPieSmoke
{
	// Parse a [x,y,z] JSON array into a vector (missing components default to 0).
	static FVector ParseVec3(const TArray<TSharedPtr<FJsonValue>>& Arr, const FVector& Fallback)
	{
		FVector V = Fallback;
		if (Arr.Num() >= 1 && Arr[0].IsValid()) { V.X = Arr[0]->AsNumber(); }
		if (Arr.Num() >= 2 && Arr[1].IsValid()) { V.Y = Arr[1]->AsNumber(); }
		if (Arr.Num() >= 3 && Arr[2].IsValid()) { V.Z = Arr[2]->AsNumber(); }
		return V;
	}

	// Resolve typed provocations from the params 'provocations' array. Each entry is
	// {time, action, params}. Unknown actions are dropped (with the raw token preserved
	// so the report can flag them). Params interpretation is per-action.
	static TArray<FPieProvocation> ResolveProvocations(const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FPieProvocation> Out;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("provocations"), Arr) || !Arr)
		{
			return Out;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Arr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(Obj) || !Obj || !(*Obj).IsValid()) { continue; }

			FPieProvocation Prov;
			(*Obj)->TryGetNumberField(TEXT("time"), Prov.AtSeconds);
			(*Obj)->TryGetStringField(TEXT("action"), Prov.RawAction);

			const TSharedPtr<FJsonObject>* PParams = nullptr;
			const bool bHasParams = (*Obj)->TryGetObjectField(TEXT("params"), PParams) && PParams && (*PParams).IsValid();

			if (Prov.RawAction.Equals(TEXT("set_control_rotation"), ESearchCase::IgnoreCase))
			{
				Prov.Action = EPieProvocationAction::SetControlRotation;
				if (bHasParams)
				{
					double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
					(*PParams)->TryGetNumberField(TEXT("pitch"), Pitch);
					(*PParams)->TryGetNumberField(TEXT("yaw"), Yaw);
					(*PParams)->TryGetNumberField(TEXT("roll"), Roll);
					Prov.Rotation = FRotator(Pitch, Yaw, Roll);
				}
			}
			else if (Prov.RawAction.Equals(TEXT("add_movement_input"), ESearchCase::IgnoreCase))
			{
				Prov.Action = EPieProvocationAction::AddMovementInput;
				if (bHasParams)
				{
					const TArray<TSharedPtr<FJsonValue>>* DirArr = nullptr;
					if ((*PParams)->TryGetArrayField(TEXT("direction"), DirArr) && DirArr)
					{
						Prov.Direction = ParseVec3(*DirArr, FVector::ForwardVector);
					}
					(*PParams)->TryGetNumberField(TEXT("scale"), Prov.Scale);
				}
			}
			else if (Prov.RawAction.Equals(TEXT("jump"), ESearchCase::IgnoreCase))
			{
				Prov.Action = EPieProvocationAction::Jump;
			}
			else if (Prov.RawAction.Equals(TEXT("console_command"), ESearchCase::IgnoreCase))
			{
				Prov.Action = EPieProvocationAction::ConsoleCommand;
				if (bHasParams)
				{
					(*PParams)->TryGetStringField(TEXT("command"), Prov.Command);
				}
			}
			else
			{
				Prov.Action = EPieProvocationAction::Unknown;
			}
			Out.Add(MoveTemp(Prov));
		}
		return Out;
	}
}

FMonolithActionResult FMonolithEditorActions::StartTimeseriesSession(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	if (!GEditor || !GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("sample_pie_timeseries requires editor context (GEditor/GUnrealEd)."));
	}

	// Target selector: one of actor / pawn_class / object_name (mirrors Gap 8's resolver
	// vocabulary; 'actor' = exact label, 'pawn_class' = class-name substring).
	FString TargetActorLabel, TargetObjectName, TargetClassName;
	Params->TryGetStringField(TEXT("actor"), TargetActorLabel);
	Params->TryGetStringField(TEXT("object_name"), TargetObjectName);
	Params->TryGetStringField(TEXT("pawn_class"), TargetClassName);
	if (TargetActorLabel.IsEmpty() && TargetObjectName.IsEmpty() && TargetClassName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("sample_pie_timeseries requires one of: actor, pawn_class, object_name"));
	}

	TArray<FString> VarPaths;
	ReadStringArrayField(Params, TEXT("variables"), VarPaths);
	if (VarPaths.Num() == 0)
	{
		return FMonolithActionResult::Error(TEXT("sample_pie_timeseries requires a non-empty 'variables' array of dotted paths"));
	}

	double Duration = 6.0;
	if (Params->HasField(TEXT("duration_seconds")) && !Params->TryGetNumberField(TEXT("duration_seconds"), Duration))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter: 'duration_seconds' must be a number"));
	}
	Duration = FMath::Clamp(Duration, 0.0, 120.0);

	double SampleInterval = 0.0;
	if (Params->HasField(TEXT("sample_interval")) && !Params->TryGetNumberField(TEXT("sample_interval"), SampleInterval))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter: 'sample_interval' must be a number"));
	}
	SampleInterval = FMath::Max(0.0, SampleInterval);

	int32 MaxSamples = 2048;
	if (Params->HasField(TEXT("max_samples")))
	{
		double TempMaxSamples;
		if (!Params->TryGetNumberField(TEXT("max_samples"), TempMaxSamples))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter: 'max_samples' must be a number"));
		}
		MaxSamples = (int32)TempMaxSamples;
	}
	MaxSamples = FMath::Clamp(MaxSamples, 1, 100000);

	if (FindActivePieWorld())
	{
		return FMonolithActionResult::Error(TEXT("A PIE session is already running — stop it before sample_pie_timeseries."));
	}

	// Optional map load before PIE.
	FString LoadError;
	if (!LoadMapIfRequested(Params, LoadError))
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// Compile-error gate (same policy as run_pie_smoke: refuse a broken world by default).
	FString CompileMode = TEXT("refuse");
	Params->TryGetStringField(TEXT("on_compile_errors"), CompileMode);
	const bool bSuppressModals = CompileMode.Equals(TEXT("suppress"), ESearchCase::IgnoreCase);
	{
		TArray<FErroredBlueprintEntry> Errored;
		ScanErroredBlueprints(Errored);
		if (Errored.Num() > 0 && !bSuppressModals)
		{
			TSharedPtr<FJsonObject> ErrObj = MakeShared<FJsonObject>();
			ErrObj->SetNumberField(TEXT("errored_blueprint_count"), Errored.Num());
			ErrObj->SetArrayField(TEXT("errored_blueprints"), ErroredBlueprintsToJson(Errored));
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("sample_pie_timeseries refused: %d Blueprint(s) have unresolved compile errors. ")
					TEXT("Fix them, or pass on_compile_errors=\"suppress\" to PIE anyway."), Errored.Num()))
				.WithErrorData(ErrObj);
		}
	}

	FString StartError;
	if (!StartPieInternal(StartError, bSuppressModals))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to start PIE: %s"), *StartError));
	}

	UWorld* PieWorld = FindActivePieWorld();
	const FString Marker = TEXT("MONOLITH_TIMESERIES");
	UE_LOG(LogMonolith, Display, TEXT("%s begin (map=%s)"), *Marker,
		PieWorld ? *PieWorld->GetMapName() : TEXT("<current>"));

	// Optional start-time console/python scripts (best-effort), same as run_pie_smoke.
	if (PieWorld)
	{
		RunScripts(Params, PieWorld);
	}

	// Build the time-series session.
	FPieSmokeSession Session;
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Session.DurationSeconds = Duration;
	Session.Marker = Marker;
	Session.MapName = PieWorld ? PieWorld->GetMapName() : TEXT("<current>");

	Session.bTimeseries = true;
	Session.TimeseriesVarPaths = MoveTemp(VarPaths);
	Session.SampleInterval = SampleInterval;
	Session.MaxSamples = MaxSamples;
	Session.TargetActorLabel = TargetActorLabel;
	Session.TargetObjectName = TargetObjectName;
	Session.TargetClassName = TargetClassName;
	Params->TryGetStringField(TEXT("component_name"), Session.TargetComponentName);
	Params->TryGetBoolField(TEXT("anim_instance"), Session.bTargetAnimInstance);
	Session.Provocations = ResolveProvocations(Params);

	const FString SessionId = FPieSmokeSessionManager::Get().CreateSession(MoveTemp(Session));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetBoolField(TEXT("started"), true);
	Result->SetNumberField(TEXT("duration"), Duration);
	Result->SetStringField(TEXT("note"), TEXT("Poll with poll_pie_smoke; stop with stop_pie_smoke. Time-series under 'timeseries'; provocation fire log under 'provocations'."));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// poll_pie_smoke / stop_pie_smoke — read progress / force-end an async session
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandlePollPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	FString SessionId;
	if (!Params->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("poll_pie_smoke requires a session_id."));
	}

	FPieSmokeSessionManager& Mgr = FPieSmokeSessionManager::Get();
	FPieSmokeSession* Session = Mgr.Find(SessionId);
	if (!Session)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Unknown PIE-smoke session '%s'."), *SessionId));
	}

	bool bIncludeSamples = false;
	Params->TryGetBoolField(TEXT("include_samples"), bIncludeSamples);
	const bool bFull = bIncludeSamples || (Session->Status != EPieSmokeStatus::Running);

	return FMonolithActionResult::Success(
		BuildSessionReport(*Session, bFull, Mgr.GetLogCapture()));
}

FMonolithActionResult FMonolithEditorActions::HandleStopPieSmoke(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId); // empty => stop all

	FPieSmokeSessionManager& Mgr = FPieSmokeSessionManager::Get();
	const int32 Stopped = Mgr.Stop(SessionId);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("stopped"), Stopped);

	if (!SessionId.IsEmpty())
	{
		if (FPieSmokeSession* Session = Mgr.Find(SessionId))
		{
			Result->SetObjectField(TEXT("report"),
				BuildSessionReport(*Session, /*bFull=*/true, Mgr.GetLogCapture()));
		}
		else
		{
			Result->SetStringField(TEXT("warning"),
				FString::Printf(TEXT("Unknown session '%s' — nothing to stop."), *SessionId));
		}
	}
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// list_errored_blueprints — read-only PIE pre-flight scan
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandleListErroredBlueprints(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FErroredBlueprintEntry> Errored;
	ScanErroredBlueprints(Errored);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("count"), Errored.Num());
	Result->SetArrayField(TEXT("blueprints"), ErroredBlueprintsToJson(Errored));
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F3: capture_pie_movement_clip — async session + per-interval frame capture +
// AnimInstance sampling (PIE/profiling harness plan 2026-06-04)
// ---------------------------------------------------------------------------

FMonolithActionResult FMonolithEditorActions::HandleCapturePieMovementClip(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorPieSmoke;

	if (!GEditor || !GUnrealEd)
	{
		return FMonolithActionResult::Error(TEXT("capture_pie_movement_clip requires editor context (GEditor/GUnrealEd)."));
	}
	if (FindActivePieWorld())
	{
		return FMonolithActionResult::Error(TEXT("A PIE session is already running — stop it before capture_pie_movement_clip."));
	}

	FString Marker = TEXT("MONOLITH_CLIP");
	Params->TryGetStringField(TEXT("marker"), Marker);

	double Duration = 5.0;
	if (Params->HasField(TEXT("duration")))
	{
		if (!Params->TryGetNumberField(TEXT("duration"), Duration))
		{
			return FMonolithActionResult::Error(TEXT("duration must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Duration = FMath::Clamp(Duration, 0.0, 120.0);

	double Interval = 0.25;
	if (Params->HasField(TEXT("capture_interval")))
	{
		if (!Params->TryGetNumberField(TEXT("capture_interval"), Interval))
		{
			return FMonolithActionResult::Error(TEXT("capture_interval must be a number"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	Interval = FMath::Clamp(Interval, 0.05, 5.0);

	FString SmokeParamError;
	if (!ValidateSmokeParams(Params, SmokeParamError))
	{
		return FMonolithActionResult::Error(SmokeParamError, FMonolithJsonUtils::ErrInvalidParams);
	}

	FString OutputDir;
	if (Params->HasField(TEXT("output_path")))
	{
		if (!Params->TryGetStringField(TEXT("output_path"), OutputDir))
		{
			return FMonolithActionResult::Error(TEXT("output_path must be a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	else
	{
		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		OutputDir = FPaths::ProjectDir() / TEXT("Saved/Screenshots/Monolith/PieClip") / Stamp;
	}
	// Resolve virtual /Game output paths to on-disk dirs + create the directory BEFORE
	// starting PIE. A virtual path that can't be written is a hard error here — the async
	// frame writer would otherwise silently no-op and the session would report success
	// with no PNGs on disk.
	FString OutputDirError;
	if (!ResolveCaptureOutputDir(OutputDir, OutputDir, OutputDirError))
	{
		return FMonolithActionResult::Error(OutputDirError);
	}

	FString LoadError;
	if (!LoadMapIfRequested(Params, LoadError))
	{
		return FMonolithActionResult::Error(LoadError);
	}

	// #8 resolve stages + fire pre_pie synchronously before PIE start (see HandleRunPieSmoke).
	FPieSmokeSession Session;
	ResolveStages(Params, Session.Stages);
	FirePrePieStage(Session.Stages);

	// Start PIE synchronously (safe — see HandleRunPieSmoke). Frame capture + sampling
	// run on the editor's real frames via the session observer.
	FString StartError;
	if (!StartPieInternal(StartError))
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to start PIE: %s"), *StartError));
	}

	UWorld* PieWorld = FindActivePieWorld();
	UE_LOG(LogMonolith, Display, TEXT("%s begin"), *Marker);
	if (PieWorld)
	{
		RunScripts(Params, PieWorld);
	}

	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Session.DurationSeconds = Duration;
	Session.Marker = Marker;
	Session.MapName = PieWorld ? PieWorld->GetMapName() : TEXT("<current>");
	ApplySmokeParams(Params, Session); // #3 groups + #10 bucketing + #4 probes + #8/#9/#7
	Session.bCaptureFrames = true;
	Session.CaptureInterval = Interval;
	Session.OutputDir = OutputDir;

	const FString SessionId = FPieSmokeSessionManager::Get().CreateSession(MoveTemp(Session));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("running"));
	Result->SetBoolField(TEXT("started"), true);
	Result->SetStringField(TEXT("marker"), Marker);
	Result->SetStringField(TEXT("output_dir"), OutputDir);            // resolved absolute on-disk dir
	Result->SetStringField(TEXT("resolved_output_dir"), OutputDir);   // explicit alias for callers
	Result->SetNumberField(TEXT("duration"), Duration);
	Result->SetNumberField(TEXT("capture_interval"), Interval);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// F4: create_nav_harness_map — build a nav test map from a JSON spec
// (PIE/profiling harness plan 2026-06-04)
//
// Nav rebuild + validation are delegated to the registered `ai` actions via
// runtime string dispatch (FMonolithToolRegistry::ExecuteAction) so MonolithEditor
// takes NO compile-time dependency on MonolithAI / the NavigationSystem module.
// ---------------------------------------------------------------------------

namespace MonolithEditorNavHarness
{
	// Parse a [x,y,z] (or {x,y,z}) JSON value into an FVector. Returns false if absent.
	// If OutError is provided, a present-but-malformed field also writes a -32602-ready
	// validation message so callers can reject bad params instead of silently defaulting.
	static bool ParseVec3(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& OutVec,
		FString* OutError = nullptr)
	{
		if (!Obj.IsValid() || !Obj->HasField(Field))
		{
			return false;
		}

		auto SetError = [&]()
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Invalid param: '%s' must be a numeric [x,y,z] array or an object with numeric x/y/z fields"), *Field);
			}
		};

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Obj->TryGetArrayField(Field, Arr) && Arr)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (Arr->Num() < 3 ||
				!(*Arr)[0].IsValid() || !(*Arr)[0]->TryGetNumber(X) ||
				!(*Arr)[1].IsValid() || !(*Arr)[1]->TryGetNumber(Y) ||
				!(*Arr)[2].IsValid() || !(*Arr)[2]->TryGetNumber(Z))
			{
				SetError();
				return false;
			}
			OutVec = FVector(X, Y, Z);
			return true;
		}
		const TSharedPtr<FJsonObject>* SubObj = nullptr;
		if (Obj->TryGetObjectField(Field, SubObj) && SubObj && SubObj->IsValid())
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if ((*SubObj)->TryGetNumberField(TEXT("x"), X) &&
				(*SubObj)->TryGetNumberField(TEXT("y"), Y) &&
				(*SubObj)->TryGetNumberField(TEXT("z"), Z))
			{
				OutVec = FVector(X, Y, Z);
				return true;
			}
			SetError();
			return false;
		}
		SetError();
		return false;
	}

	static bool ParseVec3Param(const TSharedPtr<FJsonObject>& Obj, const FString& Field, FVector& OutVec, FString& OutError)
	{
		OutError.Empty();
		ParseVec3(Obj, Field, OutVec, &OutError);
		return OutError.IsEmpty();
	}

	static TArray<TSharedPtr<FJsonValue>> Vec3ToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	// Resolve a JSON class-path string to a UClass*, tolerating both the Blueprint
	// object path ("/Game/.../BP_Foo.BP_Foo") and the generated-class form
	// ("/Game/.../BP_Foo.BP_Foo_C"). A bare object path imported into a CLASS/SOFTCLASS
	// property would resolve to the UBlueprint object, not its generated UClass — so
	// class-typed props MUST normalize to the `_C` generated-class form first.
	// Native class paths ("/Script/Engine.PointLight") resolve unchanged. Returns
	// nullptr if neither form loads.
	static UClass* ResolveClassPath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return nullptr;
		}
		// Direct load first — handles /Script/... native classes and already-`_C` paths.
		if (UClass* Direct = LoadClass<UObject>(nullptr, *InPath))
		{
			return Direct;
		}
		if (UClass* DirectObj = LoadObject<UClass>(nullptr, *InPath))
		{
			return DirectObj;
		}
		// Normalize a Blueprint object path to its generated-class form: append `_C`
		// to the asset-name component after the trailing '.'.
		FString Normalized = InPath;
		if (!Normalized.EndsWith(TEXT("_C")))
		{
			int32 DotIdx = INDEX_NONE;
			if (Normalized.FindLastChar(TEXT('.'), DotIdx))
			{
				Normalized += TEXT("_C");
			}
			else
			{
				// "/Game/.../BP_Foo" with no '.' — append ".<name>_C".
				int32 SlashIdx = INDEX_NONE;
				if (Normalized.FindLastChar(TEXT('/'), SlashIdx))
				{
					const FString AssetName = Normalized.RightChop(SlashIdx + 1);
					Normalized += FString::Printf(TEXT(".%s_C"), *AssetName);
				}
			}
		}
		if (Normalized != InPath)
		{
			if (UClass* Gen = LoadClass<UObject>(nullptr, *Normalized))
			{
				return Gen;
			}
			if (UClass* GenObj = LoadObject<UClass>(nullptr, *Normalized))
			{
				return GenObj;
			}
		}
		return nullptr;
	}

	// Apply a single JSON value onto one already-addressed FProperty slot (the actor's
	// own member, OR an array element via FScriptArrayHelper::GetRawPtr). Handles every
	// scalar leaf type plus class/soft-class/object/soft-object refs. Returns true on a
	// successful set; on an unsupported type, returns false and fills OutWhyUnsupported.
	// Container types (array) are handled by the caller, not here.
	//
	// CLASS-property ordering note: FClassProperty derives from FObjectProperty and
	// FSoftClassProperty derives from FSoftObjectProperty, so the class branches MUST be
	// tested BEFORE the generic object/soft-object branch — otherwise a class path is
	// imported as an OBJECT path (resolving to the UBlueprint, not its generated UClass).
	static bool TryApplyLeaf(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, UObject* Owner, FString& OutWhyUnsupported)
	{
		if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			FloatProp->SetPropertyValue(ValuePtr, Value->AsNumber());
		}
		else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			DoubleProp->SetPropertyValue(ValuePtr, Value->AsNumber());
		}
		else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(Value->AsNumber()));
		}
		else if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			BoolProp->SetPropertyValue(ValuePtr, Value->AsBool());
		}
		else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			StrProp->SetPropertyValue(ValuePtr, Value->AsString());
		}
		else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*Value->AsString()));
		}
		else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			// FSoftObjectPath (and any string-importable struct) goes through the
			// reflection text importer — same pattern as MonolithMaterialActions.
			if (StructProp->Struct == TBaseStructure<FSoftObjectPath>::Get())
			{
				const FString PathStr = Value->AsString();
				Prop->ImportText_Direct(*PathStr, ValuePtr, Owner, PPF_None);
			}
			else
			{
				OutWhyUnsupported = TEXT("unsupported struct ") + StructProp->Struct->GetName();
				return false;
			}
		}
		// CLASS / SOFTCLASS refs — resolve the class path (with `_C` normalization) and
		// store the UClass* via SetObjectPropertyValue (the authoritative engine pattern,
		// PyConversion.cpp:838-873). MUST precede the generic object branch (see note above).
		else if (FClassProperty* ClassProp = CastField<FClassProperty>(Prop))
		{
			UClass* Resolved = ResolveClassPath(Value->AsString());
			if (!Resolved)
			{
				OutWhyUnsupported = TEXT("could not resolve class path '") + Value->AsString() + TEXT("'");
				return false;
			}
			ClassProp->SetObjectPropertyValue(ValuePtr, Resolved);
		}
		else if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
		{
			UClass* Resolved = ResolveClassPath(Value->AsString());
			if (!Resolved)
			{
				OutWhyUnsupported = TEXT("could not resolve class path '") + Value->AsString() + TEXT("'");
				return false;
			}
			SoftClassProp->SetObjectPropertyValue(ValuePtr, Resolved);
		}
		else if (CastField<FSoftObjectProperty>(Prop) || CastField<FObjectProperty>(Prop))
		{
			// Asset reference by path string (e.g. an animation DB or mesh).
			const FString PathStr = Value->AsString();
			Prop->ImportText_Direct(*PathStr, ValuePtr, Owner, PPF_None);
		}
		else
		{
			OutWhyUnsupported = TEXT("unsupported type ") + Prop->GetClass()->GetName();
			return false;
		}
		return true;
	}

	static bool TryApplyJsonValueToProperty(FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, UObject* Owner, FString& OutWhyUnsupported)
	{
		if (!Prop || !Value.IsValid())
		{
			OutWhyUnsupported = TEXT("property and value are required");
			return false;
		}

		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
			if (!Value->TryGetArray(JsonArray) || !JsonArray)
			{
				OutWhyUnsupported = TEXT("expected JSON array");
				return false;
			}

			FScriptArrayHelper Helper(ArrayProp, ValuePtr);
			Helper.EmptyValues(JsonArray->Num());
			if (JsonArray->Num() > 0)
			{
				Helper.AddUninitializedValues(JsonArray->Num());
			}

			for (int32 Index = 0; Index < JsonArray->Num(); ++Index)
			{
				uint8* ElemPtr = Helper.GetRawPtr(Index);
				ArrayProp->Inner->InitializeValue(ElemPtr);
				if (!TryApplyLeaf(ArrayProp->Inner, ElemPtr, (*JsonArray)[Index], Owner, OutWhyUnsupported))
				{
					OutWhyUnsupported = FString::Printf(TEXT("array element %d: %s"), Index, *OutWhyUnsupported);
					return false;
				}
			}
			return true;
		}

		return TryApplyLeaf(Prop, ValuePtr, Value, Owner, OutWhyUnsupported);
	}

	// Apply a JSON "properties" object onto a spawned actor reflectively. Supports
	// float/double, int, bool, string/name, FSoftObjectPath (string value), object /
	// soft-object refs (asset path string), CLASS / SOFTCLASS refs (class path, `_C`
	// normalized), and arrays whose inner element is any of the above leaf types.
	// Unknown / unsupported properties are recorded in OutSkipped, never fatal.
	static void ApplyProperties(AActor* Actor, const TSharedPtr<FJsonObject>& PropObj, TArray<FString>& OutApplied, TArray<FString>& OutSkipped)
	{
		if (!Actor || !PropObj.IsValid())
		{
			return;
		}
		for (const auto& Pair : FMonolithJsonUtils::GetFields(PropObj))
		{
			const FString PropName = Pair.Key;
			const TSharedPtr<FJsonValue>& Value = Pair.Value;
			FProperty* Prop = Actor->GetClass()->FindPropertyByName(FName(*PropName));
			if (!Prop)
			{
				OutSkipped.Add(PropName + TEXT(" (not found)"));
				continue;
			}
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);

			// Array of object / soft-object / class / soft-class / scalar leaf elements.
			// Mirrors the verified FScriptArrayHelper write pattern in
			// MonolithReflectionWalker.cpp (ctor UnrealType.h:4455, EmptyValues,
			// AddUninitializedValues, GetRawPtr) + the object-inner set in
			// MonolithAIStateTreeActions.cpp:2622-2627.
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
				if (!Value->TryGetArray(JsonArray) || !JsonArray)
				{
					OutSkipped.Add(PropName + TEXT(" (expected JSON array)"));
					continue;
				}

				FScriptArrayHelper Helper(ArrayProp, ValuePtr);
				Helper.EmptyValues(JsonArray->Num());
				if (JsonArray->Num() > 0)
				{
					Helper.AddUninitializedValues(JsonArray->Num());
				}

				int32 ElemErrors = 0;
				FString FirstWhy;
				for (int32 i = 0; i < JsonArray->Num(); ++i)
				{
					uint8* ElemPtr = Helper.GetRawPtr(i);
					ArrayProp->Inner->InitializeValue(ElemPtr);
					FString Why;
					if (!TryApplyLeaf(ArrayProp->Inner, ElemPtr, (*JsonArray)[i], Actor, Why))
					{
						++ElemErrors;
						if (FirstWhy.IsEmpty()) { FirstWhy = Why; }
					}
				}

				if (ElemErrors > 0)
				{
					OutSkipped.Add(FString::Printf(TEXT("%s (%d/%d array elements failed: %s)"),
						*PropName, ElemErrors, JsonArray->Num(), *FirstWhy));
					continue;
				}
				OutApplied.Add(PropName);
				continue;
			}

			FString Why;
			if (!TryApplyLeaf(Prop, ValuePtr, Value, Actor, Why))
			{
				OutSkipped.Add(PropName + TEXT(" (") + Why + TEXT(")"));
				continue;
			}
			OutApplied.Add(PropName);
		}
	}

	// Shared map-authoring helpers for both create_nav_harness_map and
	// editor.author_map_settings. Applied to whatever editor world the caller passes —
	// they ONLY mutate (and dirty) on an actual change.

	// Set AWorldSettings::DefaultGameMode ("GameMode Override") from a class path.
	// Idiom: Modify()-then-assign (WorldSettingsDetails.cpp:71). Returns false + reason
	// on a resolution failure WITHOUT touching the package. CONFIRMED member type is
	// TSubclassOf<AGameModeBase> (WorldSettings.h:599).
	//
	// Compare-before-dirty: if the resolved class already equals WS->DefaultGameMode,
	// this is a no-op — no Modify(), no assignment, no MarkPackageDirty — and OutChanged
	// is set false. Only an actual class difference mutates (and dirties) the package.
	// Returns true on success (whether or not a change occurred); false only on a
	// resolution/validation failure (in which case the package is never touched).
	static bool ApplyGameModeOverride(UWorld* World, const FString& ClassPath, FString& OutResolved, bool& OutChanged, FString& OutError)
	{
		OutChanged = false;
		AWorldSettings* WS = World ? World->GetWorldSettings() : nullptr;
		if (!WS)
		{
			OutError = TEXT("no AWorldSettings on the target world");
			return false;
		}
		UClass* GameModeClass = ResolveClassPath(ClassPath);
		if (!GameModeClass || !GameModeClass->IsChildOf(AGameModeBase::StaticClass()))
		{
			OutError = FString::Printf(TEXT("'%s' did not resolve to an AGameModeBase subclass"), *ClassPath);
			return false;
		}
		OutResolved = GameModeClass->GetPathName();

		// Already the configured GameMode override? Skip the mutate entirely so a
		// re-apply of an identical class does not re-dirty the package.
		if (WS->DefaultGameMode == GameModeClass)
		{
			return true;
		}

		WS->Modify();
		WS->DefaultGameMode = GameModeClass;
		WS->MarkPackageDirty();
		OutChanged = true;
		return true;
	}

	// Spawn APlayerStart actors at each transform in a JSON array of
	// {location:[x,y,z], rotation:[p,y,r]} objects. Appends one report row per spawn to
	// OutRows and returns the spawned count. Only dirties packages it actually creates.
	static bool SpawnPlayerStarts(UWorld* World, const TArray<TSharedPtr<FJsonValue>>& StartsArr,
		const FActorSpawnParameters& SpawnParams, TArray<TSharedPtr<FJsonValue>>& OutRows,
		int32& OutCount, FString& OutError)
	{
		OutCount = 0;
		for (const TSharedPtr<FJsonValue>& Val : StartsArr)
		{
			const TSharedPtr<FJsonObject> StartObj = Val.IsValid() ? Val->AsObject() : nullptr;
			FVector Loc = FVector::ZeroVector;
			FVector Rot = FVector::ZeroVector;
			if (StartObj.IsValid())
			{
				if (!ParseVec3Param(StartObj, TEXT("location"), Loc, OutError) ||
					!ParseVec3Param(StartObj, TEXT("rotation"), Rot, OutError))
				{
					return false;
				}
			}

			APlayerStart* Start = World->SpawnActor<APlayerStart>(APlayerStart::StaticClass(), Loc,
				FRotator(Rot.X, Rot.Y, Rot.Z), SpawnParams);
			if (!Start)
			{
				continue;
			}
			FString Label = FString::Printf(TEXT("Harness_PlayerStart_%d"), OutCount);
			if (StartObj.IsValid())
			{
				StartObj->TryGetStringField(TEXT("name"), Label);
			}
			Start->SetActorLabel(Label);
			Start->SetFolderPath(TEXT("Harness/PlayerStarts"));
			Start->MarkPackageDirty();

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), TEXT("player_start"));
			Row->SetStringField(TEXT("name"), Start->GetActorNameOrLabel());
			Row->SetArrayField(TEXT("location"), Vec3ToJson(Loc));
			OutRows.Add(MakeShared<FJsonValueObject>(Row));
			++OutCount;
		}
		return true;
	}
}

FMonolithActionResult FMonolithEditorActions::HandleCreateNavHarnessMap(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorNavHarness;

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("create_nav_harness_map requires editor context (GEditor)."));
	}

	FString MapPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("path"), MapPath) || MapPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required parameter: path"));
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// 1. Create the blank UWorld via the existing editor.create_empty_map action,
	//    then load it as the active editor world so spawns + nav target it.
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("path"), MapPath);
		const FMonolithActionResult CreateRes = Registry.ExecuteAction(TEXT("editor"), TEXT("create_empty_map"), CreateParams);
		if (!CreateRes.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("create_empty_map failed: %s"), *CreateRes.ErrorMessage));
		}
	}

	ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEd || !LevelEd->LoadLevel(MapPath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Created map but failed to load '%s' as the editor world."), *MapPath));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world after loading the harness map."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SpawnedActors;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 2. Floor — default 50x50m plane unless overridden.
	{
		FVector FloorLoc = FVector::ZeroVector;
		FVector FloorScale(50.0f, 50.0f, 1.0f);
		FString FloorMeshPath = TEXT("/Engine/BasicShapes/Plane.Plane");

		const TSharedPtr<FJsonObject>* FloorObj = nullptr;
		if (Params->TryGetObjectField(TEXT("floor"), FloorObj) && FloorObj)
		{
			FString VecError;
			if (!ParseVec3Param(*FloorObj, TEXT("location"), FloorLoc, VecError) ||
				!ParseVec3Param(*FloorObj, TEXT("scale"), FloorScale, VecError))
			{
				return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
			}
			FString MeshOverride;
			if ((*FloorObj)->TryGetStringField(TEXT("mesh"), MeshOverride) && !MeshOverride.IsEmpty())
			{
				FloorMeshPath = MeshOverride;
			}
		}

		AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FloorLoc, FRotator::ZeroRotator, SpawnParams);
		if (Floor)
		{
			if (UStaticMeshComponent* FloorComp = Floor->GetStaticMeshComponent())
			{
				FloorComp->SetMobility(EComponentMobility::Static);
				if (UStaticMesh* FloorMesh = FMonolithAssetUtils::LoadAssetByPath<UStaticMesh>(*FloorMeshPath))
				{
					FloorComp->SetStaticMesh(FloorMesh);
				}
			}
			Floor->SetActorScale3D(FloorScale);
			Floor->SetActorLabel(TEXT("Harness_Floor"));
			Floor->SetFolderPath(TEXT("Harness"));
			Floor->MarkPackageDirty();

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("kind"), TEXT("floor"));
			Row->SetStringField(TEXT("name"), Floor->GetActorNameOrLabel());
			SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));
		}
	}

	// 3. Camera (optional).
	{
		const TSharedPtr<FJsonObject>* CamObj = nullptr;
		if (Params->TryGetObjectField(TEXT("camera"), CamObj) && CamObj)
		{
			FVector CamLoc(0.0f, 0.0f, 1000.0f);
			FVector CamRot(-60.0f, 0.0f, 0.0f);
			FString VecError;
			if (!ParseVec3Param(*CamObj, TEXT("location"), CamLoc, VecError) ||
				!ParseVec3Param(*CamObj, TEXT("rotation"), CamRot, VecError))
			{
				return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
			}

			ACameraActor* Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), CamLoc,
				FRotator(CamRot.X, CamRot.Y, CamRot.Z), SpawnParams);
			if (Cam)
			{
				Cam->SetActorLabel(TEXT("Harness_Camera"));
				Cam->SetFolderPath(TEXT("Harness"));
				Cam->MarkPackageDirty();

				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("kind"), TEXT("camera"));
				Row->SetStringField(TEXT("name"), Cam->GetActorNameOrLabel());
				SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
	}

	// 4. Target points (also feed nav validation).
	TArray<TSharedPtr<FJsonValue>> NavPoints;
	{
		const TArray<TSharedPtr<FJsonValue>>* TpArr = nullptr;
		if (Params->TryGetArrayField(TEXT("target_points"), TpArr) && TpArr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *TpArr)
			{
				const TSharedPtr<FJsonObject> TpObj = Val.IsValid() ? Val->AsObject() : nullptr;
				if (!TpObj.IsValid())
				{
					continue;
				}
				FString Name;
				TpObj->TryGetStringField(TEXT("name"), Name);
				FVector Loc = FVector::ZeroVector;
				FString VecError;
				if (!ParseVec3Param(TpObj, TEXT("location"), Loc, VecError))
				{
					return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
				}

				ATargetPoint* Tp = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), Loc, FRotator::ZeroRotator, SpawnParams);
				if (Tp)
				{
					if (!Name.IsEmpty()) { Tp->SetActorLabel(Name); }
					Tp->SetFolderPath(TEXT("Harness/Targets"));
					Tp->MarkPackageDirty();

					TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("kind"), TEXT("target_point"));
					Row->SetStringField(TEXT("name"), Tp->GetActorNameOrLabel());
					SpawnedActors.Add(MakeShared<FJsonValueObject>(Row));

					// Mirror into the nav-validation point list.
					TSharedPtr<FJsonObject> NavPt = MakeShared<FJsonObject>();
					NavPt->SetStringField(TEXT("name"), Name.IsEmpty() ? Tp->GetActorNameOrLabel() : Name);
					NavPt->SetArrayField(TEXT("location"), Vec3ToJson(Loc));
					NavPoints.Add(MakeShared<FJsonValueObject>(NavPt));
				}
			}
		}
	}

	// 5. Actor instances with BP class paths + reflective UPROPERTY defaults.
	TArray<TSharedPtr<FJsonValue>> ActorReports;
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("actors"), ActorsArr) && ActorsArr)
		{
			ActorReports.Reserve(ActorsArr->Num());
			for (const TSharedPtr<FJsonValue>& Val : *ActorsArr)
			{
				const TSharedPtr<FJsonObject> ActorObj = Val.IsValid() ? Val->AsObject() : nullptr;
				if (!ActorObj.IsValid())
				{
					continue;
				}
				FString ClassPath;
				if (!ActorObj->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
				{
					continue;
				}

				UClass* ActorClass = LoadClass<AActor>(nullptr, *ClassPath);
				if (!ActorClass)
				{
					// Tolerate _C-suffix omission by trying the generated-class path.
					ActorClass = LoadObject<UClass>(nullptr, *ClassPath);
				}

				TSharedPtr<FJsonObject> ActorRow = MakeShared<FJsonObject>();
				ActorRow->SetStringField(TEXT("class"), ClassPath);
				if (!ActorClass)
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("could not resolve actor class"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FVector Loc = FVector::ZeroVector;
				FVector Rot = FVector::ZeroVector;
				FString VecError;
				if (!ParseVec3Param(ActorObj, TEXT("location"), Loc, VecError) ||
					!ParseVec3Param(ActorObj, TEXT("rotation"), Rot, VecError))
				{
					return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
				}

				AActor* Spawned = World->SpawnActor<AActor>(ActorClass, Loc,
					FRotator(Rot.X, Rot.Y, Rot.Z), SpawnParams);
				if (!Spawned)
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("SpawnActor returned null"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FString Folder = TEXT("Harness/Actors");
				ActorObj->TryGetStringField(TEXT("folder"), Folder);
				Spawned->SetFolderPath(FName(*Folder));

				TArray<FString> Applied, Skipped;
				const TSharedPtr<FJsonObject>* PropObj = nullptr;
				if (ActorObj->TryGetObjectField(TEXT("properties"), PropObj) && PropObj)
				{
					ApplyProperties(Spawned, *PropObj, Applied, Skipped);
				}
				Spawned->MarkPackageDirty();

				ActorRow->SetBoolField(TEXT("spawned"), true);
				ActorRow->SetStringField(TEXT("name"), Spawned->GetActorNameOrLabel());
				ActorRow->SetNumberField(TEXT("properties_applied"), Applied.Num());
				if (Skipped.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SkippedJson;
					SkippedJson.Reserve(Skipped.Num());
					for (const FString& S : Skipped) { SkippedJson.Add(MakeShared<FJsonValueString>(S)); }
					ActorRow->SetArrayField(TEXT("properties_skipped"), SkippedJson);
				}
				ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
			}
		}
	}

	// 5b. WorldSettings GameMode override (optional). Only dirties on a real change.
	{
		FString GameModePath;
		if (Params->TryGetStringField(TEXT("game_mode_override"), GameModePath) && !GameModePath.IsEmpty())
		{
			FString Resolved, Error;
			bool bChanged = false;
			if (ApplyGameModeOverride(World, GameModePath, Resolved, bChanged, Error))
			{
				Result->SetBoolField(TEXT("game_mode_override_set"), true);
				Result->SetBoolField(TEXT("game_mode_override_changed"), bChanged);
				Result->SetStringField(TEXT("game_mode_override"), Resolved);
			}
			else
			{
				Result->SetBoolField(TEXT("game_mode_override_set"), false);
				Result->SetBoolField(TEXT("game_mode_override_changed"), false);
				Result->SetStringField(TEXT("game_mode_override_error"), Error);
			}
		}
	}

	// 5c. PlayerStart actors (optional).
	{
		const TArray<TSharedPtr<FJsonValue>>* StartsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("player_starts"), StartsArr) && StartsArr)
		{
			int32 Spawned = 0;
			FString VecError;
			if (!SpawnPlayerStarts(World, *StartsArr, SpawnParams, SpawnedActors, Spawned, VecError))
			{
				return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
			}
			Result->SetNumberField(TEXT("player_starts_spawned"), Spawned);
		}
	}

	// 6. Nav bounds — default sized to the floor footprint unless overridden.
	{
		FVector NavLoc = FVector::ZeroVector;
		FVector NavExtent(3000.0f, 3000.0f, 500.0f);
		const TSharedPtr<FJsonObject>* NavObj = nullptr;
		if (Params->TryGetObjectField(TEXT("nav_bounds"), NavObj) && NavObj)
		{
			FString VecError;
			if (!ParseVec3Param(*NavObj, TEXT("location"), NavLoc, VecError) ||
				!ParseVec3Param(*NavObj, TEXT("extent"), NavExtent, VecError))
			{
				return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		TSharedPtr<FJsonObject> NavParams = MakeShared<FJsonObject>();
		NavParams->SetArrayField(TEXT("location"), Vec3ToJson(NavLoc));
		NavParams->SetArrayField(TEXT("extent"), Vec3ToJson(NavExtent));
		NavParams->SetStringField(TEXT("folder_path"), TEXT("Harness/Navigation"));
		const FMonolithActionResult NavRes = Registry.ExecuteAction(TEXT("ai"), TEXT("add_nav_bounds_volume"), NavParams);
		Result->SetBoolField(TEXT("nav_bounds_added"), NavRes.bSuccess);
		if (!NavRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_bounds_error"), NavRes.ErrorMessage);
		}
	}

	// 7. Rebuild navigation (delegated to ai.rebuild_navigation, which bound-waits
	//    for async tile generation).
	{
		double NavTimeout = 30.0;
		if (Params->HasField(TEXT("nav_timeout")))
		{
			if (!Params->TryGetNumberField(TEXT("nav_timeout"), NavTimeout))
			{
				return FMonolithActionResult::Error(TEXT("nav_timeout must be a number"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}

		TSharedPtr<FJsonObject> RebuildParams = MakeShared<FJsonObject>();
		RebuildParams->SetBoolField(TEXT("save_after"), false); // we save the level explicitly below
		RebuildParams->SetNumberField(TEXT("timeout_seconds"), NavTimeout);
		const FMonolithActionResult RebuildRes = Registry.ExecuteAction(TEXT("ai"), TEXT("rebuild_navigation"), RebuildParams);
		Result->SetBoolField(TEXT("nav_rebuilt"), RebuildRes.bSuccess);
		if (RebuildRes.bSuccess && RebuildRes.Result.IsValid())
		{
			Result->SetObjectField(TEXT("nav_rebuild"), RebuildRes.Result);
		}
		else if (!RebuildRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_rebuild_error"), RebuildRes.ErrorMessage);
		}
	}

	// 8. Validate nav points + requested path pairs (delegated to ai.validate_nav_points).
	if (NavPoints.Num() > 0)
	{
		TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
		ValidateParams->SetArrayField(TEXT("points"), NavPoints);
		const TArray<TSharedPtr<FJsonValue>>* PairsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("validate_pairs"), PairsArr) && PairsArr)
		{
			ValidateParams->SetArrayField(TEXT("require_path_pairs"), *PairsArr);
		}
		const FMonolithActionResult ValidateRes = Registry.ExecuteAction(TEXT("ai"), TEXT("validate_nav_points"), ValidateParams);
		Result->SetBoolField(TEXT("nav_validated"), ValidateRes.bSuccess);
		if (ValidateRes.Result.IsValid())
		{
			Result->SetObjectField(TEXT("nav_validation"), ValidateRes.Result);
		}
		else if (!ValidateRes.bSuccess)
		{
			Result->SetStringField(TEXT("nav_validation_error"), ValidateRes.ErrorMessage);
		}
	}

	// 9. Save the level package now that actors + nav exist.
	{
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PkgArr;
		PkgArr.Add(MakeShared<FJsonValueString>(MapPath));
		SaveParams->SetArrayField(TEXT("packages"), PkgArr);
		const FMonolithActionResult SaveRes = Registry.ExecuteAction(TEXT("editor"), TEXT("save_packages"), SaveParams);
		Result->SetBoolField(TEXT("saved"), SaveRes.bSuccess);
		if (!SaveRes.bSuccess)
		{
			Result->SetStringField(TEXT("save_error"), SaveRes.ErrorMessage);
		}
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("path"), MapPath);
	Result->SetArrayField(TEXT("spawned_actors"), SpawnedActors);
	Result->SetArrayField(TEXT("actor_instances"), ActorReports);
	return FMonolithActionResult::Success(Result);
}

// ---------------------------------------------------------------------------
// Phase 10 (OG-E4): author_map_settings — generic map post-authoring.
// Sets the WorldSettings GameMode override + spawns APlayerStart actors (and optional
// generic actor instances with reflective UPROPERTY defaults) on the currently-open
// editor world, or on a `path`-specified map loaded first. Map mutation dirties the
// package by design; we only dirty on actual change and save only when asked.
// ---------------------------------------------------------------------------
FMonolithActionResult FMonolithEditorActions::HandleAuthorMapSettings(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorNavHarness;

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("author_map_settings requires editor context (GEditor)."));
	}
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("author_map_settings requires a params object."));
	}

	// Resolve the target world: load `path` as the active editor world if provided,
	// otherwise author whatever map is currently open.
	FString MapPath;
	const bool bHasPath = Params->TryGetStringField(TEXT("path"), MapPath) && !MapPath.IsEmpty();
	if (bHasPath)
	{
		ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEd || !LevelEd->LoadLevel(MapPath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to load '%s' as the editor world."), *MapPath));
		}
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world to author."));
	}

	// Require at least one authoring directive so a no-op call can't silently dirty.
	const bool bHasGameMode = Params->HasField(TEXT("game_mode_override"));
	const bool bHasStarts = Params->HasField(TEXT("player_starts"));
	const bool bHasActors = Params->HasField(TEXT("actors"));
	if (!bHasGameMode && !bHasStarts && !bHasActors)
	{
		return FMonolithActionResult::Error(
			TEXT("author_map_settings requires at least one of: game_mode_override, player_starts, actors."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SpawnedActors;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// 1. GameMode override.
	if (bHasGameMode)
	{
		FString GameModePath;
		Params->TryGetStringField(TEXT("game_mode_override"), GameModePath);
		FString Resolved, Error;
		bool bChanged = false;
		if (!GameModePath.IsEmpty() && ApplyGameModeOverride(World, GameModePath, Resolved, bChanged, Error))
		{
			Result->SetBoolField(TEXT("game_mode_override_set"), true);
			Result->SetBoolField(TEXT("game_mode_override_changed"), bChanged);
			Result->SetStringField(TEXT("game_mode_override"), Resolved);
		}
		else
		{
			Result->SetBoolField(TEXT("game_mode_override_set"), false);
			Result->SetBoolField(TEXT("game_mode_override_changed"), false);
			Result->SetStringField(TEXT("game_mode_override_error"),
				GameModePath.IsEmpty() ? TEXT("empty class path") : Error);
		}
	}

	// 2. PlayerStart actors.
	if (bHasStarts)
	{
		const TArray<TSharedPtr<FJsonValue>>* StartsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("player_starts"), StartsArr) && StartsArr)
		{
			int32 Spawned = 0;
			FString VecError;
			if (!SpawnPlayerStarts(World, *StartsArr, SpawnParams, SpawnedActors, Spawned, VecError))
			{
				return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
			}
			Result->SetNumberField(TEXT("player_starts_spawned"), Spawned);
		}
	}

	// 3. Generic actor instances (native or Blueprint) with reflective property defaults.
	TArray<TSharedPtr<FJsonValue>> ActorReports;
	if (bHasActors)
	{
		const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("actors"), ActorsArr) && ActorsArr)
		{
			ActorReports.Reserve(ActorsArr->Num());
			for (const TSharedPtr<FJsonValue>& Val : *ActorsArr)
			{
				const TSharedPtr<FJsonObject> ActorObj = Val.IsValid() ? Val->AsObject() : nullptr;
				if (!ActorObj.IsValid())
				{
					continue;
				}
				FString ClassPath;
				if (!ActorObj->TryGetStringField(TEXT("class"), ClassPath) || ClassPath.IsEmpty())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ActorRow = MakeShared<FJsonObject>();
				ActorRow->SetStringField(TEXT("class"), ClassPath);

				UClass* ActorClass = ResolveClassPath(ClassPath);
				if (!ActorClass || !ActorClass->IsChildOf(AActor::StaticClass()))
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("could not resolve actor class"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FVector Loc = FVector::ZeroVector;
				FVector Rot = FVector::ZeroVector;
				FString VecError;
				if (!ParseVec3Param(ActorObj, TEXT("location"), Loc, VecError) ||
					!ParseVec3Param(ActorObj, TEXT("rotation"), Rot, VecError))
				{
					return FMonolithActionResult::Error(VecError, FMonolithJsonUtils::ErrInvalidParams);
				}

				AActor* Spawned = World->SpawnActor<AActor>(ActorClass, Loc,
					FRotator(Rot.X, Rot.Y, Rot.Z), SpawnParams);
				if (!Spawned)
				{
					ActorRow->SetBoolField(TEXT("spawned"), false);
					ActorRow->SetStringField(TEXT("error"), TEXT("SpawnActor returned null"));
					ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
					continue;
				}

				FString Folder;
				if (ActorObj->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
				{
					Spawned->SetFolderPath(FName(*Folder));
				}

				TArray<FString> Applied, Skipped;
				const TSharedPtr<FJsonObject>* PropObj = nullptr;
				if (ActorObj->TryGetObjectField(TEXT("properties"), PropObj) && PropObj)
				{
					ApplyProperties(Spawned, *PropObj, Applied, Skipped);
				}
				Spawned->MarkPackageDirty();

				ActorRow->SetBoolField(TEXT("spawned"), true);
				ActorRow->SetStringField(TEXT("name"), Spawned->GetActorNameOrLabel());
				ActorRow->SetNumberField(TEXT("properties_applied"), Applied.Num());
				if (Skipped.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SkippedJson;
					SkippedJson.Reserve(Skipped.Num());
					for (const FString& S : Skipped) { SkippedJson.Add(MakeShared<FJsonValueString>(S)); }
					ActorRow->SetArrayField(TEXT("properties_skipped"), SkippedJson);
				}
				ActorReports.Add(MakeShared<FJsonValueObject>(ActorRow));
			}
		}
	}

	// 4. Optional save — apply-then-save atomicity per the harness flow (save only after
	//    all mutations land, never mid-apply).
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	if (bSave)
	{
		const FString SavePath = bHasPath ? MapPath : World->GetOutermost()->GetName();
		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PkgArr;
		PkgArr.Add(MakeShared<FJsonValueString>(SavePath));
		SaveParams->SetArrayField(TEXT("packages"), PkgArr);
		const FMonolithActionResult SaveRes =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("save_packages"), SaveParams);
		Result->SetBoolField(TEXT("saved"), SaveRes.bSuccess);
		if (!SaveRes.bSuccess)
		{
			Result->SetStringField(TEXT("save_error"), SaveRes.ErrorMessage);
		}
	}

	Result->SetBoolField(TEXT("ok"), true);
	Result->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
	Result->SetArrayField(TEXT("spawned_actors"), SpawnedActors);
	if (ActorReports.Num() > 0)
	{
		Result->SetArrayField(TEXT("actor_instances"), ActorReports);
	}
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleSetWorldSettingsProperty(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithEditorNavHarness;

	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("set_world_settings_property requires editor context (GEditor)."));
	}
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("set_world_settings_property requires a params object."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'property_name'."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
	if (!Value.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param 'value'."), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bDryRun = false;
	bool bSave = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	Params->TryGetBoolField(TEXT("save"), bSave);

	FString MapPath;
	const bool bHasPath = Params->TryGetStringField(TEXT("path"), MapPath) && !MapPath.IsEmpty();
	if (bHasPath)
	{
		ULevelEditorSubsystem* LevelEd = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
		if (!LevelEd || !LevelEd->LoadLevel(MapPath))
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Failed to load '%s' as the editor world."), *MapPath));
		}
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	AWorldSettings* WorldSettings = World ? World->GetWorldSettings() : nullptr;
	if (!WorldSettings)
	{
		return FMonolithActionResult::Error(TEXT("No editor world settings object is available."));
	}

	FProperty* Property = WorldSettings->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Property)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("WorldSettings class '%s' has no reflected property named '%s'."),
				*WorldSettings->GetClass()->GetPathName(),
				*PropertyName),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	if (Property->HasAnyPropertyFlags(CPF_Transient))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Property '%s' is transient and cannot be authored onto the map package."), *PropertyName),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(WorldSettings);
	FString BeforeExport;
	Property->ExportTextItem_Direct(BeforeExport, ValuePtr, nullptr, WorldSettings, PPF_None);

	void* ScratchPtr = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
	Property->InitializeValue(ScratchPtr);
	ON_SCOPE_EXIT
	{
		Property->DestroyValue(ScratchPtr);
		FMemory::Free(ScratchPtr);
	};
	Property->CopyCompleteValue(ScratchPtr, ValuePtr);

	FString WhyUnsupported;
	if (!TryApplyJsonValueToProperty(Property, ScratchPtr, Value, WorldSettings, WhyUnsupported))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Could not apply value to '%s': %s"), *PropertyName, *WhyUnsupported),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FString AfterExport;
	Property->ExportTextItem_Direct(AfterExport, ScratchPtr, nullptr, WorldSettings, PPF_None);
	const bool bChanged = !BeforeExport.Equals(AfterExport, ESearchCase::CaseSensitive);

	bool bSaved = false;
	if (bChanged && !bDryRun)
	{
		WorldSettings->Modify();
		WhyUnsupported.Reset();
		if (!TryApplyJsonValueToProperty(Property, ValuePtr, Value, WorldSettings, WhyUnsupported))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("Could not apply value to '%s': %s"), *PropertyName, *WhyUnsupported),
				FMonolithJsonUtils::ErrInvalidParams);
		}

		FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
		WorldSettings->PostEditChangeProperty(ChangedEvent);
		WorldSettings->MarkPackageDirty();
		if (World)
		{
			World->MarkPackageDirty();
		}

		if (bSave)
		{
			const FString SavePath = World->GetOutermost()->GetName();
			TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> PkgArr;
			PkgArr.Add(MakeShared<FJsonValueString>(SavePath));
			SaveParams->SetArrayField(TEXT("packages"), PkgArr);
			const FMonolithActionResult SaveRes =
				FMonolithToolRegistry::Get().ExecuteAction(TEXT("editor"), TEXT("save_packages"), SaveParams);
			bSaved = SaveRes.bSuccess;
			if (!SaveRes.bSuccess)
			{
				return FMonolithActionResult::Error(SaveRes.ErrorMessage, -32603);
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), true);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
	Result->SetStringField(TEXT("world_settings_class"), WorldSettings->GetClass()->GetPathName());
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("property_type"), Property->GetClass()->GetName());
	Result->SetStringField(TEXT("before"), BeforeExport);
	Result->SetStringField(TEXT("after"), AfterExport);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithEditorActions::HandleValidateAssets(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return FMonolithActionResult::Error(TEXT("validate_assets requires editor context (GEditor)."));
	}
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("validate_assets requires a params object."), FMonolithJsonUtils::ErrInvalidParams);
	}

	auto ResultToString = [](EDataValidationResult Result)
	{
		switch (Result)
		{
		case EDataValidationResult::Invalid: return FString(TEXT("invalid"));
		case EDataValidationResult::Valid: return FString(TEXT("valid"));
		case EDataValidationResult::NotValidated: return FString(TEXT("not_validated"));
		default: return FString(TEXT("unknown"));
		}
	};

	auto ParseUsecase = [](FString Usecase)
	{
		Usecase.TrimStartAndEndInline();
		Usecase.ToLowerInline();
		if (Usecase == TEXT("manual")) { return EDataValidationUsecase::Manual; }
		if (Usecase == TEXT("save")) { return EDataValidationUsecase::Save; }
		if (Usecase == TEXT("pre_submit") || Usecase == TEXT("presubmit")) { return EDataValidationUsecase::PreSubmit; }
		if (Usecase == TEXT("script")) { return EDataValidationUsecase::Script; }
		if (Usecase == TEXT("none")) { return EDataValidationUsecase::None; }
		return EDataValidationUsecase::Commandlet;
	};

	auto AddStringArray = [](const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, TArray<FString>& OutValues, FString& OutError) -> bool
	{
		if (!Source->HasField(FieldName))
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Source->TryGetArrayField(FieldName, Values) || !Values)
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
	};

	bool bValidateProjectSettings = false;
	Params->TryGetBoolField(TEXT("validate_project_settings"), bValidateProjectSettings);

	FString Error;
	TArray<FString> RequestedPaths;
	if (!AddStringArray(Params, TEXT("asset_paths"), RequestedPaths, Error)
		|| !AddStringArray(Params, TEXT("packages"), RequestedPaths, Error))
	{
		return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray<FAssetData> AssetsToValidate;
	TSet<FString> SeenObjectPaths;
	auto AddAssetData = [&AssetsToValidate, &SeenObjectPaths](const FAssetData& AssetData)
	{
		if (!AssetData.IsValid())
		{
			return;
		}
		const FString ObjectPath = AssetData.GetObjectPathString();
		if (!SeenObjectPaths.Contains(ObjectPath))
		{
			SeenObjectPaths.Add(ObjectPath);
			AssetsToValidate.Add(AssetData);
		}
	};

	for (const FString& RequestedPath : RequestedPaths)
	{
		FString PackageName = RequestedPath.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(RequestedPath)
			: RequestedPath;
		PackageName.TrimStartAndEndInline();
		if (PackageName.IsEmpty())
		{
			continue;
		}

		TArray<FAssetData> PackageAssets;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), PackageAssets, true);
		for (const FAssetData& AssetData : PackageAssets)
		{
			AddAssetData(AssetData);
		}
	}

	FString PrefixPath;
	if (Params->TryGetStringField(TEXT("path"), PrefixPath) && !PrefixPath.IsEmpty())
	{
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*PrefixPath));
		Filter.bRecursivePaths = true;
		TArray<FAssetData> PrefixAssets;
		AssetRegistry.GetAssets(Filter, PrefixAssets);
		for (const FAssetData& AssetData : PrefixAssets)
		{
			AddAssetData(AssetData);
		}
	}

	if (AssetsToValidate.Num() == 0 && !bValidateProjectSettings)
	{
		return FMonolithActionResult::Error(
			TEXT("No assets resolved for validation. Provide asset_paths, packages, path, or validate_project_settings=true."),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FValidateAssetsSettings Settings;
	Settings.ValidationUsecase = EDataValidationUsecase::Commandlet;
	Settings.bCollectPerAssetDetails = true;
	Settings.bLoadAssetsForValidation = true;
	Settings.bUnloadAssetsLoadedForValidation = false;
	Settings.bLoadExternalObjectsForValidation = true;
	Settings.bCaptureAssetLoadLogs = true;
	Settings.bCaptureLogsDuringValidation = true;
	Settings.bCaptureWarningsDuringValidationAsErrors = false;
	Settings.bSkipExcludedDirectories = true;
	Settings.bSilent = true;
	Settings.ShowMessageLogSeverity.Reset();

	FString UsecaseString = TEXT("commandlet");
	Params->TryGetStringField(TEXT("validation_usecase"), UsecaseString);
	Settings.ValidationUsecase = ParseUsecase(UsecaseString);
	Params->TryGetBoolField(TEXT("collect_per_asset_details"), Settings.bCollectPerAssetDetails);
	Params->TryGetBoolField(TEXT("load_assets"), Settings.bLoadAssetsForValidation);
	Params->TryGetBoolField(TEXT("load_external_objects"), Settings.bLoadExternalObjectsForValidation);
	bool bCaptureLogs = Settings.bCaptureLogsDuringValidation;
	if (Params->TryGetBoolField(TEXT("capture_logs"), bCaptureLogs))
	{
		Settings.bCaptureAssetLoadLogs = bCaptureLogs;
		Settings.bCaptureLogsDuringValidation = bCaptureLogs;
	}
	Params->TryGetBoolField(TEXT("warnings_as_errors"), Settings.bCaptureWarningsDuringValidationAsErrors);
	Params->TryGetBoolField(TEXT("skip_excluded_directories"), Settings.bSkipExcludedDirectories);
	Params->TryGetBoolField(TEXT("silent"), Settings.bSilent);
	double MaxAssetsValue = 0.0;
	if (Params->TryGetNumberField(TEXT("max_assets_to_validate"), MaxAssetsValue))
	{
		Settings.MaxAssetsToValidate = FMath::Max(0, static_cast<int32>(MaxAssetsValue));
	}

	UEditorValidatorSubsystem* ValidatorSubsystem = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
	if (!ValidatorSubsystem)
	{
		return FMonolithActionResult::Error(TEXT("UEditorValidatorSubsystem is unavailable."), -32603);
	}

	FValidateAssetsResults ValidationResults;
	const int32 InvalidOrWarningCount = AssetsToValidate.Num() > 0
		? ValidatorSubsystem->ValidateAssetsWithSettings(AssetsToValidate, Settings, ValidationResults)
		: 0;

	TArray<TSharedPtr<FJsonValue>> AssetRows;
	AssetRows.Reserve(AssetsToValidate.Num());
	for (const FAssetData& AssetData : AssetsToValidate)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), AssetData.GetObjectPathString());
		Row->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		Row->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		AssetRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> DetailRows;
	DetailRows.Reserve(ValidationResults.AssetsDetails.Num());
	for (const TPair<FString, FValidateAssetsDetails>& Pair : ValidationResults.AssetsDetails)
	{
		const FValidateAssetsDetails& Details = Pair.Value;
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("object_path"), Pair.Key);
		Row->SetStringField(TEXT("package_name"), Details.PackageName.ToString());
		Row->SetStringField(TEXT("asset_name"), Details.AssetName.ToString());
		Row->SetStringField(TEXT("result"), ResultToString(Details.Result));

		TArray<TSharedPtr<FJsonValue>> Errors;
		Errors.Reserve(Details.ValidationErrors.Num());
		for (const FText& Text : Details.ValidationErrors)
		{
			Errors.Add(MakeShared<FJsonValueString>(Text.ToString()));
		}
		Row->SetArrayField(TEXT("errors"), Errors);

		TArray<TSharedPtr<FJsonValue>> Warnings;
		Warnings.Reserve(Details.ValidationWarnings.Num());
		for (const FText& Text : Details.ValidationWarnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Text.ToString()));
		}
		Row->SetArrayField(TEXT("warnings"), Warnings);

		TArray<TSharedPtr<FJsonValue>> Messages;
		Messages.Reserve(Details.ValidationMessages.Num());
		for (const TSharedRef<FTokenizedMessage>& Message : Details.ValidationMessages)
		{
			Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
		}
		Row->SetArrayField(TEXT("messages"), Messages);
		DetailRows.Add(MakeShared<FJsonValueObject>(Row));
	}

	TArray<TSharedPtr<FJsonValue>> ValidatorMessages;
	ValidatorMessages.Reserve(ValidationResults.ValidatorMessages.Num());
	for (const TSharedRef<FTokenizedMessage>& Message : ValidationResults.ValidatorMessages)
	{
		ValidatorMessages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
	}

	TSharedPtr<FJsonObject> ProjectSettings = MakeShared<FJsonObject>();
	ProjectSettings->SetBoolField(TEXT("requested"), bValidateProjectSettings);
	ProjectSettings->SetBoolField(TEXT("supported"), false);
	if (bValidateProjectSettings)
	{
		ProjectSettings->SetStringField(TEXT("reason"),
			TEXT("Unreal Engine 5.8 exposes UEditorValidatorSubsystem::ValidateAssetsWithSettings, but no generic ValidateProjectSettings API. Project-specific settings checks must be exposed through a project adapter."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("ok"), InvalidOrWarningCount == 0 && (!bValidateProjectSettings || false));
	Result->SetNumberField(TEXT("invalid_or_warning_count"), InvalidOrWarningCount);
	Result->SetNumberField(TEXT("asset_count"), AssetsToValidate.Num());
	Result->SetArrayField(TEXT("assets"), AssetRows);
	Result->SetNumberField(TEXT("num_requested"), ValidationResults.NumRequested);
	Result->SetNumberField(TEXT("num_external_objects"), ValidationResults.NumExternalObjects);
	Result->SetNumberField(TEXT("num_checked"), ValidationResults.NumChecked);
	Result->SetNumberField(TEXT("num_valid"), ValidationResults.NumValid);
	Result->SetNumberField(TEXT("num_invalid"), ValidationResults.NumInvalid);
	Result->SetNumberField(TEXT("num_skipped"), ValidationResults.NumSkipped);
	Result->SetNumberField(TEXT("num_warnings"), ValidationResults.NumWarnings);
	Result->SetNumberField(TEXT("num_unable_to_validate"), ValidationResults.NumUnableToValidate);
	Result->SetBoolField(TEXT("asset_limit_reached"), ValidationResults.bAssetLimitReached);
	Result->SetArrayField(TEXT("details"), DetailRows);
	Result->SetArrayField(TEXT("validator_messages"), ValidatorMessages);
	Result->SetObjectField(TEXT("project_settings"), ProjectSettings);
	return FMonolithActionResult::Success(Result);
}
