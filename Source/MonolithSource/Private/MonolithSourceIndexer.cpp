#include "MonolithSourceIndexer.h"
#include "MonolithSourceDatabase.h"
#include "MonolithSourceSchema.h"
#include "MonolithSQLiteMaintenance.h"
#include "MonolithCppParser.h"
#include "MonolithShaderParser.h"
#include "MonolithReferenceBuilder.h"
#include "HAL/RunnableThread.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Internationalization/Regex.h"

namespace MonolithSourceIndexerDetail
{
	// Decision 2: derive a module's <Module>.Build.cs path from its Source/<Module>/
	// dir. Tries <ModulePath>/<ModuleName>.Build.cs first (the common convention);
	// falls back to the first *.Build.cs found in the dir (module name != dir name).
	// Returns empty when no Build.cs exists (e.g. the Shaders pseudo-module).
	static FString DeriveBuildCsPath(const FString& ModulePath, const FString& ModuleName)
	{
		const FString Candidate = ModulePath / (ModuleName + TEXT(".Build.cs"));
		if (IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}

		TArray<FString> Found;
		IFileManager::Get().FindFiles(Found, *(ModulePath / TEXT("*.Build.cs")), /*Files=*/true, /*Directories=*/false);
		if (Found.Num() > 0)
		{
			return ModulePath / Found[0];
		}
		return FString();
	}

