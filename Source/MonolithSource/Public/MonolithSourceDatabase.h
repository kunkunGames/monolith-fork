#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FSQLiteDatabase;
class FSQLitePreparedStatement;

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithSource, Log, All);

struct FMonolithSourceSymbol
{
	int64 Id = 0;
	FString Name;
	FString QualifiedName;
	FString Kind;
	int64 FileId = 0;
	int32 LineStart = 0;
	int32 LineEnd = 0;
	int64 ParentSymbolId = 0;
	FString Access;
	FString Signature;
	FString Docstring;
	bool bIsUEMacro = false;
};

struct FMonolithSourceReference
{
	int64 Id = 0;
	int64 FromSymbolId = 0;
	int64 ToSymbolId = 0;
	FString RefKind;
	int64 FileId = 0;
	int32 Line = 0;
	FString FromName;
	FString ToName;
	FString Path;
};

struct FMonolithSourceInheritance
{
	int64 Id = 0;
	FString Name;
	FString QualifiedName;
	FString Kind;
	int64 FileId = 0;
	int32 LineStart = 0;
	int32 LineEnd = 0;
};

struct FMonolithSourceOverrideEdge
{
	int64 FromSymbolId = 0; // Child override method.
	int64 ToSymbolId = 0;   // Parent overridden method.
	FString FromName;
	FString FromQualifiedName;
	FString ToName;
	FString ToQualifiedName;
	FString ChildClassName;
	FString ChildClassQualifiedName;
	FString ParentClassName;
	FString ParentClassQualifiedName;
	FString Confidence;
	FString Reason;
};

struct FMonolithSourceModuleStats
{
	FString Name;
	FString Path;
	FString ModuleType;
	int32 FileCount = 0;
	TMap<FString, int32> SymbolCounts;
};

struct FMonolithSourceChunk
{
	int64 FileId = 0;
	int32 LineNumber = 0;
	FString Text;
};

struct FMonolithSourceFile
{
	int64 Id = 0;
	FString Path;
	int64 ModuleId = 0;
	FString FileType;
	int32 LineCount = 0;
};

struct FMonolithConsoleObjectRow
{
	FString Name;
	FString ObjectType;
	FString Help;
	int32 Flags = 0;
	bool bIsEnabled = false;
	bool bIsDeprecated = false;
	FString Value;
	FString DefaultValue;
	FString VariableType;
	FString SetBy;
	bool bReadOnly = false;
	bool bCheat = false;
	FString Source;
};

/** A single symbol_deprecations row (item 3). Structured so a message containing
 *  '|' cannot corrupt the version/kind fields (parity-safe vs the offline mirrors,
 *  which read columns directly). */
struct FMonolithDeprecationRow
{
	FString Version;
	FString Message;
	FString Kind;
};

/**
 * C++ wrapper around the engine source SQLite DB.
 * Supports both read-only access (Open) and read-write access (OpenForWriting)
 * for use by both query handlers and the C++ source indexer.
 */
class MONOLITHSOURCE_API FMonolithSourceDatabase
{
public:
	FMonolithSourceDatabase();
	~FMonolithSourceDatabase();

	bool Open(const FString& DbPath);
	void Close();
	bool IsOpen() const;

