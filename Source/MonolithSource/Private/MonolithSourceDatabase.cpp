#include "MonolithSourceDatabase.h"
#include "MonolithSourceSchema.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include <initializer_list>

DEFINE_LOG_CATEGORY(LogMonolithSource);

// ============================================================
// Helper: execute a multi-statement SQL string statement-by-statement.
// FSQLiteDatabase::Execute() only runs the first statement when given
// a semicolon-separated multi-statement string, so we must split manually.
//
// Splits on ';' at BEGIN/END nesting depth 0, so trigger bodies like
//   BEGIN INSERT INTO ...; END;
// are kept intact as a single statement.
// ============================================================
static bool ExecuteMulti(FSQLiteDatabase& DB, const TCHAR* SQL)
{
	const FString Source(SQL);
	const int32 Len = Source.Len();

	int32 Depth = 0;   // BEGIN...END nesting depth
	FString Current;

	auto FlushStatement = [&]() -> bool
	{
		FString Stmt = Current.TrimStartAndEnd();
		Current.Empty();
		if (Stmt.IsEmpty())
		{
			return true;
		}
		return DB.Execute(*Stmt);
	};

	int32 i = 0;
	while (i < Len)
	{
		const TCHAR Ch = Source[i];

		// Detect SQL keywords (BEGIN / END) at word boundaries.
		// String literals are not present in our DDL so we skip quote handling.
		if (FChar::IsAlpha(Ch) || Ch == TEXT('_'))
		{
			const int32 WordStart = i;
			while (i < Len && (FChar::IsAlnum(Source[i]) || Source[i] == TEXT('_')))
			{
				++i;
			}
			const FString Word = Source.Mid(WordStart, i - WordStart).ToUpper();
			Current += Source.Mid(WordStart, i - WordStart);

			if (Word == TEXT("BEGIN"))
			{
				++Depth;
			}
			else if (Word == TEXT("END") && Depth > 0)
			{
				--Depth;
			}
			continue;
		}

		if (Ch == TEXT(';') && Depth == 0)
		{
			++i;
			if (!FlushStatement())
			{
				return false;
			}
			continue;
		}

		Current += Ch;
		++i;
	}

	// Flush any trailing statement (no trailing semicolon)
	return FlushStatement();
}

// ============================================================
// Constructor / Destructor
// ============================================================

FMonolithSourceDatabase::FMonolithSourceDatabase()
{
}

FMonolithSourceDatabase::~FMonolithSourceDatabase()
{
	Close();
}

bool FMonolithSourceDatabase::Open(const FString& DbPath)
{
	FScopeLock Lock(&DbLock);

	if (Database)
	{
		Close();
	}

	CachedDbPath = DbPath;

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*DbPath))
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Engine source DB not found: %s"), *DbPath);
		return false;
	}

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to open engine source DB: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Force DELETE journal mode — WAL breaks ReadOnly on Windows
	Database->Execute(TEXT("PRAGMA journal_mode=DELETE;"));

	UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB opened: %s"), *DbPath);
	return true;
}

static void AddNextActions(const TSharedPtr<FJsonObject>& Root, std::initializer_list<const TCHAR*> Actions)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const TCHAR* Action : Actions)
	{
		Arr.Add(MakeShared<FJsonValueString>(FString(Action)));
	}
	Root->SetArrayField(TEXT("next_actions"), Arr);
}

void FMonolithSourceDatabase::Close()
{
	FScopeLock Lock(&DbLock);
	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}
}

bool FMonolithSourceDatabase::IsOpen() const
{
	FScopeLock Lock(&DbLock);
	return Database != nullptr && Database->IsValid();
}

// ============================================================
// FTS escape — mirrors Python _escape_fts()
// ============================================================

FString FMonolithSourceDatabase::EscapeFTS(const FString& Query)
{
	// Replace :: with space
	FString Q = Query.Replace(TEXT("::"), TEXT(" "));

	// Strip non-alphanumeric/non-space
	FString Cleaned;
	Cleaned.Reserve(Q.Len());
	for (TCHAR Ch : Q)
	{
		if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT(' '))
		{
			Cleaned += Ch;
		}
	}

	// Split into tokens, wrap each with quotes and trailing *
	TArray<FString> Tokens;
	Cleaned.ParseIntoArray(Tokens, TEXT(" "), true);

	if (Tokens.Num() == 0)
	{
		return TEXT("\"\"");
	}

	FString Result;
	int32 TotalLen = 0;
	for (const FString& Token : Tokens)
	{
		TotalLen += Token.Len() + 4; // Quotes, star, space
	}
	Result.Reserve(TotalLen);

	for (int32 i = 0; i < Tokens.Num(); ++i)
	{
		if (i > 0) Result += TEXT(" ");
		Result += TEXT("\"");
		Result += Tokens[i];
		Result += TEXT("\"*");
	}
	return Result;
}

// ============================================================
// Row readers
// ============================================================

