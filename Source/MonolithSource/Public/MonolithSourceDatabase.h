#pragma once

#include "CoreMinimal.h"

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

	// --- Symbol queries ---
	TArray<FMonolithSourceSymbol> SearchSymbolsFTS(const FString& Query, int32 Limit = 20);
	TArray<FMonolithSourceSymbol> GetSymbolsByName(const FString& Name, const FString& Kind = TEXT(""));
	TOptional<FMonolithSourceSymbol> GetSymbolById(int64 Id);

	// --- File queries ---
	FString GetFilePath(int64 FileId);
	TOptional<FMonolithSourceFile> FindFileBySuffix(const FString& Suffix);
	TOptional<FMonolithSourceFile> FindFileByPath(const FString& Path);

	// --- Reference queries ---
	TArray<FMonolithSourceReference> GetReferencesTo(int64 SymbolId, const FString& RefKind = TEXT(""), int32 Limit = 50);
	TArray<FMonolithSourceReference> GetReferencesFrom(int64 SymbolId, const FString& RefKind = TEXT(""), int32 Limit = 50);

	// --- Inheritance queries ---
	TArray<FMonolithSourceInheritance> GetParents(int64 SymbolId);
	TArray<FMonolithSourceInheritance> GetChildren(int64 SymbolId);

	// --- Module queries ---
	TOptional<FMonolithSourceModuleStats> GetModuleStats(const FString& ModuleName);
	TArray<FMonolithSourceSymbol> GetSymbolsInModule(const FString& ModuleName, const FString& Kind = TEXT(""), int32 Limit = 200);

	// --- Source FTS ---
	TArray<FMonolithSourceChunk> SearchSourceFTS(const FString& Query, const FString& Scope = TEXT("all"), int32 Limit = 20);
	TArray<FMonolithSourceChunk> SearchSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter, int32 Limit);
	TArray<FMonolithSourceSymbol> SearchSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter, int32 Limit);

	// --- FTS helper ---
	static FString EscapeFTS(const FString& Query);

	// --- Write methods (for C++ indexer) ---
	bool OpenForWriting(const FString& DbPath);
	bool CreateTablesIfNeeded();
	bool ResetDatabase();

	bool BeginTransaction();
	bool CommitTransaction();
	bool RollbackTransaction();

	int64 InsertModule(const FString& Name, const FString& Path, const FString& ModuleType, const FString& BuildCsPath = TEXT(""));
	int64 InsertFile(const FString& FilePath, int64 ModuleId, const FString& FileType, int32 LineCount, double LastModified);
	int64 InsertSymbol(const FString& Name, const FString& QualifiedName, const FString& Kind, int64 FileId, int32 LineStart, int32 LineEnd, int64 ParentSymbolId, const FString& Access, const FString& Signature, const FString& Docstring, bool bIsUEMacro);
	void InsertInheritance(int64 ChildId, int64 ParentId);
	void InsertReference(int64 FromSymbolId, int64 ToSymbolId, const FString& RefKind, int64 FileId, int32 Line);
	void InsertInclude(int64 FileId, const FString& IncludedPath, int32 Line);
	void InsertSourceChunks(int64 FileId, const TArray<FString>& Lines);

	void SetMeta(const FString& Key, const FString& Value);
	FString GetMeta(const FString& Key);

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