	static FString NormalizePathForStorage(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return FString();
		}

		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	static FString CanonicalizeExistingRoot(const FString& InPath)
	{
		FString Path = NormalizePathForStorage(InPath);
		if (Path.IsEmpty())
		{
			return Path;
		}

		// Preserve junction identity while stabilizing drive-letter and on-disk
		// component casing. FPaths::FindCorrectCase uses IterateComponents, whose
		// UE 5.8 contract does not handle Windows network paths; applying it to a
		// normalized //server/share root would lose the UNC prefix. Preserve UNC
		// casing as supplied and correct only local filesystem roots.
		if (!Path.StartsWith(TEXT("//"), ESearchCase::CaseSensitive))
		{
			Path = FPaths::FindCorrectCase(Path);
		}
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	static FString NormalizePathForIdentity(const FString& InPath)
	{
		FString Path = NormalizePathForStorage(InPath);
#if PLATFORM_WINDOWS
		Path.ToLowerInline();
#endif
		return Path;
	}

	static bool IsStrictlyUnderPath(const FString& ChildKey, const FString& ParentKey)
	{
		if (ChildKey.Len() <= ParentKey.Len())
		{
			return false;
		}
		FString Prefix = ParentKey;
		if (!Prefix.EndsWith(TEXT("/")))
		{
			Prefix += TEXT("/");
		}
		return ChildKey.StartsWith(Prefix, ESearchCase::CaseSensitive);
	}
}

// ============================================================
// Construction / Destruction
// ============================================================

FMonolithSourceIndexer::FMonolithSourceIndexer()
{
}

FMonolithSourceIndexer::~FMonolithSourceIndexer()
{
	RequestStop();
	if (Thread)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

// ============================================================
// Configuration setters
// ============================================================

void FMonolithSourceIndexer::SetSourcePath(const FString& InPath)   { SourcePath = InPath; }
void FMonolithSourceIndexer::SetShaderPath(const FString& InPath)   { ShaderPath = InPath; }
void FMonolithSourceIndexer::SetProjectPath(const FString& InPath)  { ProjectPath = InPath; }
void FMonolithSourceIndexer::SetDatabasePath(const FString& InPath) { DbPath = InPath; }
void FMonolithSourceIndexer::SetCleanBuild(bool bClean)             { bCleanBuild = bClean; }
void FMonolithSourceIndexer::SetIndexProjectSource(bool bIndex)     { bIndexProjectSource = bIndex; }

// ============================================================
// Thread control
// ============================================================

bool FMonolithSourceIndexer::StartAsync()
{
	if (bIsRunning) return false;
	Thread = FRunnableThread::Create(this, TEXT("MonolithSourceIndexer"), 0, TPri_BelowNormal);
	return Thread != nullptr;
}

bool FMonolithSourceIndexer::RunSynchronous()
{
	if (bIsRunning)
	{
		return false;
	}
	Init();
	return Run() == 0;
}

void FMonolithSourceIndexer::RequestStop()
{
	bShouldStop = true;
}

bool FMonolithSourceIndexer::IsRunning() const
{
	return bIsRunning;
}

FSourceIndexDiagnostics FMonolithSourceIndexer::GetDiagnostics() const
{
	FScopeLock Lock(&DiagLock);
	return Diagnostics;
}

// ============================================================
// FRunnable interface
// ============================================================

bool FMonolithSourceIndexer::Init()
{
	return true;
}

void FMonolithSourceIndexer::Stop()
{
	bShouldStop = true;
}

uint32 FMonolithSourceIndexer::Run()
{
	bIsRunning = true;
	bool bRunSucceeded = false;
	ON_SCOPE_EXIT
	{
		bIsRunning = false;

		const int32 Files = TotalFilesProcessed.Load();
		const int32 Symbols = TotalSymbolsExtracted.Load();
		const int32 Errors = TotalErrors.Load();

		UE_LOG(LogMonolithSource, Log, TEXT("Indexer complete: %d files, %d symbols, %d errors"), Files, Symbols, Errors);
		if (DuplicateFileVisitsSkipped > 0)
		{
			UE_LOG(LogMonolithSource, Log,
				TEXT("Indexer skipped %d duplicate file visits across overlapping module roots"),
				DuplicateFileVisitsSkipped);
		}
		OnComplete.Broadcast(Files, Symbols, Errors, bRunSucceeded);
	};

	const auto FailRun = [this]() -> uint32
	{
		TotalErrors++;
		return 1;
	};

	FMonolithSourceDatabase DB;
	ON_SCOPE_EXIT
	{
		DB.Close();
	};

	// Normalize configured roots before pruning, discovery, traversal, and DB
	// insertion. This keeps incremental runs stable when Windows callers vary
	// drive-letter or directory casing and does not resolve directory junctions.
	SourcePath = MonolithSourceIndexerDetail::CanonicalizeExistingRoot(SourcePath);
	ShaderPath = MonolithSourceIndexerDetail::CanonicalizeExistingRoot(ShaderPath);
	ProjectPath = MonolithSourceIndexerDetail::CanonicalizeExistingRoot(ProjectPath);

	const bool bOpenedForWriting = DB.OpenForWriting(DbPath);
	if (!bOpenedForWriting && !bCleanBuild)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: Failed to open DB for writing: %s"), *DbPath);
		return FailRun();
	}

	if (bCleanBuild)
	{
		if (!DB.ResetDatabase())
		{
			UE_LOG(LogMonolithSource, Error, TEXT("Indexer: Failed to reset/recreate DB for clean source reindex: %s"), *DbPath);
			return FailRun();
		}
	}
	else
	{
		if (!DB.CreateTablesIfNeeded())
		{
			UE_LOG(LogMonolithSource, Error, TEXT("Indexer: Failed to create/verify DB schema before source reindex: %s"), *DbPath);
			return FailRun();
		}
	}

	// --- Engine phase ---
	if (!SourcePath.IsEmpty())
	{
		TArray<FModuleEntry> EngineModules;
		DiscoverEngineModules(EngineModules);

		UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Found %d engine modules"), EngineModules.Num());

		for (int32 i = 0; i < EngineModules.Num() && !bShouldStop; ++i)
		{
			if (!IndexModule(EngineModules[i], DB))
			{
				return FailRun();
			}
			OnProgress.Broadcast(EngineModules[i].Name, i + 1, EngineModules.Num(),
				TotalFilesProcessed.Load(), TotalSymbolsExtracted.Load());
		}
	}

	// --- Project phase ---
	if (bIndexProjectSource && !ProjectPath.IsEmpty() && !bShouldStop)
	{
		// If NOT clean build, load existing engine symbols for cross-reference resolution
		if (!bCleanBuild)
		{
			TArray<FString> PruneRoots;
			PruneRoots.Add(ProjectPath / TEXT("Source"));
			PruneRoots.Add(ProjectPath / TEXT("Plugins"));
			const int32 PrunedFiles = DB.PruneIndexedFilesUnderRoots(PruneRoots);
			if (PrunedFiles < 0)
			{
				UE_LOG(LogMonolithSource, Error, TEXT("Indexer: Failed to prune project source rows before scoped source reindex"));
				return FailRun();
			}
			UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Loading existing symbols for incremental indexing..."));
			DB.LoadExistingSymbols(SymbolNameToId, ClassNameToId, SymbolSpans, ClassSpans);
		}

		TArray<FModuleEntry> ProjectModules;
		DiscoverProjectModules(ProjectModules);

		UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Found %d project modules"), ProjectModules.Num());

		for (int32 i = 0; i < ProjectModules.Num() && !bShouldStop; ++i)
		{
			if (!IndexModule(ProjectModules[i], DB))
			{
				return FailRun();
			}
			OnProgress.Broadcast(ProjectModules[i].Name, i + 1, ProjectModules.Num(),
				TotalFilesProcessed.Load(), TotalSymbolsExtracted.Load());
		}
	}

	// --- Finalize ---
	if (!bShouldStop)
	{
		if (!Finalize(DB))
		{
			return FailRun();
		}
		if (!bCleanBuild && bIndexProjectSource)
		{
			TSharedPtr<FJsonObject> ScopedCrg = DB.RefreshCrgCacheForFiles(NewFileIds, TEXT("Project source indexing complete"));
			FString Status;
			FString Summary;
			FString RefreshMode;
			if (ScopedCrg.IsValid())
			{
				ScopedCrg->TryGetStringField(TEXT("status"), Status);
				ScopedCrg->TryGetStringField(TEXT("summary"), Summary);
				ScopedCrg->TryGetStringField(TEXT("refresh_mode"), RefreshMode);
			}
			if (Status != TEXT("ok"))
			{
				UE_LOG(LogMonolithSource, Error,
					TEXT("Indexer: scoped source CRG refresh failed; project source indexing cannot complete: %s"),
					*Summary);
				return FailRun();
			}

			if (RefreshMode == TEXT("full_required"))
			{
				TSharedPtr<FJsonObject> FullCrg = DB.RepairCrgCache(true);
				FString FullStatus;
				FString FullSummary;
				if (FullCrg.IsValid())
				{
					FullCrg->TryGetStringField(TEXT("status"), FullStatus);
					FullCrg->TryGetStringField(TEXT("summary"), FullSummary);
				}
				if (FullStatus != TEXT("ok"))
				{
					UE_LOG(LogMonolithSource, Error,
						TEXT("Indexer: source CRG projection bootstrap failed; project source indexing cannot complete: %s"),
						*FullSummary);
					return FailRun();
				}
				UE_LOG(LogMonolithSource, Log,
					TEXT("Indexer: source CRG projection tables were absent; completed one required full bootstrap: %s"),
					*FullSummary);
			}
			else
			{
				UE_LOG(LogMonolithSource, Log, TEXT("Indexer: %s"), *Summary);
			}
		}
	}
	else
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexer: indexing cancelled before finalization"));
		return 1;
	}
	if (bShouldStop)
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Indexer: indexing cancelled during finalization"));
		return 1;
	}

	bRunSucceeded = true;
	return 0;
}