FMonolithSourceSymbol FMonolithSourceDatabase::ReadSymbolFromStatement(FSQLitePreparedStatement& Stmt)
{
	FMonolithSourceSymbol Sym;
	Stmt.GetColumnValueByIndex(0, Sym.Id);
	Stmt.GetColumnValueByIndex(1, Sym.Name);
	Stmt.GetColumnValueByIndex(2, Sym.QualifiedName);
	Stmt.GetColumnValueByIndex(3, Sym.Kind);
	Stmt.GetColumnValueByIndex(4, Sym.FileId);
	int32 LineStart = 0, LineEnd = 0;
	Stmt.GetColumnValueByIndex(5, LineStart);
	Stmt.GetColumnValueByIndex(6, LineEnd);
	Sym.LineStart = LineStart;
	Sym.LineEnd = LineEnd;
	// parent_symbol_id at index 7 — skip
	Stmt.GetColumnValueByIndex(8, Sym.Access);
	Stmt.GetColumnValueByIndex(9, Sym.Signature);
	Stmt.GetColumnValueByIndex(10, Sym.Docstring);
	int32 IsUEMacro = 0;
	Stmt.GetColumnValueByIndex(11, IsUEMacro);
	Sym.bIsUEMacro = IsUEMacro != 0;
	return Sym;
}

FMonolithSourceReference FMonolithSourceDatabase::ReadReferenceFromStatement(FSQLitePreparedStatement& Stmt, bool bIsRefTo)
{
	FMonolithSourceReference Ref;
	Stmt.GetColumnValueByIndex(0, Ref.Id);
	Stmt.GetColumnValueByIndex(1, Ref.FromSymbolId);
	Stmt.GetColumnValueByIndex(2, Ref.ToSymbolId);
	Stmt.GetColumnValueByIndex(3, Ref.RefKind);
	Stmt.GetColumnValueByIndex(4, Ref.FileId);
	int32 Line = 0;
	Stmt.GetColumnValueByIndex(5, Line);
	Ref.Line = Line;
	if (bIsRefTo)
	{
		Stmt.GetColumnValueByIndex(6, Ref.FromName);
	}
	else
	{
		Stmt.GetColumnValueByIndex(6, Ref.ToName);
	}
	Stmt.GetColumnValueByIndex(7, Ref.Path);
	return Ref;
}

// ============================================================
// Symbol queries
// ============================================================

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolsByName(const FString& Name, const FString& Kind)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	if (Kind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE name = ? ORDER BY (line_end > line_start) DESC;"));
		Stmt.SetBindingValueByIndex(1, Name);
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE name = ? AND kind = ? ORDER BY (line_end > line_start) DESC;"));
		Stmt.SetBindingValueByIndex(1, Name);
		Stmt.SetBindingValueByIndex(2, Kind);
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::SearchSymbolsFTS(const FString& Query, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FString FTSQuery = EscapeFTS(Query);

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols_fts f JOIN symbols s ON s.id = f.rowid WHERE symbols_fts MATCH ? ORDER BY bm25(symbols_fts) LIMIT ?;"));
	Stmt.SetBindingValueByIndex(1, FTSQuery);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

TOptional<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolById(int64 Id)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro FROM symbols WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, Id);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		return ReadSymbolFromStatement(Stmt);
	}
	return {};
}

// ============================================================
// File queries
// ============================================================

FString FMonolithSourceDatabase::GetFilePath(int64 FileId)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return TEXT("<unknown>");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT path FROM files WHERE id = ?;"));
	Stmt.SetBindingValueByIndex(1, FileId);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Path;
		Stmt.GetColumnValueByIndex(0, Path);
		return Path;
	}
	return TEXT("<unknown>");
}

TOptional<FMonolithSourceFile> FMonolithSourceDatabase::FindFileBySuffix(const FString& Suffix)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	FString EscapedSuffix = Suffix.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
	Stmt.Create(*Database, TEXT("SELECT id, path, module_id, file_type, line_count FROM files WHERE path LIKE ? ESCAPE '\\' LIMIT 1;"));
	Stmt.SetBindingValueByIndex(1, FString::Printf(TEXT("%%%s"), *EscapedSuffix));

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceFile File;
		Stmt.GetColumnValueByIndex(0, File.Id);
		Stmt.GetColumnValueByIndex(1, File.Path);
		Stmt.GetColumnValueByIndex(2, File.ModuleId);
		Stmt.GetColumnValueByIndex(3, File.FileType);
		int32 LC = 0;
		Stmt.GetColumnValueByIndex(4, LC);
		File.LineCount = LC;
		return File;
	}
	return {};
}

TOptional<FMonolithSourceFile> FMonolithSourceDatabase::FindFileByPath(const FString& Path)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT id, path, module_id, file_type, line_count FROM files WHERE path = ?;"));
	Stmt.SetBindingValueByIndex(1, Path);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceFile File;
		Stmt.GetColumnValueByIndex(0, File.Id);
		Stmt.GetColumnValueByIndex(1, File.Path);
		Stmt.GetColumnValueByIndex(2, File.ModuleId);
		Stmt.GetColumnValueByIndex(3, File.FileType);
		int32 LC = 0;
		Stmt.GetColumnValueByIndex(4, LC);
		File.LineCount = LC;
		return File;
	}
	return {};
}

// ============================================================
// Reference queries
// ============================================================

TArray<FMonolithSourceReference> FMonolithSourceDatabase::GetReferencesTo(int64 SymbolId, const FString& RefKind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceReference> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FSQLitePreparedStatement Stmt;
	if (RefKind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.from_symbol_id JOIN files f ON f.id = r.file_id WHERE r.to_symbol_id = ? AND r.ref_kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, RefKind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(SafeLimit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadReferenceFromStatement(Stmt, true));
	}
	return Result;
}