	/**
	 * Borrowable access to the underlying open SQLite handle.
	 *
	 * MonolithReflectionIntel's read-only query adapters (decision / risk /
	 * cppreflect / network — ~25 actions) borrow THIS handle instead of opening
	 * a SECOND handle on the same EngineSource.db file. UE 5.7 builds SQLite with
	 * a custom `unreal-fs` VFS that permits only ONE open of a given file per
	 * process (and grabs a write reservation even on a "ReadOnly" open), so a
	 * second open in the same process is rejected with SQLITE_IOERR
	 * ("disk I/O error"). Routing the read-only SELECTs through the subsystem's
	 * already-open ReadWrite handle sidesteps the single-open VFS entirely — a
	 * read-only SELECT rides a ReadWrite handle perfectly well.
	 *
	 * Returns nullptr when the DB is not open (e.g. before the first index, or
	 * while a reindex has the handle closed). Callers MUST null-check and surface
	 * a clean "not yet indexed — run source.trigger_reindex" state, never crash.
	 *
	 * THREAD SAFETY: the returned raw pointer is NOT self-synchronising. A caller
	 * that prepares/steps statements on it MUST hold this database's lock for the
	 * duration of the borrow — take it via GetLock() / FScopeLock. All of this
	 * class's own query/write methods already lock the same FCriticalSection, so
	 * a borrower that locks correctly is serialised against them.
	 */
	FSQLiteDatabase* GetRawHandle() const;

	/**
	 * Expose the database lock so an external borrower of GetRawHandle() can
	 * serialise its statement use against this class's own locked methods.
	 * Hold an FScopeLock on this for the full borrow (handle fetch through the
	 * last Step()/Destroy() on the prepared statement).
	 */
	FCriticalSection& GetLock() const { return DbLock; }

	// --- Symbol queries ---
	TArray<FMonolithSourceSymbol> SearchSymbolsFTS(const FString& Query, int32 Limit = 20);
	TArray<FMonolithSourceSymbol> GetSymbolsByName(const FString& Name, const FString& Kind = TEXT(""), int32 Limit = 0);
	TOptional<FMonolithSourceSymbol> GetSymbolById(int64 Id);

	// --- File queries ---
	FString GetFilePath(int64 FileId);
	TOptional<FMonolithSourceFile> FindFileBySuffix(const FString& Suffix);
	TOptional<FMonolithSourceFile> FindFileByPath(const FString& Path);
	/** Resolve a file's owning module name + (possibly empty) build_cs_path. False if file/module not found. */
	bool GetFileModuleInfo(int64 FileId, FString& OutModuleName, FString& OutBuildCsPath);

	// --- Reference queries ---
	TArray<FMonolithSourceReference> GetReferencesTo(int64 SymbolId, const FString& RefKind = TEXT(""), int32 Limit = 50);
	TArray<FMonolithSourceReference> GetReferencesFrom(int64 SymbolId, const FString& RefKind = TEXT(""), int32 Limit = 50);

	// --- Inheritance queries ---
	TArray<FMonolithSourceInheritance> GetParents(int64 SymbolId);
	TArray<FMonolithSourceInheritance> GetChildren(int64 SymbolId);
	TArray<FMonolithSourceOverrideEdge> GetOverridesTo(int64 SymbolId, int32 Limit = 50);
	TArray<FMonolithSourceOverrideEdge> GetOverridesFrom(int64 SymbolId, int32 Limit = 50);

	// --- Module queries ---
	TOptional<FMonolithSourceModuleStats> GetModuleStats(const FString& ModuleName);
	TArray<FMonolithSourceSymbol> GetSymbolsInModule(const FString& ModuleName, const FString& Kind = TEXT(""), int32 Limit = 200);