// ============================================================
// Module discovery
// ============================================================

void FMonolithSourceIndexer::DiscoverEngineModules(TArray<FModuleEntry>& OutModules)
{
	// Runtime, Editor, Developer, Programs categories
	const TArray<FString> Categories = { TEXT("Runtime"), TEXT("Editor"), TEXT("Developer"), TEXT("Programs") };

	for (const FString& Category : Categories)
	{
		FString CategoryDir = SourcePath / Category;
		IFileManager::Get().IterateDirectory(*CategoryDir, [&](const TCHAR* Path, bool bIsDir) -> bool
		{
			if (bIsDir)
			{
				FString DirName = FPaths::GetCleanFilename(Path);
				OutModules.Add({ FString(Path), DirName, Category,
					MonolithSourceIndexerDetail::DeriveBuildCsPath(FString(Path), DirName) });
			}
			return true; // continue iteration
		});
	}

	// Engine plugins — preserve every top-level Source corpus while suppressing
	// non-plugin Source directories nested below another Source root.
	// SourcePath is Engine/Source, parent is Engine/
	FString PluginsDir = FPaths::GetPath(SourcePath) / TEXT("Plugins");
	DiscoverPluginSourceRoots(PluginsDir, /*bProjectPlugins=*/false, OutModules);

	// Shaders — no Build.cs
	if (!ShaderPath.IsEmpty())
	{
		OutModules.Add({ ShaderPath, TEXT("Shaders"), TEXT("Shaders"), FString() });
	}
}

void FMonolithSourceIndexer::DiscoverProjectModules(TArray<FModuleEntry>& OutModules)
{
	// Top-level project modules: ProjectPath/Source/*/
	FString ProjectSourceDir = ProjectPath / TEXT("Source");
	IFileManager::Get().IterateDirectory(*ProjectSourceDir, [&](const TCHAR* Path, bool bIsDir) -> bool
	{
		if (bIsDir)
		{
			FString DirName = FPaths::GetCleanFilename(Path);
			OutModules.Add({ FString(Path), DirName, TEXT("Project"),
				MonolithSourceIndexerDetail::DeriveBuildCsPath(FString(Path), DirName) });
		}
		return true;
	});

	// Preserve standalone project-plugin source corpora while suppressing false
	// Source roots nested inside an enclosing source tree.
	FString ProjectPluginsDir = ProjectPath / TEXT("Plugins");
	DiscoverPluginSourceRoots(ProjectPluginsDir, /*bProjectPlugins=*/true, OutModules);
}