TArray<FMonolithSourceReference> FMonolithSourceDatabase::GetReferencesFrom(int64 SymbolId, const FString& RefKind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceReference> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FSQLitePreparedStatement Stmt;
	if (RefKind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT r.id, r.from_symbol_id, r.to_symbol_id, r.ref_kind, r.file_id, r.line, s.name, f.path FROM \"references\" r JOIN symbols s ON s.id = r.to_symbol_id JOIN files f ON f.id = r.file_id WHERE r.from_symbol_id = ? AND r.ref_kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, SymbolId);
		Stmt.SetBindingValueByIndex(2, RefKind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(SafeLimit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadReferenceFromStatement(Stmt, false));
	}
	return Result;
}

// ============================================================
// Inheritance queries
// ============================================================

TArray<FMonolithSourceInheritance> FMonolithSourceDatabase::GetParents(int64 SymbolId)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceInheritance> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end FROM inheritance i JOIN symbols s ON s.id = i.parent_id WHERE i.child_id = ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceInheritance Inh;
		Stmt.GetColumnValueByIndex(0, Inh.Id);
		Stmt.GetColumnValueByIndex(1, Inh.Name);
		Stmt.GetColumnValueByIndex(2, Inh.QualifiedName);
		Stmt.GetColumnValueByIndex(3, Inh.Kind);
		Stmt.GetColumnValueByIndex(4, Inh.FileId);
		int32 LS = 0, LE = 0;
		Stmt.GetColumnValueByIndex(5, LS);
		Stmt.GetColumnValueByIndex(6, LE);
		Inh.LineStart = LS;
		Inh.LineEnd = LE;
		Result.Add(MoveTemp(Inh));
	}
	return Result;
}

TArray<FMonolithSourceInheritance> FMonolithSourceDatabase::GetChildren(int64 SymbolId)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceInheritance> Result;
	if (!Database || !Database->IsValid()) return Result;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end FROM inheritance i JOIN symbols s ON s.id = i.child_id WHERE i.parent_id = ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceInheritance Inh;
		Stmt.GetColumnValueByIndex(0, Inh.Id);
		Stmt.GetColumnValueByIndex(1, Inh.Name);
		Stmt.GetColumnValueByIndex(2, Inh.QualifiedName);
		Stmt.GetColumnValueByIndex(3, Inh.Kind);
		Stmt.GetColumnValueByIndex(4, Inh.FileId);
		int32 LS = 0, LE = 0;
		Stmt.GetColumnValueByIndex(5, LS);
		Stmt.GetColumnValueByIndex(6, LE);
		Inh.LineStart = LS;
		Inh.LineEnd = LE;
		Result.Add(MoveTemp(Inh));
	}
	return Result;
}

// ============================================================
// Module queries
// ============================================================

