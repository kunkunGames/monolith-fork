#include "MonolithReindexCommandlet.h"

#include "MonolithActivationState.h"
#include "MonolithIndexFreshness.h"
#include "MonolithSourceIndexer.h"
#include "MonolithSourceDatabase.h" // DECLARE_LOG_CATEGORY_EXTERN(LogMonolithSource)
#include "MonolithSettings.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	// Mirrors UMonolithSourceSubsystem::GetDatabasePath() so the offline and
	// in-editor paths target the exact same DB (settings override → plugin
	// Saved dir → project plugins fallback).
	FString ResolveDbPath()
	{
		return FMonolithIndexFreshnessUtils::ResolveSourceIndexDbPath(UMonolithSettings::Get());
	}

	FString ResolveEngineSourcePath()
	{
		if (const UMonolithSettings* Settings = UMonolithSettings::Get())
		{
			if (!Settings->EngineSourcePath.Path.IsEmpty())
			{
				return Settings->EngineSourcePath.Path;
			}
		}
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source"));
	}
}

UMonolithReindexCommandlet::UMonolithReindexCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;        // needs editor-phase modules (MonolithSource is Type: Editor)
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UMonolithReindexCommandlet::Main(const FString& Params)
{
	// ---- parse args -------------------------------------------------------
	FString Mode = TEXT("project");
	FParse::Value(*Params, TEXT("mode="), Mode);
	Mode = Mode.ToLower();
	const bool bFull = (Mode == TEXT("full"));

	FString DbPath        = ResolveDbPath();
	FString EngineSrcPath = ResolveEngineSourcePath();
	FString ShaderPath    = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Shaders"));
	FString ProjectPath   = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	FString Override;
	if (FParse::Value(*Params, TEXT("db="), Override) && !Override.IsEmpty())            { DbPath = Override; }
	if (FParse::Value(*Params, TEXT("enginesource="), Override) && !Override.IsEmpty()) { EngineSrcPath = Override; }
	if (FParse::Value(*Params, TEXT("projectpath="), Override) && !Override.IsEmpty())  { ProjectPath = Override; }
	const bool bForceClean = FParse::Param(*Params, TEXT("clean"));
	const bool bAllowWhenIndexingDisabled =
		FParse::Param(*Params, TEXT("AllowWhenIndexingDisabled"));

	if (!FMonolithActivationState::IsIndexingEnabled() && !bAllowWhenIndexingDisabled)
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: indexing is disabled. Run Monolith.StartIndexing in the editor console, or pass -AllowWhenIndexingDisabled for an explicit one-shot maintenance override."));
		return 1;
	}
	if (bAllowWhenIndexingDisabled)
	{
		UE_LOG(LogMonolithSource, Display,
			TEXT("MonolithReindex: explicit -AllowWhenIndexingDisabled override accepted; durable activation is unchanged"));
	}

	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

	UE_LOG(LogMonolithSource, Display,
		TEXT("MonolithReindex: mode=%s db=%s"), bFull ? TEXT("full") : TEXT("project"), *DbPath);

	// Project (incremental) mode requires an existing DB with engine symbols.
	if (!bFull && !PF.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: EngineSource.db not found at %s — run with -mode=full first."), *DbPath);
		return 1;
	}

	// Ensure target directory exists (full mode may create the DB fresh).
	const FString SavedDir = FPaths::GetPath(DbPath);
	if (!SavedDir.IsEmpty() && !PF.DirectoryExists(*SavedDir))
	{
		PF.CreateDirectoryTree(*SavedDir);
	}

	// ---- configure indexer (mirrors subsystem Trigger*Reindex) -----------
	FMonolithSourceIndexer Indexer;
	Indexer.SetDatabasePath(DbPath);
	Indexer.SetProjectPath(ProjectPath);
	Indexer.SetIndexProjectSource(true);
	if (bFull)
	{
		Indexer.SetSourcePath(EngineSrcPath);
		Indexer.SetShaderPath(ShaderPath);
		Indexer.SetCleanBuild(true);
	}
	else
	{
		// project-only: no engine source path, keep existing engine symbols
		Indexer.SetCleanBuild(bForceClean);
	}

	int32 ResultFiles = 0, ResultSymbols = 0, ResultErrors = 0;
	bool bCompleted = false;
	bool bCompletionSucceeded = false;
	Indexer.OnComplete.AddLambda([&](int32 Files, int32 Symbols, int32 Errors, bool bSucceeded)
	{
		ResultFiles = Files;
		ResultSymbols = Symbols;
		ResultErrors = Errors;
		bCompletionSucceeded = bSucceeded;
		bCompleted = true;
	});
	Indexer.OnProgress.AddLambda([](const FString& ModuleName, int32 Cur, int32 Total, int32 Files, int32 Syms)
	{
		UE_LOG(LogMonolithSource, Display,
			TEXT("MonolithReindex: [%d/%d] %s (files=%d symbols=%d)"), Cur, Total, *ModuleName, Files, Syms);
	});

	const double StartedAt = FPlatformTime::Seconds();
	const bool bRunSucceeded = Indexer.RunSynchronous(); // blocks until done
	const double Elapsed = FPlatformTime::Seconds() - StartedAt;

	if (!bRunSucceeded)
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: indexer did not complete successfully (files=%d symbols=%d errors=%d)."),
			ResultFiles, ResultSymbols, ResultErrors);
		return 1;
	}
	if (!bCompleted)
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: indexer returned without firing OnComplete; failing closed."));
		return 1;
	}
	if (!bCompletionSucceeded)
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: completion callback reported failure; failing closed."));
		return 1;
	}

	if (!PF.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("MonolithReindex: completed but %s does not exist — failure."), *DbPath);
		return 1;
	}

	UE_LOG(LogMonolithSource, Display,
		TEXT("MonolithReindex: done in %.1fs — files=%d symbols=%d errors=%d (db=%s)"),
		Elapsed, ResultFiles, ResultSymbols, ResultErrors, *DbPath);

	// Per-file parse errors are expected across a large engine tree and are
	// non-fatal (matches the in-editor subsystem). DB-produced == success.
	return 0;
}