void FMonolithSourceIndexer::DiscoverPluginSourceRoots(
	const FString& PluginsDir,
	bool bProjectPlugins,
	TArray<FModuleEntry>& OutModules)
{
	struct FSourceRootCandidate
	{
		FModuleEntry Module;
		FString Key;
		bool bDescriptorOwned = false;
		int32 NestingDepth = 0;
	};

	TArray<FSourceRootCandidate> Candidates;
	TSet<FString> SeenSourceRoots;
	IFileManager::Get().IterateDirectoryRecursively(*PluginsDir, [&](const TCHAR* Path, bool bIsDir) -> bool
	{
		if (!bIsDir || FPaths::GetCleanFilename(Path) != TEXT("Source"))
		{
			return true;
		}

		const FString SourceDir = MonolithSourceIndexerDetail::NormalizePathForStorage(Path);
		const FString SourceKey = MonolithSourceIndexerDetail::NormalizePathForIdentity(SourceDir);
		if (SeenSourceRoots.Contains(SourceKey))
		{
			return true;
		}
		SeenSourceRoots.Add(SourceKey);

		const FString ParentDir = FPaths::GetPath(SourceDir);
		const FString ModuleName = FPaths::GetCleanFilename(ParentDir);
		const FString ModuleType = bProjectPlugins && SourceDir.Contains(TEXT("/GameFeatures/"), ESearchCase::IgnoreCase)
			? TEXT("GameFeature")
			: TEXT("Plugin");
		TArray<FString> Descriptors;
		IFileManager::Get().FindFiles(
			Descriptors, *(ParentDir / TEXT("*.uplugin")),
			/*Files=*/true, /*Directories=*/false);
		Candidates.Add({
			{ SourceDir, ModuleName, ModuleType,
				MonolithSourceIndexerDetail::DeriveBuildCsPath(SourceDir, ModuleName) },
			SourceKey,
			Descriptors.Num() > 0,
			0 });
		return true;
	});

	TArray<FSourceRootCandidate> KeptCandidates;
	int32 NestedNonPluginRootsSkipped = 0;
	for (FSourceRootCandidate& Candidate : Candidates)
	{
		bool bNestedBelowSourceRoot = false;
		for (const FSourceRootCandidate& PossibleParent : Candidates)
		{
			if (MonolithSourceIndexerDetail::IsStrictlyUnderPath(Candidate.Key, PossibleParent.Key))
			{
				bNestedBelowSourceRoot = true;
				++Candidate.NestingDepth;
			}
		}

		// A nested Source without its own descriptor is data, a fixture, docs, or
		// vendor output already covered by the enclosing source root. A real nested
		// plugin descriptor remains a valid, more-specific owner.
		if (bNestedBelowSourceRoot && !Candidate.bDescriptorOwned)
		{
			++NestedNonPluginRootsSkipped;
			continue;
		}
		// Keep every source key intact until all candidates have completed their
		// ancestry checks. Moving here would clear an earlier parent's key and make
		// later nested candidates appear standalone.
		KeptCandidates.Add(Candidate);
	}

	// Most-specific descriptor-backed roots claim their files before an enclosing
	// root. Unrelated roots use normalized lexical ordering for reproducibility.
	KeptCandidates.Sort([](const FSourceRootCandidate& A, const FSourceRootCandidate& B)
	{
		if (A.NestingDepth != B.NestingDepth)
		{
			return A.NestingDepth > B.NestingDepth;
		}
		if (A.Key != B.Key)
		{
			return A.Key < B.Key;
		}
		return A.Module.Name < B.Module.Name;
	});

	for (FSourceRootCandidate& Candidate : KeptCandidates)
	{
		OutModules.Add(MoveTemp(Candidate.Module));
	}
	if (NestedNonPluginRootsSkipped > 0)
	{
		UE_LOG(LogMonolithSource, Log,
			TEXT("Indexer ignored %d non-plugin Source roots nested below another Source root"),
			NestedNonPluginRootsSkipped);
	}
}

// ============================================================
// Module indexing
// ============================================================

bool FMonolithSourceIndexer::IndexModule(const FModuleEntry& Module, FMonolithSourceDatabase& DB)
{
	const FString ModulePath = MonolithSourceIndexerDetail::NormalizePathForStorage(Module.Path);
	const FString BuildCsPath = MonolithSourceIndexerDetail::NormalizePathForStorage(Module.BuildCsPath);
	const int64 ModuleId = DB.InsertModule(Module.Name, ModulePath, Module.Type, BuildCsPath);
	if (ModuleId <= 0)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to create or resolve module row for '%s'"), *Module.Name);
		return false;
	}

	// Collect all source files for this module
	TArray<FString> Files;

	if (Module.Type == TEXT("Shaders"))
	{
		// Shader module — only shader files
		IFileManager::Get().FindFilesRecursive(Files, *ModulePath, TEXT("*.usf"), true, false, true);
		IFileManager::Get().FindFilesRecursive(Files, *ModulePath, TEXT("*.ush"), true, false, false); // bClearFileNames=false!
	}
	else
	{
		// C++ module — headers, source, inline files
		IFileManager::Get().FindFilesRecursive(Files, *ModulePath, TEXT("*.h"), true, false, true);
		IFileManager::Get().FindFilesRecursive(Files, *ModulePath, TEXT("*.cpp"), true, false, false); // bClearFileNames=false!
		IFileManager::Get().FindFilesRecursive(Files, *ModulePath, TEXT("*.inl"), true, false, false); // bClearFileNames=false!
	}

	if (!DB.BeginTransaction())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to begin transaction for module '%s'"), *Module.Name);
		return false;
	}

	for (const FString& FilePath : Files)
	{
		if (bShouldStop) break;

		const FString StorageFilePath = MonolithSourceIndexerDetail::NormalizePathForStorage(FilePath);
		const FString FilePathKey = MonolithSourceIndexerDetail::NormalizePathForIdentity(StorageFilePath);
		if (IndexedFilePathKeys.Contains(FilePathKey))
		{
			++DuplicateFileVisitsSkipped;
			continue;
		}
		IndexedFilePathKeys.Add(FilePathKey);

		FString Ext = FPaths::GetExtension(StorageFilePath).ToLower();
		int32 SymbolCount = 0;

		if (Ext == TEXT("usf") || Ext == TEXT("ush"))
		{
			SymbolCount = IndexShaderFile(StorageFilePath, ModuleId, DB);
		}
		else
		{
			SymbolCount = IndexCppFile(StorageFilePath, ModuleId, DB);
		}

		if (SymbolCount < 0)
		{
			DB.RollbackTransaction();
			UE_LOG(LogMonolithSource, Error, TEXT("Indexer: database write failed while indexing module '%s'"), *Module.Name);
			return false;
		}

		TotalFilesProcessed++;
		TotalSymbolsExtracted += SymbolCount;
	}

	if (bShouldStop)
	{
		DB.RollbackTransaction();
		return false;
	}

	if (!DB.CommitTransaction())
	{
		DB.RollbackTransaction();
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to commit transaction for module '%s'"), *Module.Name);
		return false;
	}

	return true;
}