TOptional<FMonolithSourceModuleStats> FMonolithSourceDatabase::GetModuleStats(const FString& ModuleName)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	// Get module info
	FSQLitePreparedStatement ModStmt;
	ModStmt.Create(*Database, TEXT("SELECT id, name, path, module_type FROM modules WHERE name = ?;"));
	ModStmt.SetBindingValueByIndex(1, ModuleName);

	if (ModStmt.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return {};
	}

	FMonolithSourceModuleStats Stats;
	int64 ModId = 0;
	ModStmt.GetColumnValueByIndex(0, ModId);
	ModStmt.GetColumnValueByIndex(1, Stats.Name);
	ModStmt.GetColumnValueByIndex(2, Stats.Path);
	ModStmt.GetColumnValueByIndex(3, Stats.ModuleType);

	// File count
	FSQLitePreparedStatement FileStmt;
	FileStmt.Create(*Database, TEXT("SELECT COUNT(*) FROM files WHERE module_id = ?;"));
	FileStmt.SetBindingValueByIndex(1, ModId);
	if (FileStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Count = 0;
		FileStmt.GetColumnValueByIndex(0, Count);
		Stats.FileCount = static_cast<int32>(Count);
	}

	// Symbol counts by kind
	FSQLitePreparedStatement KindStmt;
	KindStmt.Create(*Database, TEXT("SELECT s.kind, COUNT(*) as cnt FROM symbols s JOIN files f ON f.id = s.file_id WHERE f.module_id = ? GROUP BY s.kind;"));
	KindStmt.SetBindingValueByIndex(1, ModId);
	while (KindStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Kind;
		int64 Count = 0;
		KindStmt.GetColumnValueByIndex(0, Kind);
		KindStmt.GetColumnValueByIndex(1, Count);
		Stats.SymbolCounts.Add(Kind, static_cast<int32>(Count));
	}

	return Stats;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolsInModule(const FString& ModuleName, const FString& Kind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FSQLitePreparedStatement Stmt;
	if (Kind.IsEmpty())
	{
		Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, ModuleName);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s JOIN files f ON f.id = s.file_id JOIN modules m ON m.id = f.module_id WHERE m.name = ? AND s.kind = ? LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, ModuleName);
		Stmt.SetBindingValueByIndex(2, Kind);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(SafeLimit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

// ============================================================
// Source FTS
// ============================================================

TArray<FMonolithSourceChunk> FMonolithSourceDatabase::SearchSourceFTS(const FString& Query, const FString& Scope, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceChunk> Result;
	if (!Database || !Database->IsValid()) return Result;

	FString FTSQuery = EscapeFTS(Query);

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FSQLitePreparedStatement Stmt;
	if (Scope == TEXT("all"))
	{
		Stmt.Create(*Database, TEXT("SELECT f.file_id, f.line_number, f.text FROM source_fts f WHERE source_fts MATCH ? ORDER BY bm25(source_fts) LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
	}
	else
	{
		Stmt.Create(*Database, TEXT("SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf JOIN files fi ON fi.id = sf.file_id WHERE source_fts MATCH ? AND fi.file_type = ? ORDER BY bm25(source_fts) LIMIT ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);
		Stmt.SetBindingValueByIndex(2, Scope);
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(SafeLimit));
	}

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceChunk Chunk;
		Stmt.GetColumnValueByIndex(0, Chunk.FileId);
		int32 LN = 0;
		Stmt.GetColumnValueByIndex(1, LN);
		Chunk.LineNumber = LN;
		Stmt.GetColumnValueByIndex(2, Chunk.Text);
		Result.Add(MoveTemp(Chunk));
	}
	return Result;
}

TArray<FMonolithSourceChunk> FMonolithSourceDatabase::SearchSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceChunk> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	if (Scope == TEXT("all") && Module.IsEmpty() && PathFilter.IsEmpty())
	{
		return SearchSourceFTS(Query, Scope, SafeLimit);
	}

	FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT sf.file_id, sf.line_number, sf.text FROM source_fts sf JOIN files fi ON fi.id = sf.file_id ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("source_fts MATCH ?"));

	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (Scope != TEXT("all"))
	{
		Conditions.Add(TEXT("fi.file_type = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ? ESCAPE '\\'"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += TEXT(" ORDER BY bm25(source_fts) LIMIT ?;");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (Scope != TEXT("all"))
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Scope);
	}
	if (!PathFilter.IsEmpty())
	{
		FString EscapedPathFilter = PathFilter.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *EscapedPathFilter));
	}
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<int64>(SafeLimit));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceChunk Chunk;
		Stmt.GetColumnValueByIndex(0, Chunk.FileId);
		int32 LN = 0;
		Stmt.GetColumnValueByIndex(1, LN);
		Chunk.LineNumber = LN;
		Stmt.GetColumnValueByIndex(2, Chunk.Text);
		Result.Add(MoveTemp(Chunk));
	}
	return Result;
}

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::SearchSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);

	FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols_fts f JOIN symbols s ON s.id = f.rowid ");
	TArray<FString> Conditions;
	Conditions.Add(TEXT("symbols_fts MATCH ?"));

	if (!Module.IsEmpty() || !PathFilter.IsEmpty())
	{
		SQL += TEXT("JOIN files fi ON fi.id = s.file_id ");
	}
	if (!Module.IsEmpty())
	{
		SQL += TEXT("JOIN modules m ON m.id = fi.module_id ");
		Conditions.Add(TEXT("m.name = ?"));
	}
	if (!Kind.IsEmpty())
	{
		Conditions.Add(TEXT("s.kind = ?"));
	}
	if (!PathFilter.IsEmpty())
	{
		Conditions.Add(TEXT("fi.path LIKE ? ESCAPE '\\'"));
	}

	SQL += TEXT("WHERE ") + FString::Join(Conditions, TEXT(" AND "));
	SQL += TEXT(" ORDER BY bm25(symbols_fts) LIMIT ?;");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, *SQL);

	int32 BindIdx = 1;
	Stmt.SetBindingValueByIndex(BindIdx++, FTSQuery);
	if (!Module.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Module);
	}
	if (!Kind.IsEmpty())
	{
		Stmt.SetBindingValueByIndex(BindIdx++, Kind);
	}
	if (!PathFilter.IsEmpty())
	{
		FString EscapedPathFilter = PathFilter.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
		Stmt.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s%%"), *EscapedPathFilter));
	}
	Stmt.SetBindingValueByIndex(BindIdx++, static_cast<int64>(SafeLimit));

	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Result.Add(ReadSymbolFromStatement(Stmt));
	}
	return Result;
}

// ============================================================
// Write API — OpenForWriting
// ============================================================

bool FMonolithSourceDatabase::OpenForWriting(const FString& DbPath)
{
	FScopeLock Lock(&DbLock);

	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}

	CachedDbPath = DbPath;

	Database = new FSQLiteDatabase();
	if (!Database->Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("OpenForWriting: failed to open/create DB: %s"), *DbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	// Belt-and-suspenders: force DELETE journal mode (WAL breaks ReadOnly on Windows, per lesson learned)
	Database->Execute(TEXT("PRAGMA journal_mode=DELETE;"));
	Database->Execute(TEXT("PRAGMA synchronous=NORMAL;"));
	Database->Execute(TEXT("PRAGMA cache_size=-64000;"));   // 64 MB page cache

	UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB opened for writing: %s"), *DbPath);
	return true;
}

// ============================================================
// Schema management
// ============================================================

bool FMonolithSourceDatabase::CreateTablesIfNeeded()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DB not open"));
		return false;
	}

	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Tables))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_Tables failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_FTS))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_FTS failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Triggers))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("CreateTablesIfNeeded: DDL_Triggers failed — %s"), *Database->GetLastError());
		return false;
	}

	// Stamp the schema version into meta
	FSQLitePreparedStatement MetaStmt;
	MetaStmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	MetaStmt.SetBindingValueByIndex(1, FString(TEXT("schema_version")));
	MetaStmt.SetBindingValueByIndex(2, FString::FromInt(MonolithSourceSchema::SchemaVersion));
	MetaStmt.Step();

	UE_LOG(LogMonolithSource, Log, TEXT("Schema created/verified (version %d)"), MonolithSourceSchema::SchemaVersion);
	return true;
}