	// --- Source FTS ---
	TArray<FMonolithSourceChunk> SearchSourceFTS(const FString& Query, const FString& Scope = TEXT("all"), int32 Limit = 20);
	TArray<FMonolithSourceChunk> SearchSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter, int32 Limit);
	TArray<FMonolithSourceSymbol> SearchSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter, int32 Limit);

	// --- FTS COUNT(*) helpers (Survivor E, plan §3.E) ---
	// Issued ONLY on page 0 of cursor-paginated search_source so subsequent
	// pages can thread the cached total. Each helper issues a single
	// `SELECT COUNT(*) FROM <fts_table> WHERE <fts_table> MATCH ?` plus the
	// same JOIN/WHERE filters used by SearchSymbolsFTSFiltered /
	// SearchSourceFTSFiltered respectively. Dominant cost is ~50-200ms cold
	// cache per audit; warm cache is sub-ms.
	int32 CountSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter);
	int32 CountSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter);

	// --- FTS helper ---
	static FString EscapeFTS(const FString& Query);

	// --- Console object snapshot queries ---
	bool EnsureConsoleObjectSchema();
	TSharedPtr<FJsonObject> ReplaceConsoleObjectSnapshot(const TArray<FMonolithConsoleObjectRow>& Rows, const FString& SourceLabel);
	TSharedPtr<FJsonObject> SearchConsoleObjects(const FString& Query, const FString& ObjectType, int32 Limit, bool bDetail = false, int32 Offset = 0);
	TSharedPtr<FJsonObject> GetConsoleObject(const FString& Name);
	TSharedPtr<FJsonObject> ComputeConsoleHealth(bool bIncludeCounts);

	// --- CRG-inspired health / repair (additive; see MonolithSourceReview) ---
	/** Read-only schema/trigger/FTS/orphan/meta diagnostics. Never mutates. */
	TSharedPtr<FJsonObject> ComputeHealth(bool bIncludeCounts, bool bIncludeDeepChecks = false);
	/**
	 * Rebuild FTS. Default dry-run; mutates only when bExecute is true.
	 * Only `symbols_fts` is external-content and rebuildable; `source_fts` is a
	 * plain fts5 table with no backing content, so target=source always degrades
	 * to a reindex recommendation. Caller must gate on subsystem IsIndexing().
	 */
	TSharedPtr<FJsonObject> RepairFts(const FString& Target, bool bExecute);
	/**
	 * Rebuild derived CRG projection/cache tables from symbols/references/
	 * inheritance. Default dry-run; mutates only when bExecute is true.
	 */
	TSharedPtr<FJsonObject> RepairCrgCache(bool bExecute);
	TSharedPtr<FJsonObject> RepairCrgCache(const FString& Scope, bool bExecute);
	/** Cached symbol risk row, or nullptr when the derived cache is absent/stale. */
	TSharedPtr<FJsonObject> GetCachedRiskForSymbol(int64 SymbolId);
	/**
	 * Changed source path triage: changed_entities, direct caller impact, heuristic test gaps, and review queue.
	 *
	 * RX-1.1 line precision: when ChangedRanges has entries for a (normalized) path, only symbols whose
	 * [line_start,line_end] overlaps a supplied [start,end] range are returned for that path (CRG
	 * changes.py:204 rule); paths with no ranges keep the existing file-level behavior. Backward compatible:
	 * an empty ChangedRanges map reproduces the original output exactly.
	 */
	TSharedPtr<FJsonObject> DetectChanges(const TArray<FString>& ChangedPaths, int32 MaxResults, const FString& DetailLevel, const TMap<FString, TArray<TPair<int32, int32>>>& ChangedRanges = {});

	/**
	 * Parse a unified diff (git or `p4 diff -du`) into normalized-path -> added line ranges.
	 * Pure text parsing, no VCS shell-out (port of code_review_graph/changes.py:_parse_unified_diff).
	 * Public + static for offline/editor parity and direct test coverage.
	 */
	static TMap<FString, TArray<TPair<int32, int32>>> ParseUnifiedDiffRanges(const FString& DiffText);
	/** Advisory dead-symbol candidates. Read-only; never mutates and never reports high confidence. */
	TSharedPtr<FJsonObject> FindUnused(const FString& Kind, int32 Limit, const FString& MinConfidence);
	/** Read-only pre-merge gate composed from health, detect_changes, and optional find_unused. */
	TSharedPtr<FJsonObject> PreMergeCheck(const TArray<FString>& ChangedPaths, int32 MaxResults, int32 UnusedLimit, const FString& DetailLevel, bool bIncludeUnused);
	/** Capture the current derived CRG projection manifest. Default dry-run; mutates only when bExecute is true. */
	TSharedPtr<FJsonObject> Snapshot(const FString& Label, bool bExecute);
	/** Read-only diff between a stored CRG projection snapshot and another stored/current manifest. */
	TSharedPtr<FJsonObject> DiffSnapshots(const FString& Before, const FString& After, int32 Limit);
	/** Top source review hotspots from CRG/native fan-in, fan-out, risk, LOC, and override signals. */
	TSharedPtr<FJsonObject> ReviewHotspots(const FString& Kind, int32 Limit, int32 MinLines, bool bIncludeQuestions);

	// --- Write methods (for C++ indexer) ---
	bool OpenForWriting(const FString& DbPath);
	bool CreateTablesIfNeeded();
	bool ResetDatabase();

	bool BeginTransaction();
	bool CommitTransaction();
	bool RollbackTransaction();

	/** Remove indexed files and dependent rows under the supplied roots before a scoped reindex. Returns -1 on failure. */
	int32 PruneIndexedFilesUnderRoots(const TArray<FString>& RootPaths);
	/** Refresh derived source CRG rows for newly indexed files plus pending prune neighbors. */
	TSharedPtr<FJsonObject> RefreshCrgCacheForFiles(const TSet<int64>& FileIds, const FString& Context);

	int64 InsertModule(const FString& Name, const FString& Path, const FString& ModuleType, const FString& BuildCsPath = TEXT(""));
	int64 InsertFile(const FString& FilePath, int64 ModuleId, const FString& FileType, int32 LineCount, double LastModified);
	int64 InsertSymbol(const FString& Name, const FString& QualifiedName, const FString& Kind, int64 FileId, int32 LineStart, int32 LineEnd, int64 ParentSymbolId, const FString& Access, const FString& Signature, const FString& Docstring, bool bIsUEMacro);
	void InsertInheritance(int64 ChildId, int64 ParentId);
	void InsertReference(int64 FromSymbolId, int64 ToSymbolId, const FString& RefKind, int64 FileId, int32 Line);
	void InsertInclude(int64 FileId, const FString& IncludedPath, int32 Line);
	void InsertSourceChunks(int64 FileId, const TArray<FString>& Lines);

	void SetMeta(const FString& Key, const FString& Value);
	FString GetMeta(const FString& Key);

	// --- Deprecation queries (item 3) ---
	// symbol_id is NULLABLE: pass 0 to store NULL (class-body methods have no
	// symbols row — Step-0 finding). Lookups key on symbol_name.
	void InsertDeprecation(int64 SymbolId, const FString& SymbolName, const FString& Version, const FString& Message, const FString& Kind);
	/** Returns the deprecation row for the first matching symbol, or unset when not deprecated. */
	TOptional<FMonolithDeprecationRow> GetDeprecation(const FString& SymbolName);
	/** Batch lookup: maps each input symbol name that IS deprecated to its row. Absent keys = not deprecated. */
	TMap<FString, FMonolithDeprecationRow> GetDeprecationsBatch(const TArray<FString>& SymbolNames);
	/** Total rows in symbol_deprecations (used to detect the "index empty" state — Decision 3). */
	int32 GetDeprecationCount();

	// --- Incremental indexing support ---
	int32 LoadExistingSymbols(TMap<FString, int64>& OutSymbolNameToId, TMap<FString, int64>& OutClassNameToId,
		TMap<FString, TPair<int32,int32>>& OutSymbolSpans, TMap<FString, TPair<int32,int32>>& OutClassSpans);

private:
	FMonolithSourceSymbol ReadSymbolFromStatement(FSQLitePreparedStatement& Stmt);
	FMonolithSourceReference ReadReferenceFromStatement(FSQLitePreparedStatement& Stmt, bool bIsRefTo);

	FSQLiteDatabase* Database = nullptr;
	FString CachedDbPath;
	mutable FCriticalSection DbLock;
};