// ============================================================
// File indexing
// ============================================================

int32 FMonolithSourceIndexer::IndexCppFile(const FString& FilePath, int64 ModuleId, FMonolithSourceDatabase& DB)
{
	FMonolithCppParser Parser;
	FParsedFileResult ParseResult = Parser.ParseFile(FilePath);

	FString Ext = FPaths::GetExtension(FilePath).ToLower();
	FString FileType;
	if (Ext == TEXT("h"))        FileType = TEXT("header");
	else if (Ext == TEXT("cpp")) FileType = TEXT("source");
	else                         FileType = TEXT("inline"); // .inl

	// Get file modification time
	FDateTime ModTime = IFileManager::Get().GetTimeStamp(*FilePath);
	double LastModified = static_cast<double>(ModTime.ToUnixTimestamp());

	const int64 FileId = DB.InsertFile(FilePath, ModuleId, FileType, ParseResult.SourceLines.Num(), LastModified);
	if (FileId <= 0)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to create or resolve file row for '%s'"), *FilePath);
		return -1;
	}
	NewFileIds.Add(FileId);

	// Includes
	for (const FString& IncPath : ParseResult.Includes)
	{
		int32 IncLine = 0;
		for (int32 i = 0; i < ParseResult.SourceLines.Num(); ++i)
		{
			if (ParseResult.SourceLines[i].Contains(IncPath) && ParseResult.SourceLines[i].Contains(TEXT("#include")))
			{
				IncLine = i + 1; // 1-based
				break;
			}
		}
		DB.InsertInclude(FileId, IncPath, IncLine);
	}

	// Symbols
	int32 SymbolCount = 0;
	for (const FParsedSourceSymbol& Sym : ParseResult.Symbols)
	{
		if (Sym.Kind == TEXT("include")) continue;

		FString QualifiedName = Sym.Name;
		if (!Sym.ParentClass.IsEmpty())
		{
			QualifiedName = Sym.ParentClass + TEXT("::") + Sym.Name;
		}

		// Look up parent symbol id
		int64 ParentSymbolId = 0;
		if (!Sym.ParentClass.IsEmpty())
		{
			const int64* ParentId = SymbolNameToId.Find(Sym.ParentClass);
			if (ParentId) ParentSymbolId = *ParentId;
		}

		int64 SymId = DB.InsertSymbol(
			Sym.Name, QualifiedName, Sym.Kind,
			FileId, Sym.LineStart, Sym.LineEnd,
			ParentSymbolId, Sym.Access, Sym.Signature, Sym.Docstring,
			Sym.bIsUEMacro
		);
		if (SymId <= 0)
		{
			UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to insert symbol '%s' from '%s'"), *QualifiedName, *FilePath);
			return -1;
		}

		// Update symbol maps
		UpdateSymbolMap(Sym.Name, SymId, Sym.LineStart, Sym.LineEnd);
		if (QualifiedName != Sym.Name)
		{
			UpdateSymbolMap(QualifiedName, SymId, Sym.LineStart, Sym.LineEnd);
		}

		// Class/struct tracking
		if (Sym.Kind == TEXT("class") || Sym.Kind == TEXT("struct"))
		{
			UpdateClassMap(Sym.Name, SymId, Sym.LineStart, Sym.LineEnd);

			if (Sym.BaseClasses.Num() > 0)
			{
				PendingBaseClasses.Add(Sym.Name, Sym.BaseClasses);

				FScopeLock Lock(&DiagLock);
				Diagnostics.WithBaseClasses++;
			}

			{
				FScopeLock Lock(&DiagLock);
				if (Sym.LineEnd > Sym.LineStart) Diagnostics.Definitions++;
				else Diagnostics.ForwardDecls++;
			}
		}

		SymbolCount++;
	}

	// --- Deprecation extraction (item 3) ---
	// LOCAL FRegexPattern/FRegexMatcher only — never static/global (ICU init
	// gotcha; the matcher relies on the internationalization system, and a
	// file-scope instance would construct before ICU is ready). The indexer runs
	// after engine init, so constructing locals here is safe.
	//
	// Matches UE_DEPRECATED / UE_DEPRECATED_FORENGINE / UE_DEPRECATED_FORGAME with
	//   (Version, "Message")  — Version may be absent (e.g. some FORGAME forms).
	// Capture groups: 1 = kind suffix (_FORENGINE|_FORGAME|empty), 2 = version,
	//                 3 = message.
	{
		const FRegexPattern DeprPattern(
			TEXT("UE_DEPRECATED(_FORENGINE|_FORGAME)?\\s*\\(\\s*([\\d.]+)?\\s*,?\\s*\"([^\"]*)\""));

		const TArray<FString>& Lines = ParseResult.SourceLines;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			FRegexMatcher Matcher(DeprPattern, Lines[i]);
			if (!Matcher.FindNext())
			{
				continue;
			}

			const FString Suffix  = Matcher.GetCaptureGroup(1);
			const FString Version = Matcher.GetCaptureGroup(2);
			const FString Message = Matcher.GetCaptureGroup(3);

			FString Kind = TEXT("UE_DEPRECATED");
			if (Suffix == TEXT("_FORENGINE")) Kind = TEXT("UE_DEPRECATED_FORENGINE");
			else if (Suffix == TEXT("_FORGAME")) Kind = TEXT("UE_DEPRECATED_FORGAME");

			// Parse the symbol NAME from the declaration text following the macro.
			// Class-body methods have no symbols row (Step-0 finding), so we scan
			// forward over intervening specifier/macro lines (UPROPERTY/UFUNCTION/
			// UE_API/comments/blanks) to the first real declaration, then take the
			// identifier immediately before '(' (function) or before ';'/'=' (var).
			FString SymbolName;
			for (int32 j = i; j < Lines.Num() && j < i + 8; ++j)
			{
				FString Decl = Lines[j].TrimStartAndEnd();

				// On the macro line itself, strip the macro invocation up to the
				// LAST ')' (the macro's closing paren; using the last paren tolerates
				// a message string that itself contains "()", e.g. "Use Foo() instead").
				if (j == i)
				{
					int32 CloseIdx = INDEX_NONE;
					if (Decl.FindLastChar(TEXT(')'), CloseIdx))
					{
						Decl = Decl.Mid(CloseIdx + 1).TrimStartAndEnd();
					}
					else
					{
						continue; // macro args spill to next line — skip this one
					}
				}

				if (Decl.IsEmpty()) continue;
				if (Decl.StartsWith(TEXT("//")) || Decl.StartsWith(TEXT("/*")) || Decl.StartsWith(TEXT("*"))) continue;

				// Skip pure specifier/macro lines (UPROPERTY(), UFUNCTION(...), etc.)
				// and lone UE_API-style export macros — keep scanning for the decl.
				if (Decl.StartsWith(TEXT("UPROPERTY")) || Decl.StartsWith(TEXT("UFUNCTION"))
					|| Decl.StartsWith(TEXT("UDELEGATE")) || Decl.StartsWith(TEXT("UE_DEPRECATED")))
				{
					continue;
				}

				// Function form: identifier immediately before the first '('.
				int32 ParenIdx = INDEX_NONE;
				int32 SemiIdx = INDEX_NONE;
				int32 EqIdx = INDEX_NONE;
				Decl.FindChar(TEXT('('), ParenIdx);
				Decl.FindChar(TEXT(';'), SemiIdx);
				Decl.FindChar(TEXT('='), EqIdx);

				int32 EndIdx = INDEX_NONE;
				if (ParenIdx != INDEX_NONE)
				{
					EndIdx = ParenIdx; // function
				}
				else if (SemiIdx != INDEX_NONE || EqIdx != INDEX_NONE)
				{
					EndIdx = (SemiIdx != INDEX_NONE && (EqIdx == INDEX_NONE || SemiIdx < EqIdx)) ? SemiIdx : EqIdx; // variable / property
				}

				if (EndIdx == INDEX_NONE)
				{
					continue; // declaration continues on the next line
				}

				// Walk backwards from EndIdx over the trailing identifier.
				int32 NameEnd = EndIdx;
				while (NameEnd > 0 && FChar::IsWhitespace(Decl[NameEnd - 1])) --NameEnd;
				int32 NameStart = NameEnd;
				while (NameStart > 0)
				{
					const TCHAR Ch = Decl[NameStart - 1];
					if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
					{
						--NameStart;
					}
					else
					{
						break;
					}
				}
				if (NameEnd > NameStart)
				{
					SymbolName = Decl.Mid(NameStart, NameEnd - NameStart);
				}
				break;
			}

			if (SymbolName.IsEmpty())
			{
				continue; // could not resolve a name — skip rather than store garbage
			}

			// Link to a symbols row when one exists; NULL (0) otherwise.
			int64 LinkedSymId = 0;
			if (const int64* Found = SymbolNameToId.Find(SymbolName))
			{
				LinkedSymId = *Found;
			}

			DB.InsertDeprecation(LinkedSymId, SymbolName, Version, Message, Kind);
		}
	}

	// Source FTS chunks
	DB.InsertSourceChunks(FileId, ParseResult.SourceLines);

	return SymbolCount;
}