bool FMonolithSourceDatabase::ResetDatabase()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DB not open"));
		return false;
	}

	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Drop))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: drop failed — %s"), *Database->GetLastError());
		return false;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("ResetDatabase: all tables dropped, recreating schema"));

	// Execute DDL inline (we're already holding DbLock, can't call CreateTablesIfNeeded)
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Tables))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_Tables failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_FTS))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_FTS failed — %s"), *Database->GetLastError());
		return false;
	}
	if (!ExecuteMulti(*Database, MonolithSourceSchema::DDL_Triggers))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: DDL_Triggers failed — %s"), *Database->GetLastError());
		return false;
	}

	FSQLitePreparedStatement MetaStmt;
	MetaStmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	MetaStmt.SetBindingValueByIndex(1, FString(TEXT("schema_version")));
	MetaStmt.SetBindingValueByIndex(2, FString::FromInt(MonolithSourceSchema::SchemaVersion));
	MetaStmt.Step();

	UE_LOG(LogMonolithSource, Log, TEXT("ResetDatabase: schema recreated successfully"));
	return true;
}

// ============================================================
// Transaction control
// ============================================================

bool FMonolithSourceDatabase::BeginTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("BEGIN;"));
}

bool FMonolithSourceDatabase::CommitTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("COMMIT;"));
}

bool FMonolithSourceDatabase::RollbackTransaction()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return false;
	return Database->Execute(TEXT("ROLLBACK;"));
}

// ============================================================
// CRG-inspired health / repair
//
// Adapted from code-review-graph (0919071a): non-fatal health post-processing
// and FTS rebuild. Engine-source-domain native: only the existing
// modules/files/symbols/inheritance/"references"/symbols_fts/source_fts schema.
// ============================================================