int32 FMonolithSourceIndexer::IndexShaderFile(const FString& FilePath, int64 ModuleId, FMonolithSourceDatabase& DB)
{
	FMonolithShaderParser Parser;
	FParsedFileResult ParseResult = Parser.ParseFile(FilePath);

	FString Ext = FPaths::GetExtension(FilePath).ToLower();
	FString FileType = (Ext == TEXT("ush")) ? TEXT("shader_header") : TEXT("shader");

	FDateTime ModTime = IFileManager::Get().GetTimeStamp(*FilePath);
	double LastModified = static_cast<double>(ModTime.ToUnixTimestamp());

	const int64 FileId = DB.InsertFile(FilePath, ModuleId, FileType, ParseResult.SourceLines.Num(), LastModified);
	if (FileId <= 0)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to create or resolve shader file row for '%s'"), *FilePath);
		return -1;
	}
	NewFileIds.Add(FileId);

	// Includes
	for (const FString& IncPath : ParseResult.Includes)
	{
		int32 IncLine = 0;
		for (int32 i = 0; i < ParseResult.SourceLines.Num(); ++i)
		{
			if (ParseResult.SourceLines[i].Contains(IncPath) && ParseResult.SourceLines[i].Contains(TEXT("#include")))
			{
				IncLine = i + 1;
				break;
			}
		}
		DB.InsertInclude(FileId, IncPath, IncLine);
	}

	// Symbols — shader symbols do NOT go into SymbolNameToId (prevents false cross-references)
	int32 SymbolCount = 0;
	for (const FParsedSourceSymbol& Sym : ParseResult.Symbols)
	{
		if (Sym.Kind == TEXT("include")) continue;

		FString QualifiedName = Sym.Name;
		if (!Sym.ParentClass.IsEmpty())
		{
			QualifiedName = Sym.ParentClass + TEXT("::") + Sym.Name;
		}

		const int64 SymId = DB.InsertSymbol(
			Sym.Name, QualifiedName, Sym.Kind,
			FileId, Sym.LineStart, Sym.LineEnd,
			0, Sym.Access, Sym.Signature, Sym.Docstring,
			Sym.bIsUEMacro
		);
		if (SymId <= 0)
		{
			UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to insert shader symbol '%s' from '%s'"), *QualifiedName, *FilePath);
			return -1;
		}

		SymbolCount++;
	}

	// Source FTS chunks
	DB.InsertSourceChunks(FileId, ParseResult.SourceLines);

	return SymbolCount;
}