TSharedPtr<FJsonObject> FMonolithSourceDatabase::ComputeHealth(bool bIncludeCounts)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetBoolField(TEXT("include_counts"), bIncludeCounts);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetBoolField(TEXT("include_counts"), bIncludeCounts);
	Root->SetObjectField(TEXT("limits"), Limits);

	auto Check = [&](const FString& Name, bool bPass, const FString& Detail)
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("check"), Name);
		C->SetStringField(TEXT("result"), bPass ? TEXT("ok") : TEXT("warning"));
		C->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(C));
		if (!bPass) Warnings.Add(MakeShared<FJsonValueString>(Detail));
	};

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("checks"), Checks);
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}

	auto Exists = [&](const TCHAR* Type, const TCHAR* Name) -> bool
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = ? AND name = ?;")))
		{
			return false;
		}
		S.SetBindingValueByIndex(1, FString(Type));
		S.SetBindingValueByIndex(2, FString(Name));
		return S.Step() == ESQLitePreparedStatementStepResult::Row;
	};
	auto CountOf = [&](const TCHAR* Sql) -> int64
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, Sql)) return -1;
		int64 N = 0;
		if (S.Step() == ESQLitePreparedStatementStepResult::Row) S.GetColumnValueByIndex(0, N);
		return N;
	};

	static const TCHAR* Tables[] = { TEXT("modules"), TEXT("files"), TEXT("symbols"),
		TEXT("inheritance"), TEXT("references"), TEXT("includes"), TEXT("meta") };
	for (const TCHAR* T : Tables)
	{
		const bool bHas = Exists(TEXT("table"), T);
		Check(FString::Printf(TEXT("table:%s"), T), bHas,
			bHas ? FString::Printf(TEXT("table %s present"), T)
				: FString::Printf(TEXT("missing table %s"), T));
	}

	for (const TCHAR* F : { TEXT("symbols_fts"), TEXT("source_fts") })
	{
		const bool bHas = Exists(TEXT("table"), F);
		Check(FString::Printf(TEXT("fts:%s"), F), bHas,
			bHas ? FString::Printf(TEXT("FTS table %s present"), F)
				: FString::Printf(TEXT("missing FTS table %s"), F));
	}

	// Source has exactly symbols_ai / symbols_ad (no _au, no source_fts trigger).
	for (const TCHAR* Tr : { TEXT("symbols_ai"), TEXT("symbols_ad") })
	{
		const bool bHas = Exists(TEXT("trigger"), Tr);
		Check(FString::Printf(TEXT("trigger:%s"), Tr), bHas,
			bHas ? FString::Printf(TEXT("trigger %s present"), Tr)
				: FString::Printf(TEXT("missing trigger %s (symbols_fts may drift)"), Tr));
	}

	FString SchemaVer;
	{
		FSQLitePreparedStatement S;
		if (S.Create(*Database, TEXT("SELECT value FROM meta WHERE key = 'schema_version';"))
			&& S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			S.GetColumnValueByIndex(0, SchemaVer);
		}
	}
	Check(TEXT("meta:schema_version"), SchemaVer == TEXT("1"),
		SchemaVer.IsEmpty() ? TEXT("meta.schema_version missing")
			: FString::Printf(TEXT("schema_version=%s (expected 1)"), *SchemaVer));

	const int64 OrphanRefs = CountOf(TEXT(
		"SELECT COUNT(*) FROM \"references\" r "
		"WHERE r.from_symbol_id NOT IN (SELECT id FROM symbols) "
		"   OR r.to_symbol_id NOT IN (SELECT id FROM symbols);"));
	Check(TEXT("integrity:orphan_references"), OrphanRefs == 0,
		OrphanRefs == 0 ? TEXT("no orphan reference rows")
			: FString::Printf(TEXT("%lld orphan reference row(s)"), OrphanRefs));

	const int64 SymCnt = CountOf(TEXT("SELECT COUNT(*) FROM symbols;"));
	const int64 SymFtsCnt = CountOf(TEXT("SELECT COUNT(*) FROM symbols_fts;"));
	Check(TEXT("fts:symbols_row_parity"), SymCnt == SymFtsCnt,
		FString::Printf(TEXT("symbols=%lld symbols_fts=%lld%s"), SymCnt, SymFtsCnt,
			SymCnt == SymFtsCnt ? TEXT("") : TEXT(" (mismatch -> source.repair_fts target=symbols)")));

	// source_fts is a plain (non external-content) fts5 table — a row-count
	// difference is expected and informational, never a warning.
	const int64 SrcFtsCnt = CountOf(TEXT("SELECT COUNT(*) FROM source_fts;"));
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("check"), TEXT("fts:source_fts_info"));
		C->SetStringField(TEXT("result"), TEXT("info"));
		C->SetStringField(TEXT("detail"), FString::Printf(
			TEXT("source_fts rows=%lld (plain fts5; not rebuildable — reindex to repair)"), SrcFtsCnt));
		Checks.Add(MakeShared<FJsonValueObject>(C));
	}

	FString Journal;
	{
		FSQLitePreparedStatement S;
		if (S.Create(*Database, TEXT("PRAGMA journal_mode;"))
			&& S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			S.GetColumnValueByIndex(0, Journal);
		}
	}
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("schema_version"), SchemaVer);
	Schema->SetStringField(TEXT("journal_mode"), Journal);
	Root->SetObjectField(TEXT("schema"), Schema);

	if (bIncludeCounts)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("symbols"), static_cast<double>(SymCnt));
		Counts->SetNumberField(TEXT("references"),
			static_cast<double>(CountOf(TEXT("SELECT COUNT(*) FROM \"references\";"))));
		Counts->SetNumberField(TEXT("inheritance"),
			static_cast<double>(CountOf(TEXT("SELECT COUNT(*) FROM inheritance;"))));
		Counts->SetNumberField(TEXT("source_fts"), static_cast<double>(SrcFtsCnt));
		Root->SetObjectField(TEXT("row_counts"), Counts);
	}

	const bool bHealthy = Warnings.Num() == 0;
	Root->SetStringField(TEXT("status"), bHealthy ? TEXT("ok") : TEXT("warning"));
	Root->SetStringField(TEXT("summary"), bHealthy
		? TEXT("EngineSource schema, triggers, symbols_fts parity and integrity OK")
		: FString::Printf(TEXT("%d health warning(s)"), Warnings.Num()));
	Root->SetArrayField(TEXT("checks"), Checks);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNextActions(Root, { TEXT("source.repair_fts"), TEXT("source.trigger_project_reindex"), TEXT("source.search_source") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::RepairFts(const FString& Target, bool bExecute)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString T = Target.IsEmpty() ? TEXT("all") : Target;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target"), T);
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetStringField(TEXT("target"), T);
	Limits->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("limits"), Limits);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Plan;

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}

	const bool bDoSymbols = (T == TEXT("all") || T == TEXT("symbols"));
	const bool bAskedSource = (T == TEXT("all") || T == TEXT("source"));

	if (T != TEXT("all") && T != TEXT("symbols") && T != TEXT("source"))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Unknown target '%s' (expected all|symbols|source)"), *T));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.repair_fts"), TEXT("source.health") });
		return Root;
	}

	auto Count = [&](const TCHAR* Sql) -> int64
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, Sql)) return -1;
		int64 N = 0;
		if (S.Step() == ESQLitePreparedStatementStepResult::Row) S.GetColumnValueByIndex(0, N);
		return N;
	};

	TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
	if (bDoSymbols) Before->SetNumberField(TEXT("symbols_fts"),
		static_cast<double>(Count(TEXT("SELECT COUNT(*) FROM symbols_fts;"))));
	Root->SetObjectField(TEXT("before"), Before);

	if (bDoSymbols)
	{
		Plan.Add(MakeShared<FJsonValueString>(
			TEXT("INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');")));
	}
	if (bAskedSource)
	{
		// source_fts has no content table — 'rebuild' is meaningless. Always
		// degrade to a reindex recommendation regardless of execute.
		Warnings.Add(MakeShared<FJsonValueString>(TEXT(
			"source_fts is a plain fts5 table (no backing content); it cannot be "
			"rebuilt in place. Run source.trigger_reindex / trigger_project_reindex "
			"to repopulate source line search.")));
	}
	Root->SetArrayField(TEXT("plan"), Plan);

	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), bDoSymbols
			? TEXT("Dry-run: symbols_fts would be rebuilt. Pass execute=true to apply.")
			: TEXT("Dry-run: nothing rebuildable for this target."));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.repair_fts (execute=true)"), TEXT("source.health") });
		return Root;
	}

	bool bOk = true;
	if (bDoSymbols)
	{
		bOk = Database->Execute(TEXT("BEGIN;"));
		if (bOk && !Database->Execute(TEXT("INSERT INTO symbols_fts(symbols_fts) VALUES('rebuild');")))
		{
			bOk = false;
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("symbols_fts rebuild failed")));
		}
		if (bOk) Database->Execute(TEXT("COMMIT;"));
		else Database->Execute(TEXT("ROLLBACK;"));
	}

	TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
	if (bDoSymbols) After->SetNumberField(TEXT("symbols_fts"),
		static_cast<double>(Count(TEXT("SELECT COUNT(*) FROM symbols_fts;"))));
	Root->SetObjectField(TEXT("after"), After);

	Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
	Root->SetStringField(TEXT("summary"), bOk
		? (bDoSymbols ? TEXT("Rebuilt symbols_fts")
			: TEXT("Nothing rebuilt; see warnings for source_fts reindex guidance"))
		: TEXT("symbols_fts rebuild failed; rolled back"));
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNextActions(Root, { TEXT("source.health"), TEXT("source.search_symbols") });
	return Root;
}

// ============================================================
// Insert helpers
// ============================================================

int64 FMonolithSourceDatabase::InsertModule(const FString& Name, const FString& Path, const FString& ModuleType, const FString& BuildCsPath)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	// INSERT OR IGNORE — if UNIQUE(name,path) already exists, this is a no-op
	FSQLitePreparedStatement InsStmt;
	InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO modules (name, path, module_type, build_cs_path) VALUES (?, ?, ?, ?);"));
	InsStmt.SetBindingValueByIndex(1, Name);
	InsStmt.SetBindingValueByIndex(2, Path);
	InsStmt.SetBindingValueByIndex(3, ModuleType);
	InsStmt.SetBindingValueByIndex(4, BuildCsPath);
	InsStmt.Step();

	int64 RowId = Database->GetLastInsertRowId();
	if (RowId != 0)
	{
		return RowId;
	}

	// Already existed — fetch its id
	FSQLitePreparedStatement SelStmt;
	SelStmt.Create(*Database, TEXT("SELECT id FROM modules WHERE name = ? AND path = ?;"));
	SelStmt.SetBindingValueByIndex(1, Name);
	SelStmt.SetBindingValueByIndex(2, Path);
	if (SelStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 ExistingId = 0;
		SelStmt.GetColumnValueByIndex(0, ExistingId);
		return ExistingId;
	}

	UE_LOG(LogMonolithSource, Warning, TEXT("InsertModule: could not retrieve id for '%s'"), *Name);
	return 0;
}

int64 FMonolithSourceDatabase::InsertFile(const FString& FilePath, int64 ModuleId, const FString& FileType, int32 LineCount, double LastModified)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement InsStmt;
	InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO files (path, module_id, file_type, line_count, last_modified) VALUES (?, ?, ?, ?, ?);"));
	InsStmt.SetBindingValueByIndex(1, FilePath);
	InsStmt.SetBindingValueByIndex(2, ModuleId);
	InsStmt.SetBindingValueByIndex(3, FileType);
	InsStmt.SetBindingValueByIndex(4, static_cast<int64>(LineCount));
	InsStmt.SetBindingValueByIndex(5, LastModified);
	InsStmt.Step();

	int64 RowId = Database->GetLastInsertRowId();
	if (RowId != 0)
	{
		return RowId;
	}

	// Already existed — fetch its id
	FSQLitePreparedStatement SelStmt;
	SelStmt.Create(*Database, TEXT("SELECT id FROM files WHERE path = ?;"));
	SelStmt.SetBindingValueByIndex(1, FilePath);
	if (SelStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 ExistingId = 0;
		SelStmt.GetColumnValueByIndex(0, ExistingId);
		return ExistingId;
	}

	UE_LOG(LogMonolithSource, Warning, TEXT("InsertFile: could not retrieve id for '%s'"), *FilePath);
	return 0;
}

int64 FMonolithSourceDatabase::InsertSymbol(
	const FString& Name, const FString& QualifiedName, const FString& Kind,
	int64 FileId, int32 LineStart, int32 LineEnd,
	int64 ParentSymbolId,
	const FString& Access, const FString& Signature, const FString& Docstring,
	bool bIsUEMacro)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO symbols (name, qualified_name, kind, file_id, line_start, line_end, parent_symbol_id, access, signature, docstring, is_ue_macro) ")
		TEXT("VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);"));

	Stmt.SetBindingValueByIndex(1, Name);
	Stmt.SetBindingValueByIndex(2, QualifiedName);
	Stmt.SetBindingValueByIndex(3, Kind);

	// file_id — bind NULL if 0
	if (FileId != 0)
	{
		Stmt.SetBindingValueByIndex(4, FileId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(5, static_cast<int64>(LineStart));
	Stmt.SetBindingValueByIndex(6, static_cast<int64>(LineEnd));

	// parent_symbol_id — bind NULL if 0
	if (ParentSymbolId != 0)
	{
		Stmt.SetBindingValueByIndex(7, ParentSymbolId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(8, Access);
	Stmt.SetBindingValueByIndex(9, Signature);
	Stmt.SetBindingValueByIndex(10, Docstring);
	Stmt.SetBindingValueByIndex(11, static_cast<int64>(bIsUEMacro ? 1 : 0));

	Stmt.Step();

	return Database->GetLastInsertRowId();
}

void FMonolithSourceDatabase::InsertInheritance(int64 ChildId, int64 ParentId)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	// OR IGNORE — silent on unique constraint violation, mirrors Python IntegrityError catch
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR IGNORE INTO inheritance (child_id, parent_id) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, ChildId);
	Stmt.SetBindingValueByIndex(2, ParentId);
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertReference(int64 FromSymbolId, int64 ToSymbolId, const FString& RefKind, int64 FileId, int32 Line)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO \"references\" (from_symbol_id, to_symbol_id, ref_kind, file_id, line) ")
		TEXT("VALUES (?, ?, ?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, FromSymbolId);
	Stmt.SetBindingValueByIndex(2, ToSymbolId);
	Stmt.SetBindingValueByIndex(3, RefKind);

	if (FileId != 0)
	{
		Stmt.SetBindingValueByIndex(4, FileId);
	}

	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Line));
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertInclude(int64 FileId, const FString& IncludedPath, int32 Line)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO includes (file_id, included_path, line) VALUES (?, ?, ?);"));
	Stmt.SetBindingValueByIndex(1, FileId);
	Stmt.SetBindingValueByIndex(2, IncludedPath);
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Line));
	Stmt.Step();
}