// ============================================================
// Symbol map helpers
// ============================================================

void FMonolithSourceIndexer::UpdateSymbolMap(const FString& Name, int64 SymId, int32 LineStart, int32 LineEnd)
{
	if (Name.StartsWith(TEXT("_bases_"))) return;

	bool bIsDefinition = (LineEnd > LineStart);
	TPair<int32, int32>* ExistingSpan = SymbolSpans.Find(Name);

	if (!ExistingSpan)
	{
		SymbolNameToId.Add(Name, SymId);
		SymbolSpans.Add(Name, TPair<int32, int32>(LineStart, LineEnd));
	}
	else if (bIsDefinition && ExistingSpan->Value <= ExistingSpan->Key)
	{
		// Overwrite forward decl with definition
		SymbolNameToId[Name] = SymId;
		*ExistingSpan = TPair<int32, int32>(LineStart, LineEnd);
	}
}

void FMonolithSourceIndexer::UpdateClassMap(const FString& Name, int64 SymId, int32 LineStart, int32 LineEnd)
{
	if (Name.StartsWith(TEXT("_bases_"))) return;

	bool bIsDefinition = (LineEnd > LineStart);
	TPair<int32, int32>* ExistingSpan = ClassSpans.Find(Name);

	if (!ExistingSpan)
	{
		ClassNameToId.Add(Name, SymId);
		ClassSpans.Add(Name, TPair<int32, int32>(LineStart, LineEnd));
	}
	else if (bIsDefinition && ExistingSpan->Value <= ExistingSpan->Key)
	{
		// Overwrite forward decl with definition
		ClassNameToId[Name] = SymId;
		*ExistingSpan = TPair<int32, int32>(LineStart, LineEnd);
	}
}