void FMonolithSourceDatabase::InsertSourceChunks(int64 FileId, const TArray<FString>& Lines)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;
	if (Lines.Num() == 0) return;

	// Batch lines in groups of 10, matching Python _insert_source_lines()
	// Chunk's line_number is the 1-based index of the first line in that batch.
	static const int32 ChunkSize = 10;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT INTO source_fts (file_id, line_number, text) VALUES (?, ?, ?);"));

	for (int32 BatchStart = 0; BatchStart < Lines.Num(); BatchStart += ChunkSize)
	{
		const int32 BatchEnd = FMath::Min(BatchStart + ChunkSize, Lines.Num());

		FString JoinedText;
		int32 TotalLen = 0;
		for (int32 i = BatchStart; i < BatchEnd; ++i)
		{
			TotalLen += Lines[i].Len() + 1;
		}
		JoinedText.Reserve(TotalLen);

		for (int32 i = BatchStart; i < BatchEnd; ++i)
		{
			if (i > BatchStart)
			{
				JoinedText += TEXT("\n");
			}
			JoinedText += Lines[i];
		}

		// 1-based line number of the first line in this batch
		const int64 ChunkLineNumber = static_cast<int64>(BatchStart + 1);

		Stmt.Reset();
		Stmt.SetBindingValueByIndex(1, FileId);
		Stmt.SetBindingValueByIndex(2, ChunkLineNumber);
		Stmt.SetBindingValueByIndex(3, JoinedText);
		Stmt.Step();
	}
}

// ============================================================
// Meta key/value
// ============================================================

void FMonolithSourceDatabase::SetMeta(const FString& Key, const FString& Value)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);"));
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	Stmt.Step();
}

FString FMonolithSourceDatabase::GetMeta(const FString& Key)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return TEXT("");

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT value FROM meta WHERE key = ?;"));
	Stmt.SetBindingValueByIndex(1, Key);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Value;
		Stmt.GetColumnValueByIndex(0, Value);
		return Value;
	}
	return TEXT("");
}

// ============================================================
// Incremental indexing support
// ============================================================

int32 FMonolithSourceDatabase::LoadExistingSymbols(
	TMap<FString, int64>& OutSymbolNameToId,
	TMap<FString, int64>& OutClassNameToId,
	TMap<FString, TPair<int32,int32>>& OutSymbolSpans,
	TMap<FString, TPair<int32,int32>>& OutClassSpans)
{
	FScopeLock Lock(&DbLock);
	OutSymbolNameToId.Empty();
	OutClassNameToId.Empty();
	OutSymbolSpans.Empty();
	OutClassSpans.Empty();

	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("SELECT id, name, qualified_name, kind, line_start, line_end FROM symbols;"));

	int32 Count = 0;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 Id = 0;
		FString Name, QualifiedName, Kind;
		int32 LineStart = 0, LineEnd = 0;

		Stmt.GetColumnValueByIndex(0, Id);
		Stmt.GetColumnValueByIndex(1, Name);
		Stmt.GetColumnValueByIndex(2, QualifiedName);
		Stmt.GetColumnValueByIndex(3, Kind);
		Stmt.GetColumnValueByIndex(4, LineStart);
		Stmt.GetColumnValueByIndex(5, LineEnd);

		// Populate name->id maps (name and qualified_name both point to same id)
		OutSymbolNameToId.Add(Name, Id);
		if (QualifiedName != Name && !QualifiedName.IsEmpty())
		{
			OutSymbolNameToId.Add(QualifiedName, Id);
		}

		// Span tracking — prefer definitions (line_end > line_start) over forward decls
		const bool bIsDefinition = (LineEnd > LineStart);
		const TPair<int32,int32> NewSpan(LineStart, LineEnd);

		if (!OutSymbolSpans.Contains(Name))
		{
			OutSymbolSpans.Add(Name, NewSpan);
		}
		else if (bIsDefinition && OutSymbolSpans[Name].Value <= OutSymbolSpans[Name].Key)
		{
			// Overwrite forward decl (line_end <= line_start) with definition
			OutSymbolSpans[Name] = NewSpan;
		}

		// Class/struct maps
		const bool bIsClassOrStruct = (Kind == TEXT("class") || Kind == TEXT("struct"));
		if (bIsClassOrStruct)
		{
			OutClassNameToId.Add(Name, Id);
			if (QualifiedName != Name && !QualifiedName.IsEmpty())
			{
				OutClassNameToId.Add(QualifiedName, Id);
			}

			if (!OutClassSpans.Contains(Name))
			{
				OutClassSpans.Add(Name, NewSpan);
			}
			else if (bIsDefinition && OutClassSpans[Name].Value <= OutClassSpans[Name].Key)
			{
				// Overwrite forward decl with definition
				OutClassSpans[Name] = NewSpan;
			}
		}

		++Count;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("LoadExistingSymbols: loaded %d symbols (%d classes/structs)"),
		Count, OutClassNameToId.Num());

	return Count;
}