// ============================================================
// Finalization — inheritance resolution + reference extraction
// ============================================================

bool FMonolithSourceIndexer::Finalize(FMonolithSourceDatabase& DB)
{
	UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Finalizing — resolving inheritance..."));

	// Phase 1: Resolve inheritance
	if (!DB.BeginTransaction())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to begin inheritance finalization transaction"));
		return false;
	}
	for (const auto& Pair : PendingBaseClasses)
	{
		if (bShouldStop)
		{
			DB.RollbackTransaction();
			return false;
		}

		const FString& ChildName = Pair.Key;
		const TArray<FString>& BaseClasses = Pair.Value;

		const int64* ChildId = ClassNameToId.Find(ChildName);
		if (!ChildId)
		{
			FScopeLock Lock(&DiagLock);
			Diagnostics.InheritanceFailed += BaseClasses.Num();
			continue;
		}

		for (const FString& BaseName : BaseClasses)
		{
			const int64* ParentId = ClassNameToId.Find(BaseName);
			if (ParentId)
			{
				DB.InsertInheritance(*ChildId, *ParentId);
				FScopeLock Lock(&DiagLock);
				Diagnostics.InheritanceResolved++;
			}
			else
			{
				FScopeLock Lock(&DiagLock);
				Diagnostics.InheritanceFailed++;
			}
		}
	}
	if (!DB.CommitTransaction())
	{
		DB.RollbackTransaction();
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to commit inheritance finalization transaction"));
		return false;
	}

	// Phase 2: Reference extraction (only new files)
	UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Extracting references from %d new files..."), NewFileIds.Num());

	FMonolithReferenceBuilder RefBuilder(DB, SymbolNameToId);

	if (!DB.BeginTransaction())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to begin reference extraction transaction"));
		return false;
	}
	int32 RefCount = 0;
	int32 FilesProcessed = 0;

	for (int64 FileId : NewFileIds)
	{
		if (bShouldStop) break;

		FString Path = DB.GetFilePath(FileId);
		if (Path == TEXT("<unknown>")) continue;

		// Only process C++ files for references (not shaders)
		FString Ext = FPaths::GetExtension(Path).ToLower();
		if (Ext != TEXT("h") && Ext != TEXT("cpp") && Ext != TEXT("inl")) continue;

		int32 Refs = RefBuilder.ExtractReferences(Path, FileId);
		RefCount += Refs;
		FilesProcessed++;

		// Periodic commit every 500 files to keep WAL size manageable
		if (FilesProcessed % 500 == 0)
		{
			if (!DB.CommitTransaction())
			{
				DB.RollbackTransaction();
				UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to commit reference extraction batch"));
				return false;
			}
			if (!DB.BeginTransaction())
			{
				UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to begin next reference extraction batch"));
				return false;
			}
			UE_LOG(LogMonolithSource, Log, TEXT("  References: %d files processed, %d refs found"), FilesProcessed, RefCount);
		}
	}
	if (bShouldStop)
	{
		DB.RollbackTransaction();
		return false;
	}
	if (!DB.CommitTransaction())
	{
		DB.RollbackTransaction();
		UE_LOG(LogMonolithSource, Error, TEXT("Indexer: failed to commit final reference extraction batch"));
		return false;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("Indexer: Reference extraction complete — %d refs from %d files"), RefCount, FilesProcessed);

	// Set meta
	DB.SetMeta(TEXT("schema_version"), FString::FromInt(MonolithSourceSchema::SchemaVersion));
	DB.SetMeta(TEXT("index_timestamp"), FString::FromInt(FDateTime::UtcNow().ToUnixTimestamp()));
	DB.SetMeta(TEXT("total_files"), FString::FromInt(TotalFilesProcessed.Load()));
	DB.SetMeta(TEXT("total_symbols"), FString::FromInt(TotalSymbolsExtracted.Load()));

	// Q5 (PRD AssetSearchSemanticSearch): compact symbols_fts once per full source
	// reindex (source_fts is a plain non-content fts5 and is excluded — it cannot take a
	// cheap 'optimize' merge). The ai/ad/au-trigger churn during indexing leaves segment
	// fragmentation that an end-of-bulk 'optimize' + PRAGMA optimize consolidates. Borrow
	// the raw handle under the DB lock per GetRawHandle()'s contract.
	{
		FScopeLock MaintLock(&DB.GetLock());
		if (FSQLiteDatabase* RawDb = DB.GetRawHandle())
		{
			FMonolithSQLiteMaintenanceOptions MaintOpts;
			MaintOpts.bRunPragmaOptimize = true;
			MaintOpts.bRunIncrementalVacuum = false;
			MaintOpts.FtsTablesToOptimize = { TEXT("symbols_fts") };
			if (!RunMonolithSQLiteMaintenance(*RawDb, MaintOpts))
			{
				UE_LOG(LogMonolithSource, Error, TEXT("Indexer: final SQLite maintenance failed"));
				return false;
			}
		}
	}

	return true;
}
