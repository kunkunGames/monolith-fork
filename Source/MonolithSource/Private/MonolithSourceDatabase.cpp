#include "MonolithSourceDatabase.h"
#include "MonolithSourceSchema.h"
#include "MonolithFuzzyMatch.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <initializer_list>

DEFINE_LOG_CATEGORY(LogMonolithSource);

static FString MakeAutoSnapshotLabel(const TCHAR* Prefix)
{
	return FString::Printf(TEXT("%s-%lld"), Prefix, FDateTime::UtcNow().GetTicks());
}

static FString CollapseWhitespace(FString Value)
{
	FString Out;
	bool bPendingSpace = false;
	for (int32 Index = 0; Index < Value.Len(); ++Index)
	{
		const TCHAR Ch = Value[Index];
		if (FChar::IsWhitespace(Ch))
		{
			if (!Out.IsEmpty())
			{
				bPendingSpace = true;
			}
			continue;
		}
		if (bPendingSpace)
		{
			Out += TEXT(" ");
			bPendingSpace = false;
		}
		Out += Ch;
	}
	return Out.TrimStartAndEnd();
}

static FString MakeDuplicatedQualifiedLookupName(const FString& Name)
{
	const int32 LastScope = Name.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastScope == INDEX_NONE)
	{
		return Name;
	}

	const FString Owner = Name.Left(LastScope);
	return Owner.IsEmpty() ? Name : Owner + TEXT("::") + Name;
}

static FString MakeShortOwnerDuplicatedQualifiedLookupName(const FString& Name)
{
	const int32 LastScope = Name.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastScope == INDEX_NONE)
	{
		return Name;
	}

	const FString Owner = Name.Left(LastScope);
	const FString Method = Name.Mid(LastScope + 2);
	const int32 OwnerScope = Owner.Find(TEXT("::"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	const FString ShortOwner = OwnerScope == INDEX_NONE ? Owner : Owner.Mid(OwnerScope + 2);
	return Owner.IsEmpty() || ShortOwner.IsEmpty() ? Name : Owner + TEXT("::") + ShortOwner + TEXT("::") + Method;
}

static int32 FindMatchingParen(const FString& Text, int32 OpenIndex)
{
	int32 Depth = 0;
	for (int32 Index = OpenIndex; Index < Text.Len(); ++Index)
	{
		if (Text[Index] == TEXT('('))
		{
			++Depth;
		}
		else if (Text[Index] == TEXT(')'))
		{
			--Depth;
			if (Depth == 0)
			{
				return Index;
			}
		}
	}
	return INDEX_NONE;
}

static bool ExtractParamsAtParen(const FString& Signature, int32 OpenIndex, FString& OutParams)
{
	const int32 CloseIndex = FindMatchingParen(Signature, OpenIndex);
	if (CloseIndex == INDEX_NONE || CloseIndex <= OpenIndex)
	{
		return false;
	}
	OutParams = Signature.Mid(OpenIndex + 1, CloseIndex - OpenIndex - 1);
	return true;
}

static bool ExtractSignatureParams(const FString& Signature, const FString& FunctionName, FString& OutParams)
{
	if (!FunctionName.IsEmpty())
	{
		int32 SearchFrom = 0;
		while (SearchFrom < Signature.Len())
		{
			const int32 NameIndex = Signature.Find(FunctionName, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (NameIndex == INDEX_NONE)
			{
				break;
			}
			const int32 Before = NameIndex - 1;
			const bool bLeftBoundary = Before < 0 || !(FChar::IsAlnum(Signature[Before]) || Signature[Before] == TEXT('_'));
			int32 OpenIndex = NameIndex + FunctionName.Len();
			while (OpenIndex < Signature.Len() && FChar::IsWhitespace(Signature[OpenIndex]))
			{
				++OpenIndex;
			}
			if (bLeftBoundary && OpenIndex < Signature.Len() && Signature[OpenIndex] == TEXT('(')
				&& ExtractParamsAtParen(Signature, OpenIndex, OutParams))
			{
				return true;
			}
			SearchFrom = NameIndex + FMath::Max(1, FunctionName.Len());
		}
	}

	for (int32 Index = 0; Index < Signature.Len(); ++Index)
	{
		if (Signature[Index] == TEXT('(') && ExtractParamsAtParen(Signature, Index, OutParams))
		{
			return true;
		}
	}
	return false;
}

static TArray<FString> SplitTopLevelParams(const FString& Params)
{
	TArray<FString> Out;
	Out.Reserve(4);
	FString Current;
	Current.Reserve(Params.Len());
	int32 AngleDepth = 0;
	int32 ParenDepth = 0;
	int32 BracketDepth = 0;
	for (int32 Index = 0; Index < Params.Len(); ++Index)
	{
		const TCHAR Ch = Params[Index];
		if (Ch == TEXT('<')) ++AngleDepth;
		else if (Ch == TEXT('>') && AngleDepth > 0) --AngleDepth;
		else if (Ch == TEXT('(')) ++ParenDepth;
		else if (Ch == TEXT(')') && ParenDepth > 0) --ParenDepth;
		else if (Ch == TEXT('[')) ++BracketDepth;
		else if (Ch == TEXT(']') && BracketDepth > 0) --BracketDepth;

		if (Ch == TEXT(',') && AngleDepth == 0 && ParenDepth == 0 && BracketDepth == 0)
		{
			Out.Add(Current.TrimStartAndEnd());
			Current.Reset();
			continue;
		}
		Current += Ch;
	}
	const FString Tail = Current.TrimStartAndEnd();
	if (!Tail.IsEmpty())
	{
		Out.Add(Tail);
	}
	return Out;
}

static FString StripDefaultParamValue(const FString& Param)
{
	int32 AngleDepth = 0;
	int32 ParenDepth = 0;
	int32 BracketDepth = 0;
	for (int32 Index = 0; Index < Param.Len(); ++Index)
	{
		const TCHAR Ch = Param[Index];
		if (Ch == TEXT('<')) ++AngleDepth;
		else if (Ch == TEXT('>') && AngleDepth > 0) --AngleDepth;
		else if (Ch == TEXT('(')) ++ParenDepth;
		else if (Ch == TEXT(')') && ParenDepth > 0) --ParenDepth;
		else if (Ch == TEXT('[')) ++BracketDepth;
		else if (Ch == TEXT(']') && BracketDepth > 0) --BracketDepth;
		else if (Ch == TEXT('=') && AngleDepth == 0 && ParenDepth == 0 && BracketDepth == 0)
		{
			return Param.Left(Index).TrimStartAndEnd();
		}
	}
	return Param.TrimStartAndEnd();
}

static bool IsIdentifierChar(TCHAR Ch)
{
	return FChar::IsAlnum(Ch) || Ch == TEXT('_');
}

static bool IsTrailingTypeQualifier(const FString& Word)
{
	const FString Lower = Word.ToLower();
	return Lower == TEXT("const")
		|| Lower == TEXT("volatile")
		|| Lower == TEXT("mutable")
		|| Lower == TEXT("final")
		|| Lower == TEXT("override");
}

static FString StripTrailingParamName(FString Param)
{
	Param = Param.TrimStartAndEnd();
	if (Param.EndsWith(TEXT("...")))
	{
		return Param;
	}

	int32 End = Param.Len() - 1;
	while (End >= 0 && FChar::IsWhitespace(Param[End])) --End;
	if (End < 0 || !IsIdentifierChar(Param[End]))
	{
		return Param;
	}

	int32 Start = End;
	while (Start >= 0 && IsIdentifierChar(Param[Start])) --Start;
	const FString Word = Param.Mid(Start + 1, End - Start);
	if (IsTrailingTypeQualifier(Word))
	{
		return Param;
	}

	const FString Prefix = Param.Left(Start + 1).TrimEnd();
	if (Prefix.IsEmpty())
	{
		return Param;
	}
	return Prefix;
}

static FString StripLeadingElaboratedTypeKeyword(FString Param)
{
	Param = Param.TrimStartAndEnd();
	for (const TCHAR* Keyword : { TEXT("enum "), TEXT("class "), TEXT("struct ") })
	{
		if (Param.StartsWith(Keyword, ESearchCase::IgnoreCase))
		{
			return Param.Mid(FCString::Strlen(Keyword)).TrimStartAndEnd();
		}
	}
	return Param;
}

static FString NormalizeOverrideParam(FString Param)
{
	Param = CollapseWhitespace(StripLeadingElaboratedTypeKeyword(StripTrailingParamName(StripDefaultParamValue(Param))));
	Param.ReplaceInline(TEXT(" &"), TEXT("&"));
	Param.ReplaceInline(TEXT(" *"), TEXT("*"));
	Param.ReplaceInline(TEXT(" &&"), TEXT("&&"));
	Param.ReplaceInline(TEXT(" ,"), TEXT(","));
	Param.ReplaceInline(TEXT(", "), TEXT(","));
	return Param.TrimStartAndEnd();
}

static bool NormalizeOverrideParams(const FString& Signature, const FString& FunctionName, FString& OutNormalized)
{
	FString Params;
	if (!ExtractSignatureParams(Signature, FunctionName, Params))
	{
		return false;
	}
	TArray<FString> Parts = SplitTopLevelParams(Params);
	if (Parts.Num() == 1)
	{
		const FString Only = Parts[0].TrimStartAndEnd().ToLower();
		if (Only == TEXT("void"))
		{
			Parts.Empty();
		}
	}
	for (FString& Part : Parts)
	{
		Part = NormalizeOverrideParam(Part);
	}
	OutNormalized = FString::Join(Parts, TEXT(","));
	return true;
}

static bool AreOverrideSignaturesCompatible(
	const FString& ChildSignature,
	const FString& ParentSignature,
	const FString& ChildName,
	const FString& ParentName,
	FString& OutReason)
{
	if (ChildSignature.IsEmpty() || ParentSignature.IsEmpty())
	{
		OutReason = TEXT("signature parameters unavailable but assuming compatible");
		return true;
	}
	FString ChildParams;
	FString ParentParams;
	if (!NormalizeOverrideParams(ChildSignature, ChildName, ChildParams)
		|| !NormalizeOverrideParams(ParentSignature, ParentName, ParentParams))
	{
		OutReason = TEXT("signature parameters unavailable but assuming compatible");
		return true;
	}
	if (ChildParams != ParentParams)
	{
		OutReason = TEXT("same name but different normalized parameter list");
		return false;
	}
	OutReason = ChildParams.IsEmpty()
		? TEXT("same name and zero parameters")
		: TEXT("same name and matching normalized parameter list");
	return true;
}

static FString OverrideConfidenceForReason(const FString& Reason)
{
	return Reason.Contains(TEXT("assuming compatible")) ? TEXT("medium") : TEXT("high");
}

static void ReadOverrideEdges(FSQLitePreparedStatement& Stmt, TArray<FMonolithSourceOverrideEdge>& Result, int32 MaxResults)
{
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceOverrideEdge Edge;
		FString ChildSignature;
		FString ParentSignature;
		Stmt.GetColumnValueByIndex(0, Edge.FromSymbolId);
		Stmt.GetColumnValueByIndex(1, Edge.ToSymbolId);
		Stmt.GetColumnValueByIndex(2, Edge.FromName);
		Stmt.GetColumnValueByIndex(3, Edge.FromQualifiedName);
		Stmt.GetColumnValueByIndex(4, Edge.ToName);
		Stmt.GetColumnValueByIndex(5, Edge.ToQualifiedName);
		Stmt.GetColumnValueByIndex(6, Edge.ChildClassName);
		Stmt.GetColumnValueByIndex(7, Edge.ChildClassQualifiedName);
		Stmt.GetColumnValueByIndex(8, Edge.ParentClassName);
		Stmt.GetColumnValueByIndex(9, Edge.ParentClassQualifiedName);
		Stmt.GetColumnValueByIndex(10, ChildSignature);
		Stmt.GetColumnValueByIndex(11, ParentSignature);

		FString Reason;
		if (!AreOverrideSignaturesCompatible(ChildSignature, ParentSignature, Edge.FromName, Edge.ToName, Reason))
		{
			continue;
		}
		Edge.Confidence = OverrideConfidenceForReason(Reason);
		Edge.Reason = Reason;
		Result.Add(MoveTemp(Edge));
		if (Result.Num() >= MaxResults)
		{
			break;
		}
	}
}

static bool ReadCachedOverrideEdges(FSQLitePreparedStatement& Stmt, TArray<FMonolithSourceOverrideEdge>& Result)
{
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceOverrideEdge Edge;
		Stmt.GetColumnValueByIndex(0, Edge.FromSymbolId);
		Stmt.GetColumnValueByIndex(1, Edge.ToSymbolId);
		Stmt.GetColumnValueByIndex(2, Edge.FromName);
		Stmt.GetColumnValueByIndex(3, Edge.FromQualifiedName);
		Stmt.GetColumnValueByIndex(4, Edge.ToName);
		Stmt.GetColumnValueByIndex(5, Edge.ToQualifiedName);
		Stmt.GetColumnValueByIndex(6, Edge.ChildClassName);
		Stmt.GetColumnValueByIndex(7, Edge.ChildClassQualifiedName);
		Stmt.GetColumnValueByIndex(8, Edge.ParentClassName);
		Stmt.GetColumnValueByIndex(9, Edge.ParentClassQualifiedName);
		Stmt.GetColumnValueByIndex(10, Edge.Confidence);
		Stmt.GetColumnValueByIndex(11, Edge.Reason);
		Result.Add(MoveTemp(Edge));
	}
	return true;
}

static int32 CountOverrideEdgesToUnlocked(FSQLiteDatabase& DB, int64 SymbolId, int32 Limit)
{
	const int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);
	const int32 ProbeLimit = FMath::Clamp(SafeLimit * 4, SafeLimit, 1000);
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(DB, TEXT(
		"WITH RECURSIVE descendants(base_class_id, child_class_id) AS ("
		"  SELECT parent_id, child_id FROM inheritance"
		"  UNION "
		"  SELECT d.base_class_id, i.child_id "
		"  FROM descendants d "
		"  JOIN inheritance AS i ON i.parent_id = d.child_class_id"
		") "
		"SELECT child_fn.id,base_fn.id,"
		"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
		"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
		"       COALESCE(child_fn.signature,''),COALESCE(base_fn.signature,'') "
		"FROM symbols base_fn "
		"JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id "
		"JOIN descendants AS d ON d.base_class_id = base_cls.id "
		"JOIN symbols child_cls ON child_cls.id = d.child_class_id "
		"JOIN symbols child_fn ON child_fn.parent_symbol_id = child_cls.id "
		"    AND (base_fn.name = child_fn.name "
		"      OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"    AND child_fn.kind = base_fn.kind "
		"    AND child_fn.id != base_fn.id "
		"WHERE base_fn.id = ? AND base_fn.kind = 'function' "
		"ORDER BY child_fn.qualified_name "
		"LIMIT ?;")))
	{
		return 0;
	}
	Stmt.SetBindingValueByIndex(1, SymbolId);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(ProbeLimit));
	TArray<FMonolithSourceOverrideEdge> Edges;
	ReadOverrideEdges(Stmt, Edges, SafeLimit);
	return Edges.Num();
}

static int32 CountOverrideEdgesFromUnlocked(FSQLiteDatabase& DB, int64 SymbolId, int32 Limit)
{
	const int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);
	const int32 ProbeLimit = FMath::Clamp(SafeLimit * 4, SafeLimit, 1000);
	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(DB, TEXT(
		"WITH RECURSIVE ancestors(child_class_id, ancestor_class_id) AS ("
		"  SELECT child_id, parent_id FROM inheritance"
		"  UNION "
		"  SELECT a.child_class_id, i.parent_id "
		"  FROM ancestors a "
		"  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id"
		") "
		"SELECT child_fn.id,base_fn.id,"
		"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
		"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
		"       COALESCE(child_fn.signature,''),COALESCE(base_fn.signature,'') "
		"FROM symbols child_fn "
		"JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
		"JOIN ancestors AS a ON a.child_class_id = child_cls.id "
		"JOIN symbols base_cls ON base_cls.id = a.ancestor_class_id "
		"JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id = base_cls.id "
		"    AND (base_fn.name = child_fn.name "
		"      OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"    AND base_fn.kind = child_fn.kind "
		"WHERE child_fn.id = ? AND child_fn.kind = 'function' "
		"ORDER BY base_fn.qualified_name "
		"LIMIT ?;")))
	{
		return 0;
	}
	Stmt.SetBindingValueByIndex(1, SymbolId);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(ProbeLimit));
	TArray<FMonolithSourceOverrideEdge> Edges;
	ReadOverrideEdges(Stmt, Edges, SafeLimit);
	return Edges.Num();
}

static bool PopulateSourceOverrideEdgeCacheLocked(FSQLiteDatabase& DB, const TCHAR* ChildSeedTable, int64& OutCount, FString& OutError)
{
	OutCount = 0;
	const FString ChildSeedFilter = ChildSeedTable && ChildSeedTable[0] != TEXT('\0')
		? FString::Printf(TEXT(" AND id IN (SELECT id FROM %s)"), ChildSeedTable)
		: FString();
	const FString CandidateSql = FString::Printf(TEXT(
		"WITH RECURSIVE child_seed(id,name,qualified_name,kind,parent_symbol_id,signature) AS ("
		"  SELECT id,name,qualified_name,kind,parent_symbol_id,signature FROM symbols "
		"  WHERE kind = 'function' AND signature LIKE '%%override%%'%s"
		"), ancestors(child_class_id, ancestor_class_id) AS ("
		"  SELECT DISTINCT child_cls.id, i.parent_id "
		"  FROM child_seed child_fn "
		"  JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
		"  JOIN inheritance i ON i.child_id = child_cls.id "
		"  UNION "
		"  SELECT a.child_class_id, i.parent_id "
		"  FROM ancestors a "
		"  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id"
		") "
		"SELECT child_fn.id,base_fn.id,"
		"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
		"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
		"       COALESCE(child_fn.signature,''),COALESCE(base_fn.signature,'') "
		"FROM child_seed AS child_fn "
		"JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
		"JOIN ancestors AS a ON a.child_class_id = child_cls.id "
		"JOIN symbols base_cls ON base_cls.id = a.ancestor_class_id "
		"JOIN symbols AS base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id = base_cls.id "
		"    AND (base_fn.name = child_fn.name "
		"      OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"    AND base_fn.kind = child_fn.kind "
		"WHERE child_fn.kind = 'function' "
		"ORDER BY base_fn.id, child_fn.id;"), *ChildSeedFilter);
	FSQLitePreparedStatement Candidates;
	if (!Candidates.Create(DB, *CandidateSql))
	{
		OutError = DB.GetLastError();
		return false;
	}

	FSQLitePreparedStatement Insert;
	if (!Insert.Create(DB, TEXT(
		"INSERT OR IGNORE INTO source_override_edges "
		"(child_symbol_id,parent_symbol_id,confidence,reason,updated_at) "
		"VALUES (?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER));")))
	{
		OutError = DB.GetLastError();
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	while (Candidates.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithSourceOverrideEdge Edge;
		FString ChildSignature;
		FString ParentSignature;
		Candidates.GetColumnValueByIndex(0, Edge.FromSymbolId);
		Candidates.GetColumnValueByIndex(1, Edge.ToSymbolId);
		Candidates.GetColumnValueByIndex(2, Edge.FromName);
		Candidates.GetColumnValueByIndex(3, Edge.FromQualifiedName);
		Candidates.GetColumnValueByIndex(4, Edge.ToName);
		Candidates.GetColumnValueByIndex(5, Edge.ToQualifiedName);
		Candidates.GetColumnValueByIndex(6, Edge.ChildClassName);
		Candidates.GetColumnValueByIndex(7, Edge.ChildClassQualifiedName);
		Candidates.GetColumnValueByIndex(8, Edge.ParentClassName);
		Candidates.GetColumnValueByIndex(9, Edge.ParentClassQualifiedName);
		Candidates.GetColumnValueByIndex(10, ChildSignature);
		Candidates.GetColumnValueByIndex(11, ParentSignature);

		FString Reason;
		if (!AreOverrideSignaturesCompatible(ChildSignature, ParentSignature, Edge.FromName, Edge.ToName, Reason))
		{
			continue;
		}
		Edge.Confidence = OverrideConfidenceForReason(Reason);
		Edge.Reason = Reason;

		Insert.Reset();
		Insert.SetBindingValueByIndex(1, Edge.FromSymbolId);
		Insert.SetBindingValueByIndex(2, Edge.ToSymbolId);
		Insert.SetBindingValueByIndex(3, Edge.Confidence);
		Insert.SetBindingValueByIndex(4, Edge.Reason);
		const ESQLitePreparedStatementStepResult Step = Insert.Step();
		if (Step != ESQLitePreparedStatementStepResult::Done)
		{
			OutError = DB.GetLastError();
			return false;
		}
		++OutCount;
	}
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	if (ElapsedSeconds >= 1.0)
	{
		UE_LOG(LogMonolithSource, Log,
			TEXT("PopulateSourceOverrideEdgeCache: child_seed=%s inserted=%lld elapsed=%.3fs"),
			ChildSeedTable && ChildSeedTable[0] != TEXT('\0') ? ChildSeedTable : TEXT("<all>"),
			OutCount,
			ElapsedSeconds);
	}
	return true;
}

static bool PopulateSourceOverrideEdgeCacheLocked(FSQLiteDatabase& DB, int64& OutCount, FString& OutError)
{
	return PopulateSourceOverrideEdgeCacheLocked(DB, nullptr, OutCount, OutError);
}

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
		Current.Reset();
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

static FString ReadPragmaString(FSQLiteDatabase& DB, const TCHAR* Name)
{
	FString Value;
	const FString SQL = FString::Printf(TEXT("PRAGMA %s;"), Name);
	FSQLitePreparedStatement S;
	if (S.Create(DB, *SQL) && S.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		S.GetColumnValueByIndex(0, Value);
	}
	return Value;
}

static bool ApplyEngineSourceDeletePragmas(FSQLiteDatabase& DB, const FString& DbPath)
{
	bool bOk = true;
	bOk = DB.Execute(TEXT("PRAGMA busy_timeout=5000;")) && bOk;
	bOk = DB.Execute(TEXT("PRAGMA locking_mode=NORMAL;")) && bOk;
	bOk = DB.Execute(TEXT("PRAGMA journal_mode=DELETE;")) && bOk;
	bOk = DB.Execute(TEXT("PRAGMA synchronous=NORMAL;")) && bOk;

	const FString Journal = ReadPragmaString(DB, TEXT("journal_mode"));
	if (!Journal.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogMonolithSource, Error,
			TEXT("EngineSource DB did not enter DELETE journal mode: path=%s journal_mode=%s"),
			*DbPath,
			Journal.IsEmpty() ? TEXT("<unknown>") : *Journal);
		return false;
	}
	return bOk;
}

static bool DeleteSourceDatabaseFileIfPresent(IPlatformFile& PlatformFile, const FString& Path, bool bRequired)
{
	if (Path.IsEmpty() || !PlatformFile.FileExists(*Path))
	{
		return true;
	}
	if (PlatformFile.DeleteFile(*Path))
	{
		return true;
	}
	if (bRequired)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to delete EngineSource DB file before reset: %s"), *Path);
	}
	else
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("Failed to delete EngineSource DB sidecar before reset: %s"), *Path);
	}
	return !bRequired;
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

	if (!ApplyEngineSourceDeletePragmas(*Database, DbPath))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("Failed to configure EngineSource DB journal mode: %s"), *DbPath);
		Database->Close();
		delete Database;
		Database = nullptr;
		return false;
	}

	UE_LOG(LogMonolithSource, Log, TEXT("Engine source DB opened: %s"), *DbPath);
	return true;
}

static void AddNextActions(const TSharedPtr<FJsonObject>& Root, std::initializer_list<const TCHAR*> Actions)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Actions.size());
	for (const TCHAR* Action : Actions)
	{
		Arr.Add(MakeShared<FJsonValueString>(FString(Action)));
	}
	Root->SetArrayField(TEXT("next_actions"), Arr);
}

static void AddNextActions(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Actions)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Actions.Num());
	for (const FString& Action : Actions)
	{
		Arr.Add(MakeShared<FJsonValueString>(Action));
	}
	Root->SetArrayField(TEXT("next_actions"), Arr);
}

static bool ParseJsonArray(const FString& Json, TArray<TSharedPtr<FJsonValue>>& Out)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Out);
}

static TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
{
	TSharedPtr<FJsonObject> Out;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Out) ? Out : nullptr;
}

static TSharedPtr<FJsonObject> CacheMeta(const FString& Status, const FString& CacheVersion, const FString& ScoringVersion)
{
	TSharedPtr<FJsonObject> Cache = MakeShared<FJsonObject>();
	Cache->SetStringField(TEXT("status"), Status);
	if (!CacheVersion.IsEmpty())
	{
		Cache->SetStringField(TEXT("version"), CacheVersion);
		Cache->SetStringField(TEXT("cache_version"), CacheVersion);
	}
	if (!ScoringVersion.IsEmpty()) Cache->SetStringField(TEXT("scoring_version"), ScoringVersion);
	return Cache;
}

static int32 ConfidenceRank(const FString& Confidence)
{
	if (Confidence == TEXT("high")) return 2;
	if (Confidence == TEXT("medium")) return 1;
	return 0;
}

static FString TierForScore(double Score)
{
	if (Score >= 0.66) return TEXT("high");
	if (Score >= 0.33) return TEXT("medium");
	return TEXT("low");
}

static bool AllowsSensitivityPrefix(const FString& Token)
{
	return Token == TEXT("save")
		|| Token == TEXT("serialize")
		|| Token == TEXT("archive")
		|| Token == TEXT("auth")
		|| Token == TEXT("login")
		|| Token == TEXT("account")
		|| Token == TEXT("session")
		|| Token == TEXT("purchase")
		|| Token == TEXT("store")
		|| Token == TEXT("entitlement")
		|| Token == TEXT("anticheat")
		|| Token == TEXT("crypto")
		|| Token == TEXT("crypt")
		|| Token == TEXT("encrypt")
		|| Token == TEXT("decrypt")
		|| Token == TEXT("signature")
		|| Token == TEXT("signed")
		|| Token == TEXT("signing")
		|| Token == TEXT("hash")
		|| Token == TEXT("exec")
		|| Token == TEXT("eval")
		|| Token == TEXT("command")
		|| Token == TEXT("file")
		|| Token == TEXT("registry")
		|| Token == TEXT("process")
		|| Token == TEXT("ufunction")
		|| Token == TEXT("server")
		|| Token == TEXT("client")
		|| Token == TEXT("netmulticast")
		|| Token == TEXT("onrep")
		|| Token == TEXT("replication")
		|| Token == TEXT("rpc")
		|| Token == TEXT("network");
}

static bool MatchesSensitivityToken(const FString& CandidateToken, const FString& SensitiveToken)
{
	if (CandidateToken == SensitiveToken)
	{
		return true;
	}
	return AllowsSensitivityPrefix(SensitiveToken) && CandidateToken.StartsWith(SensitiveToken);
}

static void AddSensitivityCandidateToken(TArray<FString>& Tokens, TSet<FString>& Seen, FString Token)
{
	Token.ToLowerInline();
	if (Token.IsEmpty() || Seen.Contains(Token))
	{
		return;
	}
	Seen.Add(Token);
	Tokens.Add(MoveTemp(Token));
}

static TArray<FString> BuildSensitivityCandidateTokens(const FString& Text)
{
	TArray<FString> Tokens;
	TSet<FString> Seen;
	for (const FString& Token : FMonolithFuzzyMatch::Tokenize(Text))
	{
		AddSensitivityCandidateToken(Tokens, Seen, Token);
	}

	FString Current;
	TCHAR Previous = TEXT('\0');
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Ch = Text[Index];
		if (!FChar::IsAlnum(Ch))
		{
			AddSensitivityCandidateToken(Tokens, Seen, Current);
			Current.Empty();
			Previous = TEXT('\0');
			continue;
		}
		if (!Current.IsEmpty()
			&& FChar::IsUpper(Ch)
			&& (FChar::IsLower(Previous) || FChar::IsDigit(Previous)))
		{
			AddSensitivityCandidateToken(Tokens, Seen, Current);
			Current.Empty();
		}
		Current.AppendChar(Ch);
		Previous = Ch;
	}
	AddSensitivityCandidateToken(Tokens, Seen, Current);
	return Tokens;
}

static bool ContainsAnyToken(const FString& Text, std::initializer_list<const TCHAR*> Tokens, FString* OutMatchedToken = nullptr)
{
	const TArray<FString> CandidateTokens = BuildSensitivityCandidateTokens(Text);
	for (const FString& CandidateToken : CandidateTokens)
	{
		if (CandidateToken.IsEmpty())
		{
			continue;
		}
		for (const TCHAR* Token : Tokens)
		{
			const FString SensitiveToken(Token);
			if (MatchesSensitivityToken(CandidateToken, SensitiveToken))
			{
				if (OutMatchedToken)
				{
					*OutMatchedToken = CandidateToken;
				}
				return true;
			}
		}
	}
	return false;
}

static double SourceSensitivityFactor(const FString& Text, FString& OutReason)
{
	auto MatchedReason = [](const TCHAR* Reason, const FString& Token)
	{
		return Token.IsEmpty()
			? FString(Reason)
			: FString::Printf(TEXT("%s (token=%s)"), Reason, *Token);
	};

	FString MatchedToken;
	if (ContainsAnyToken(Text, { TEXT("ufunction"), TEXT("server"), TEXT("client"), TEXT("netmulticast"), TEXT("onrep"), TEXT("replication"), TEXT("rpc"), TEXT("network") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: replication/RPC or network surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("save"), TEXT("serialize"), TEXT("archive") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: save/serialization surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("auth"), TEXT("login"), TEXT("account"), TEXT("session") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: auth/account/session surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("purchase"), TEXT("iap"), TEXT("store"), TEXT("entitlement") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: purchase/store entitlement surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("anticheat"), TEXT("anti_cheat"), TEXT("cheat") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: anticheat surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("crypt"), TEXT("crypto"), TEXT("encrypt"), TEXT("decrypt"), TEXT("sign"), TEXT("signature"), TEXT("signed"), TEXT("signing"), TEXT("hash") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: crypto/signing/hash surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("exec"), TEXT("eval"), TEXT("command") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: exec/eval/command surface"), MatchedToken);
		return 0.15;
	}
	if (ContainsAnyToken(Text, { TEXT("file"), TEXT("registry"), TEXT("process") }, &MatchedToken))
	{
		OutReason = MatchedReason(TEXT("sensitivity: file/registry/process surface"), MatchedToken);
		return 0.15;
	}
	return 0.0;
}

static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Arr.Add(MakeShared<FJsonValueString>(Value));
	}
	return Arr;
}

static FString NormalizeChangedPath(FString Path)
{
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	return Path;
}

static double JsonScore(const TSharedPtr<FJsonObject>& Object)
{
	double Score = 0.0;
	if (Object.IsValid())
	{
		Object->TryGetNumberField(TEXT("score"), Score);
	}
	return Score;
}

struct FSnapshotManifest
{
	TSet<FString> Nodes;
	TSet<FString> Edges;
};

struct FSnapshotRecord
{
	int64 Id = 0;
	FString Label;
	FSnapshotManifest Manifest;
};

static TArray<TSharedPtr<FJsonValue>> SetToJsonArray(const TSet<FString>& Values)
{
	TArray<FString> Sorted = Values.Array();
	Sorted.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(Sorted.Num());
	for (const FString& Value : Sorted)
	{
		Arr.Add(MakeShared<FJsonValueString>(Value));
	}
	return Arr;
}

static bool JsonArrayToSet(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TSet<FString>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(Field, Arr) || !Arr)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Arr)
	{
		FString S;
		if (Value.IsValid() && Value->TryGetString(S))
		{
			Out.Add(S);
		}
	}
	return true;
}

static FString SerializeManifest(const FSnapshotManifest& Manifest)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetArrayField(TEXT("nodes"), SetToJsonArray(Manifest.Nodes));
	Object->SetArrayField(TEXT("edges"), SetToJsonArray(Manifest.Edges));
	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Out;
}

static FString SnapshotEdgeKeyPart(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("|"), TEXT("\\|"));
	return Value;
}

static FString SnapshotEdgeKey(const FString& Source, const FString& Target, const FString& Kind, const FString& Subkind)
{
	return FString::Printf(TEXT("%s|%s|%s|%s"),
		*SnapshotEdgeKeyPart(Source),
		*SnapshotEdgeKeyPart(Target),
		*SnapshotEdgeKeyPart(Kind),
		*SnapshotEdgeKeyPart(Subkind));
}

static bool ParseManifest(const FString& Json, FSnapshotManifest& Out)
{
	TSharedPtr<FJsonObject> Object = ParseJsonObject(Json);
	if (!Object.IsValid())
	{
		return false;
	}
	return JsonArrayToSet(Object, TEXT("nodes"), Out.Nodes)
		&& JsonArrayToSet(Object, TEXT("edges"), Out.Edges);
}

static bool EnsureSnapshotTable(FSQLiteDatabase& DB)
{
	return DB.Execute(TEXT(
		"CREATE TABLE IF NOT EXISTS crg_snapshots ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT,"
		"label TEXT NOT NULL,"
		"domain TEXT NOT NULL,"
		"captured_at INTEGER NOT NULL,"
		"node_count INTEGER NOT NULL,"
		"edge_count INTEGER NOT NULL,"
		"manifest_json TEXT NOT NULL,"
		"UNIQUE(domain,label)"
		");"));
}

static bool LoadCurrentManifestLocked(FSQLiteDatabase& DB, const TCHAR* Domain, FSnapshotManifest& Out)
{
	FSQLitePreparedStatement NodeStmt;
	if (!NodeStmt.Create(DB, TEXT("SELECT stable_key FROM crg_nodes WHERE domain = ? ORDER BY stable_key;")))
	{
		return false;
	}
	NodeStmt.SetBindingValueByIndex(1, FString(Domain));
	while (NodeStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString StableKey;
		NodeStmt.GetColumnValueByIndex(0, StableKey);
		Out.Nodes.Add(StableKey);
	}

	FSQLitePreparedStatement EdgeStmt;
	if (!EdgeStmt.Create(DB, TEXT(
		"SELECT sn.stable_key,tn.stable_key,e.edge_kind,COALESCE(e.edge_subkind,'') "
		"FROM crg_edges e "
		"JOIN crg_nodes sn ON sn.id = e.source_node_id "
		"JOIN crg_nodes tn ON tn.id = e.target_node_id "
		"WHERE e.domain = ? ORDER BY sn.stable_key,tn.stable_key,e.edge_kind,e.edge_subkind;")))
	{
		return false;
	}
	EdgeStmt.SetBindingValueByIndex(1, FString(Domain));
	while (EdgeStmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString Source, Target, Kind, Subkind;
		EdgeStmt.GetColumnValueByIndex(0, Source);
		EdgeStmt.GetColumnValueByIndex(1, Target);
		EdgeStmt.GetColumnValueByIndex(2, Kind);
		EdgeStmt.GetColumnValueByIndex(3, Subkind);
		Out.Edges.Add(SnapshotEdgeKey(Source, Target, Kind, Subkind));
	}
	return true;
}

static bool LoadSnapshotRecordLocked(FSQLiteDatabase& DB, const TCHAR* Domain, const FString& Ref, FSnapshotRecord& Out)
{
	if (Ref.IsEmpty() || Ref.Equals(TEXT("current"), ESearchCase::IgnoreCase))
	{
		Out.Label = TEXT("current");
		return LoadCurrentManifestLocked(DB, Domain, Out.Manifest);
	}

	auto LoadFromStatement = [&Out](FSQLitePreparedStatement& Stmt) -> bool
	{
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return false;
		}
		FString ManifestJson;
		Stmt.GetColumnValueByIndex(0, Out.Id);
		Stmt.GetColumnValueByIndex(1, Out.Label);
		Stmt.GetColumnValueByIndex(2, ManifestJson);
		return ParseManifest(ManifestJson, Out.Manifest);
	};

	if (Ref.IsNumeric())
	{
		FSQLitePreparedStatement IdStmt;
		if (!IdStmt.Create(DB, TEXT("SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND id = ? LIMIT 1;")))
		{
			return false;
		}
		IdStmt.SetBindingValueByIndex(1, FString(Domain));
		IdStmt.SetBindingValueByIndex(2, static_cast<int64>(FCString::Atoi64(*Ref)));
		if (LoadFromStatement(IdStmt))
		{
			return true;
		}
	}

	FSQLitePreparedStatement LabelStmt;
	if (!LabelStmt.Create(DB, TEXT("SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND label = ? LIMIT 1;")))
	{
		return false;
	}
	LabelStmt.SetBindingValueByIndex(1, FString(Domain));
	LabelStmt.SetBindingValueByIndex(2, Ref);
	return LoadFromStatement(LabelStmt);
}

static TArray<TSharedPtr<FJsonValue>> TakeStringSamples(const TSet<FString>& Values, int32 Limit, bool& bTruncated)
{
	TArray<FString> Sorted = Values.Array();
	Sorted.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(FMath::Min(Sorted.Num(), Limit + 1));
	for (int32 Index = 0; Index < Sorted.Num(); ++Index)
	{
		if (Index >= Limit)
		{
			bTruncated = true;
			break;
		}
		Arr.Add(MakeShared<FJsonValueString>(Sorted[Index]));
	}
	return Arr;
}

static TSharedPtr<FJsonObject> EdgeObject(const FString& Key)
{
	TArray<FString> Parts;
	Key.ParseIntoArray(Parts, TEXT("|"), false);
	TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetStringField(TEXT("key"), Key);
	if (Parts.Num() >= 4)
	{
		Edge->SetStringField(TEXT("source"), Parts[0]);
		Edge->SetStringField(TEXT("target"), Parts[1]);
		Edge->SetStringField(TEXT("kind"), Parts[2]);
		Edge->SetStringField(TEXT("subkind"), Parts[3]);
	}
	return Edge;
}

static TArray<TSharedPtr<FJsonValue>> TakeEdgeSamples(const TSet<FString>& Values, int32 Limit, bool& bTruncated)
{
	TArray<FString> Sorted = Values.Array();
	Sorted.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(FMath::Min(Sorted.Num(), Limit + 1));
	for (int32 Index = 0; Index < Sorted.Num(); ++Index)
	{
		if (Index >= Limit)
		{
			bTruncated = true;
			break;
		}
		Arr.Add(MakeShared<FJsonValueObject>(EdgeObject(Sorted[Index])));
	}
	return Arr;
}

static TSet<FString> SetDifference(const TSet<FString>& Left, const TSet<FString>& Right)
{
	TSet<FString> Out;
	for (const FString& Value : Left)
	{
		if (!Right.Contains(Value))
		{
			Out.Add(Value);
		}
	}
	return Out;
}

static bool TableExistsLocked(FSQLiteDatabase& DB, const TCHAR* Name)
{
	FSQLitePreparedStatement S;
	if (!S.Create(DB, TEXT("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;")))
	{
		return false;
	}
	S.SetBindingValueByIndex(1, FString(Name));
	return S.Step() == ESQLitePreparedStatementStepResult::Row;
}

static bool CrgMetaEqualsLocked(FSQLiteDatabase& DB, const TCHAR* Key, const TCHAR* Expected)
{
	if (!TableExistsLocked(DB, TEXT("crg_meta")))
	{
		return false;
	}

	FSQLitePreparedStatement S;
	if (!S.Create(DB, TEXT("SELECT value FROM crg_meta WHERE key = ? LIMIT 1;")))
	{
		return false;
	}
	S.SetBindingValueByIndex(1, FString(Key));
	if (S.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return false;
	}
	FString Value;
	S.GetColumnValueByIndex(0, Value);
	return Value == FString(Expected);
}

static bool SourceOverrideEdgeCacheReadyLocked(FSQLiteDatabase& DB)
{
	return TableExistsLocked(DB, TEXT("source_override_edges"))
		&& CrgMetaEqualsLocked(DB, TEXT("source_override_edges_version"), TEXT("2"));
}

static int64 CountIdLocked(FSQLiteDatabase& DB, const TCHAR* Sql, int64 Id)
{
	FSQLitePreparedStatement S;
	if (!S.Create(DB, Sql))
	{
		return 0;
	}
	S.SetBindingValueByIndex(1, Id);
	int64 Count = 0;
	if (S.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		S.GetColumnValueByIndex(0, Count);
	}
	return Count;
}

struct FDetectSymbolRow
{
	int64 Id = 0;
	int64 FileId = 0;
	FString Name;
	FString QualifiedName;
	FString Kind;
	FString File;
	FString Signature;
	int32 LineStart = 0;
	int32 LineEnd = 0;
	bool bIsUEMacro = false;
};

static TSharedPtr<FJsonObject> CachedRiskForSymbolLocked(FSQLiteDatabase& DB, int64 SymbolId)
{
	if (!TableExistsLocked(DB, TEXT("crg_nodes"))
		|| !TableExistsLocked(DB, TEXT("crg_node_metrics"))
		|| !TableExistsLocked(DB, TEXT("crg_meta")))
	{
		return nullptr;
	}

	FSQLitePreparedStatement S;
	if (!S.Create(DB, TEXT(
		"SELECT s.name,s.qualified_name,s.kind,COALESCE(f.path,''),s.line_start,"
		"       m.risk_score,m.risk_tier,m.reasons_json,m.raw_counts_json,m.scoring_version,"
		"       COALESCE((SELECT value FROM crg_meta WHERE key = 'cache_version'), '1') "
		"FROM crg_nodes n "
		"JOIN crg_node_metrics m ON m.node_id = n.id "
		"JOIN symbols s ON s.id = n.native_id "
		"LEFT JOIN files f ON f.id = s.file_id "
		"WHERE n.domain = 'source' AND n.native_table = 'symbols' AND n.native_id = ? "
		"LIMIT 1;")))
	{
		return nullptr;
	}
	S.SetBindingValueByIndex(1, SymbolId);
	if (S.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return nullptr;
	}

	FString Name, QualifiedName, Kind, File, Tier, ReasonsJson, RawCountsJson, ScoringVersion, CacheVersion;
	int32 Line = 0;
	double Score = 0.0;
	S.GetColumnValueByIndex(0, Name);
	S.GetColumnValueByIndex(1, QualifiedName);
	S.GetColumnValueByIndex(2, Kind);
	S.GetColumnValueByIndex(3, File);
	S.GetColumnValueByIndex(4, Line);
	S.GetColumnValueByIndex(5, Score);
	S.GetColumnValueByIndex(6, Tier);
	S.GetColumnValueByIndex(7, ReasonsJson);
	S.GetColumnValueByIndex(8, RawCountsJson);
	S.GetColumnValueByIndex(9, ScoringVersion);
	S.GetColumnValueByIndex(10, CacheVersion);

	TArray<TSharedPtr<FJsonValue>> Reasons;
	if (!ParseJsonArray(ReasonsJson, Reasons))
	{
		Reasons.Add(MakeShared<FJsonValueString>(TEXT("cached reasons_json could not be parsed")));
	}
	TSharedPtr<FJsonObject> RawCounts = ParseJsonObject(RawCountsJson);
	if (!RawCounts.IsValid())
	{
		RawCounts = MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("id"), static_cast<double>(SymbolId));
	O->SetStringField(TEXT("name"), Name);
	O->SetStringField(TEXT("qualified_name"), QualifiedName);
	O->SetStringField(TEXT("kind"), Kind);
	O->SetStringField(TEXT("file"), File);
	O->SetNumberField(TEXT("line"), Line);
	O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
	O->SetStringField(TEXT("tier"), Tier.IsEmpty() ? TierForScore(Score) : Tier);
	O->SetArrayField(TEXT("reasons"), Reasons);
	O->SetObjectField(TEXT("raw_counts"), RawCounts);
	O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("hit"), CacheVersion, ScoringVersion));
	return O;
}

static TSharedPtr<FJsonObject> ScoreSymbolLocked(FSQLiteDatabase& DB, const FDetectSymbolRow& Sym)
{
	if (TSharedPtr<FJsonObject> Cached = CachedRiskForSymbolLocked(DB, Sym.Id))
	{
		return Cached;
	}

	const int64 Callers = CountIdLocked(DB, TEXT("SELECT COUNT(*) FROM \"references\" WHERE to_symbol_id = ?;"), Sym.Id);
	const int64 Callees = CountIdLocked(DB, TEXT("SELECT COUNT(*) FROM \"references\" WHERE from_symbol_id = ?;"), Sym.Id);
	const int64 Descendants = CountIdLocked(DB, TEXT("SELECT COUNT(*) FROM inheritance WHERE parent_id = ?;"), Sym.Id);
	const int64 Ancestors = CountIdLocked(DB, TEXT("SELECT COUNT(*) FROM inheritance WHERE child_id = ?;"), Sym.Id);
	const int64 CallerFiles = CountIdLocked(DB, TEXT("SELECT COUNT(DISTINCT file_id) FROM \"references\" WHERE to_symbol_id = ?;"), Sym.Id);

	FString SensitivityReason;
	const double Sensitivity = SourceSensitivityFactor(
		FString::Printf(TEXT("%s %s %s %s"), *Sym.Name, *Sym.QualifiedName, *Sym.Kind, *Sym.Signature),
		SensitivityReason);

	TArray<TSharedPtr<FJsonValue>> Reasons;
	double Raw = 0.0;
	auto Factor = [&](double Contribution, const FString& Why)
	{
		if (Contribution > 0.0)
		{
			Raw += Contribution;
			Reasons.Add(MakeShared<FJsonValueString>(Why));
		}
	};

	Factor(FMath::Min<double>(Callers, 50) / 50.0 * 0.35,
		FString::Printf(TEXT("caller fan-in: %lld"), Callers));
	Factor(FMath::Min<double>(Descendants, 30) / 30.0 * 0.25,
		FString::Printf(TEXT("inheritance descendants (1-hop): %lld"), Descendants));
	Factor(FMath::Min<double>(Callees, 50) / 50.0 * 0.10,
		FString::Printf(TEXT("callee fan-out: %lld"), Callees));
	Factor(Sym.bIsUEMacro ? 0.15 : 0.0,
		TEXT("UE reflection macro symbol (UCLASS/UFUNCTION/UPROPERTY family)"));
	Factor(CallerFiles > 1 ? FMath::Min<double>(CallerFiles, 20) / 20.0 * 0.15 : 0.0,
		FString::Printf(TEXT("module/file boundary crossing: %lld distinct caller file(s)"), CallerFiles));
	Factor(Sensitivity, SensitivityReason);

	if (Callers == 0 && Sym.Kind.Contains(TEXT("function")))
	{
		Reasons.Add(MakeShared<FJsonValueString>(TEXT(
			"missing direct callers: function has 0 indexed callers — may be reflection/delegate/Blueprint-invoked (static graph cannot see those)")));
	}

	const double Score = FMath::Clamp(Raw, 0.0, 1.0);
	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("id"), static_cast<double>(Sym.Id));
	O->SetStringField(TEXT("name"), Sym.Name);
	O->SetStringField(TEXT("qualified_name"), Sym.QualifiedName);
	O->SetStringField(TEXT("kind"), Sym.Kind);
	O->SetNumberField(TEXT("file_id"), static_cast<double>(Sym.FileId));
	O->SetStringField(TEXT("file"), Sym.File);
	O->SetNumberField(TEXT("line"), Sym.LineStart);
	O->SetNumberField(TEXT("line_start"), Sym.LineStart);
	O->SetNumberField(TEXT("line_end"), Sym.LineEnd);
	O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
	O->SetStringField(TEXT("tier"), TierForScore(Score));
	O->SetArrayField(TEXT("reasons"), Reasons);

	TSharedPtr<FJsonObject> RawCounts = MakeShared<FJsonObject>();
	RawCounts->SetNumberField(TEXT("callers"), static_cast<double>(Callers));
	RawCounts->SetNumberField(TEXT("callees"), static_cast<double>(Callees));
	RawCounts->SetNumberField(TEXT("descendants"), static_cast<double>(Descendants));
	RawCounts->SetNumberField(TEXT("ancestors"), static_cast<double>(Ancestors));
	RawCounts->SetNumberField(TEXT("caller_files"), static_cast<double>(CallerFiles));
	RawCounts->SetBoolField(TEXT("is_ue_macro"), Sym.bIsUEMacro);
	RawCounts->SetNumberField(TEXT("sensitivity"), Sensitivity);
	O->SetObjectField(TEXT("raw_counts"), RawCounts);
	O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("miss"), TEXT(""), TEXT("3")));
	return O;
}

static TSharedPtr<FJsonObject> SymbolByIdLocked(FSQLiteDatabase& DB, int64 SymbolId)
{
	FSQLitePreparedStatement S;
	if (!S.Create(DB, TEXT(
		"SELECT s.name,s.qualified_name,s.kind,s.file_id,COALESCE(f.path,''),s.line_start,s.line_end "
		"FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE s.id = ? LIMIT 1;")))
	{
		return nullptr;
	}
	S.SetBindingValueByIndex(1, SymbolId);
	if (S.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return nullptr;
	}
	FString Name, QualifiedName, Kind, File;
	int64 FileId = 0;
	int32 LineStart = 0, LineEnd = 0;
	S.GetColumnValueByIndex(0, Name);
	S.GetColumnValueByIndex(1, QualifiedName);
	S.GetColumnValueByIndex(2, Kind);
	S.GetColumnValueByIndex(3, FileId);
	S.GetColumnValueByIndex(4, File);
	S.GetColumnValueByIndex(5, LineStart);
	S.GetColumnValueByIndex(6, LineEnd);

	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("id"), static_cast<double>(SymbolId));
	O->SetStringField(TEXT("name"), Name);
	O->SetStringField(TEXT("qualified_name"), QualifiedName);
	O->SetStringField(TEXT("kind"), Kind);
	O->SetNumberField(TEXT("file_id"), static_cast<double>(FileId));
	O->SetStringField(TEXT("file"), File);
	O->SetNumberField(TEXT("line_start"), LineStart);
	O->SetNumberField(TEXT("line_end"), LineEnd);
	return O;
}

static bool HasIndexedTestReferenceLocked(FSQLiteDatabase& DB, int64 SymbolId)
{
	FSQLitePreparedStatement S;
	if (!S.Create(DB, TEXT(
		"SELECT 1 FROM \"references\" r "
		"JOIN symbols fs ON fs.id = r.from_symbol_id "
		"LEFT JOIN files ff ON ff.id = fs.file_id "
		"WHERE r.to_symbol_id = ? "
		"AND (replace(COALESCE(ff.path,''),'\\','/') LIKE '%/Tests/%' "
		"  OR fs.name LIKE '%Spec%' "
		"  OR fs.qualified_name LIKE '%AutomationTest%' "
		"  OR fs.name LIKE '%_Test%') "
		"LIMIT 1;")))
	{
		return false;
	}
	S.SetBindingValueByIndex(1, SymbolId);
	return S.Step() == ESQLitePreparedStatementStepResult::Row;
}

static const TCHAR* GCrgProjectionDdl =
	TEXT("CREATE TABLE IF NOT EXISTS crg_nodes (")
	TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT,")
	TEXT("domain TEXT NOT NULL,")
	TEXT("native_table TEXT NOT NULL,")
	TEXT("native_id INTEGER NOT NULL,")
	TEXT("stable_key TEXT NOT NULL,")
	TEXT("kind TEXT,")
	TEXT("name TEXT,")
	TEXT("path TEXT,")
	TEXT("module TEXT,")
	TEXT("source_revision TEXT,")
	TEXT("extra TEXT,")
	TEXT("updated_at INTEGER NOT NULL,")
	TEXT("UNIQUE(domain, native_table, native_id),")
	TEXT("UNIQUE(domain, stable_key)")
	TEXT(");")
	TEXT("CREATE TABLE IF NOT EXISTS crg_edges (")
	TEXT("id INTEGER PRIMARY KEY AUTOINCREMENT,")
	TEXT("domain TEXT NOT NULL,")
	TEXT("source_node_id INTEGER NOT NULL,")
	TEXT("target_node_id INTEGER NOT NULL,")
	TEXT("edge_kind TEXT NOT NULL,")
	TEXT("edge_subkind TEXT,")
	TEXT("weight REAL NOT NULL DEFAULT 1.0,")
	TEXT("native_table TEXT,")
	TEXT("native_id INTEGER,")
	TEXT("updated_at INTEGER NOT NULL")
	TEXT(");")
	TEXT("CREATE TABLE IF NOT EXISTS crg_node_metrics (")
	TEXT("node_id INTEGER PRIMARY KEY,")
	TEXT("fan_in INTEGER NOT NULL DEFAULT 0,")
	TEXT("fan_out INTEGER NOT NULL DEFAULT 0,")
	TEXT("hard_in INTEGER NOT NULL DEFAULT 0,")
	TEXT("descendants INTEGER NOT NULL DEFAULT 0,")
	TEXT("risk_score REAL NOT NULL DEFAULT 0.0,")
	TEXT("risk_tier TEXT NOT NULL DEFAULT 'low',")
	TEXT("reasons_json TEXT NOT NULL DEFAULT '[]',")
	TEXT("raw_counts_json TEXT NOT NULL DEFAULT '{}',")
	TEXT("scoring_version TEXT NOT NULL,")
	TEXT("computed_at INTEGER NOT NULL")
	TEXT(");")
	TEXT("CREATE TABLE IF NOT EXISTS crg_meta (")
	TEXT("key TEXT PRIMARY KEY,")
	TEXT("value TEXT NOT NULL")
	TEXT(");")
	TEXT("CREATE TABLE IF NOT EXISTS source_override_edges (")
	TEXT("child_symbol_id INTEGER NOT NULL,")
	TEXT("parent_symbol_id INTEGER NOT NULL,")
	TEXT("confidence TEXT NOT NULL DEFAULT 'high',")
	TEXT("reason TEXT NOT NULL DEFAULT '',")
	TEXT("updated_at INTEGER NOT NULL,")
	TEXT("PRIMARY KEY(child_symbol_id, parent_symbol_id)")
	TEXT(");")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_domain_native ON crg_nodes(domain, native_table, native_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_stable ON crg_nodes(domain, stable_key);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_native ON crg_edges(domain, native_table, native_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_source ON crg_edges(domain, source_node_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_target ON crg_edges(domain, target_node_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_kind_subkind ON crg_edges(domain, edge_kind, edge_subkind);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_metrics_score ON crg_node_metrics(risk_score DESC);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_source_override_edges_parent ON source_override_edges(parent_symbol_id, child_symbol_id);");

static const TCHAR* GSourceReviewIndexDdl =
	TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_parent_name_kind ON symbols(parent_symbol_id, name, kind);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_name_kind_parent ON symbols(name, kind, parent_symbol_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_symbols_override_signature ON symbols(kind, parent_symbol_id, name) WHERE kind='function' AND signature LIKE '%override%';")
	TEXT("CREATE INDEX IF NOT EXISTS idx_references_to_symbol ON \"references\"(to_symbol_id, from_symbol_id, file_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_references_from_symbol ON \"references\"(from_symbol_id, to_symbol_id, file_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_references_file ON \"references\"(file_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_inheritance_parent_child ON inheritance(parent_id, child_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_inheritance_child_parent ON inheritance(child_id, parent_id);");

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

FSQLiteDatabase* FMonolithSourceDatabase::GetRawHandle() const
{
	// Deliberately NOT locked here: the borrower (MonolithReflectionIntel) holds
	// GetLock() across the whole fetch-prepare-step sequence, so taking DbLock
	// here too would either deadlock (non-recursive FCriticalSection) or give a
	// false sense of safety for a pointer that outlives this call. Return the
	// raw pointer only when the handle is genuinely open; the borrower locks.
	return (Database != nullptr && Database->IsValid()) ? Database : nullptr;
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
	Stmt.GetColumnValueByIndex(7, Sym.ParentSymbolId);
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

TArray<FMonolithSourceSymbol> FMonolithSourceDatabase::GetSymbolsByName(const FString& Name, const FString& Kind, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceSymbol> Result;
	if (!Database || !Database->IsValid()) return Result;

	const int32 SafeLimit = Limit > 0 ? FMath::Clamp(Limit, 1, 1000) : 0;
	const bool bQualifiedLookup = Name.Contains(TEXT("::"));
	FSQLitePreparedStatement Stmt;
	if (bQualifiedLookup)
	{
		const FString DuplicatedName = MakeDuplicatedQualifiedLookupName(Name);
		const FString ShortOwnerDuplicatedName = MakeShortOwnerDuplicatedQualifiedLookupName(Name);
		const FString KindClause = Kind.IsEmpty() ? TEXT("") : TEXT(" AND s.kind = ?");
		const FString LimitClause = SafeLimit > 0 ? TEXT(" LIMIT ?") : TEXT("");
		const FString Sql = FString::Printf(TEXT(
			"SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro "
			"FROM symbols s LEFT JOIN files f ON f.id = s.file_id "
			"WHERE (s.qualified_name = ? OR s.name = ? OR s.qualified_name = ? OR s.qualified_name = ?)%s "
			"ORDER BY CASE WHEN s.qualified_name = ? THEN 0 WHEN s.name = ? THEN 1 WHEN s.qualified_name = ? THEN 2 WHEN s.qualified_name = ? THEN 3 ELSE 4 END, "
			"         CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, "
			"         (s.line_end > s.line_start) DESC, s.id%s;"),
			*KindClause, *LimitClause);
		Stmt.Create(*Database, *Sql);
		Stmt.SetBindingValueByIndex(1, Name);
		Stmt.SetBindingValueByIndex(2, Name);
		Stmt.SetBindingValueByIndex(3, DuplicatedName);
		Stmt.SetBindingValueByIndex(4, ShortOwnerDuplicatedName);
		int32 BindIndex = 5;
		if (!Kind.IsEmpty())
		{
			Stmt.SetBindingValueByIndex(BindIndex++, Kind);
		}
		Stmt.SetBindingValueByIndex(BindIndex++, Name);
		Stmt.SetBindingValueByIndex(BindIndex++, Name);
		Stmt.SetBindingValueByIndex(BindIndex++, DuplicatedName);
		Stmt.SetBindingValueByIndex(BindIndex++, ShortOwnerDuplicatedName);
		if (SafeLimit > 0)
		{
			Stmt.SetBindingValueByIndex(BindIndex++, static_cast<int64>(SafeLimit));
		}
	}
	else
	{
		if (Kind.IsEmpty())
		{
			const FString Sql = SafeLimit > 0
				? TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE s.name = ? ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, (s.line_end > s.line_start) DESC, s.id LIMIT ?;")
				: TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE s.name = ? ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, (s.line_end > s.line_start) DESC, s.id;");
			Stmt.Create(*Database, *Sql);
			Stmt.SetBindingValueByIndex(1, Name);
			if (SafeLimit > 0)
			{
				Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
			}
		}
		else
		{
			const FString Sql = SafeLimit > 0
				? TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE s.name = ? AND s.kind = ? ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, (s.line_end > s.line_start) DESC, s.id LIMIT ?;")
				: TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols s LEFT JOIN files f ON f.id = s.file_id WHERE s.name = ? AND s.kind = ? ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, (s.line_end > s.line_start) DESC, s.id;");
			Stmt.Create(*Database, *Sql);
			Stmt.SetBindingValueByIndex(1, Name);
			Stmt.SetBindingValueByIndex(2, Kind);
			if (SafeLimit > 0)
			{
				Stmt.SetBindingValueByIndex(3, static_cast<int64>(SafeLimit));
			}
		}
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
	Stmt.Create(*Database, TEXT("SELECT s.id, s.name, s.qualified_name, s.kind, s.file_id, s.line_start, s.line_end, s.parent_symbol_id, s.access, s.signature, s.docstring, s.is_ue_macro FROM symbols_fts fts JOIN symbols s ON s.id = fts.rowid LEFT JOIN files f ON f.id = s.file_id WHERE symbols_fts MATCH ? ORDER BY CASE WHEN f.path IS NULL OR f.path = '' OR f.path = '<unknown>' THEN 1 ELSE 0 END, bm25(symbols_fts), s.id LIMIT ?;"));
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

bool FMonolithSourceDatabase::GetFileModuleInfo(int64 FileId, FString& OutModuleName, FString& OutBuildCsPath)
{
	FScopeLock Lock(&DbLock);
	OutModuleName.Empty();
	OutBuildCsPath.Empty();
	if (!Database || !Database->IsValid()) return false;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("SELECT m.name, m.build_cs_path FROM files f JOIN modules m ON m.id = f.module_id WHERE f.id = ?;"));
	Stmt.SetBindingValueByIndex(1, FileId);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByIndex(0, OutModuleName);
		Stmt.GetColumnValueByIndex(1, OutBuildCsPath);
		return true;
	}
	return false;
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

TArray<FMonolithSourceOverrideEdge> FMonolithSourceDatabase::GetOverridesTo(int64 SymbolId, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceOverrideEdge> Result;
	if (!Database || !Database->IsValid()) return Result;

	const int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);
	const int32 ProbeLimit = FMath::Clamp(SafeLimit * 4, SafeLimit, 1000);
	FSQLitePreparedStatement Stmt;
	if (SourceOverrideEdgeCacheReadyLocked(*Database))
	{
		if (Stmt.Create(*Database, TEXT(
			"SELECT child_fn.id,base_fn.id,"
			"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
			"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
			"       e.confidence,e.reason "
			"FROM source_override_edges e "
			"JOIN symbols child_fn ON child_fn.id = e.child_symbol_id "
			"JOIN symbols base_fn ON base_fn.id = e.parent_symbol_id "
			"JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
			"JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id "
			"WHERE e.parent_symbol_id = ? "
			"ORDER BY child_fn.qualified_name "
			"LIMIT ?;")))
		{
			Stmt.SetBindingValueByIndex(1, SymbolId);
			Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
			ReadCachedOverrideEdges(Stmt, Result);
			return Result;
		}
	}

	Stmt.Create(*Database, TEXT(
		"WITH RECURSIVE descendants(base_class_id, child_class_id) AS ("
		"  SELECT parent_id, child_id FROM inheritance"
		"  UNION "
		"  SELECT d.base_class_id, i.child_id "
		"  FROM descendants d "
		"  JOIN inheritance AS i ON i.parent_id = d.child_class_id"
		") "
		"SELECT child_fn.id,base_fn.id,"
		"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
		"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
		"       COALESCE(child_fn.signature,''),COALESCE(base_fn.signature,'') "
		"FROM symbols base_fn "
		"JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id "
		"JOIN descendants AS d ON d.base_class_id = base_cls.id "
		"JOIN symbols child_cls ON child_cls.id = d.child_class_id "
		"JOIN symbols child_fn ON child_fn.parent_symbol_id = child_cls.id "
		"    AND (base_fn.name = child_fn.name "
		"      OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"    AND child_fn.kind = base_fn.kind "
		"    AND child_fn.id != base_fn.id "
		"WHERE base_fn.id = ? AND base_fn.kind = 'function' "
		"ORDER BY child_fn.qualified_name "
		"LIMIT ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(ProbeLimit));
	ReadOverrideEdges(Stmt, Result, SafeLimit);
	return Result;
}

TArray<FMonolithSourceOverrideEdge> FMonolithSourceDatabase::GetOverridesFrom(int64 SymbolId, int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TArray<FMonolithSourceOverrideEdge> Result;
	if (!Database || !Database->IsValid()) return Result;

	const int32 SafeLimit = FMath::Clamp(Limit, 1, 1000);
	const int32 ProbeLimit = FMath::Clamp(SafeLimit * 4, SafeLimit, 1000);
	FSQLitePreparedStatement Stmt;
	if (SourceOverrideEdgeCacheReadyLocked(*Database))
	{
		if (Stmt.Create(*Database, TEXT(
			"SELECT child_fn.id,base_fn.id,"
			"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
			"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
			"       e.confidence,e.reason "
			"FROM source_override_edges e "
			"JOIN symbols child_fn ON child_fn.id = e.child_symbol_id "
			"JOIN symbols base_fn ON base_fn.id = e.parent_symbol_id "
			"JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
			"JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id "
			"WHERE e.child_symbol_id = ? "
			"ORDER BY base_fn.qualified_name "
			"LIMIT ?;")))
		{
			Stmt.SetBindingValueByIndex(1, SymbolId);
			Stmt.SetBindingValueByIndex(2, static_cast<int64>(SafeLimit));
			ReadCachedOverrideEdges(Stmt, Result);
			return Result;
		}
	}

	Stmt.Create(*Database, TEXT(
		"WITH RECURSIVE ancestors(child_class_id, ancestor_class_id) AS ("
		"  SELECT child_id, parent_id FROM inheritance"
		"  UNION "
		"  SELECT a.child_class_id, i.parent_id "
		"  FROM ancestors a "
		"  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id"
		") "
		"SELECT child_fn.id,base_fn.id,"
		"       child_fn.name,child_fn.qualified_name,base_fn.name,base_fn.qualified_name,"
		"       child_cls.name,child_cls.qualified_name,base_cls.name,base_cls.qualified_name,"
		"       COALESCE(child_fn.signature,''),COALESCE(base_fn.signature,'') "
		"FROM symbols child_fn "
		"JOIN symbols child_cls ON child_cls.id = child_fn.parent_symbol_id "
		"JOIN ancestors AS a ON a.child_class_id = child_cls.id "
		"JOIN symbols base_cls ON base_cls.id = a.ancestor_class_id "
		"JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id = base_cls.id "
		"    AND (base_fn.name = child_fn.name "
		"      OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"    AND base_fn.kind = child_fn.kind "
		"WHERE child_fn.id = ? AND child_fn.kind = 'function' "
		"ORDER BY base_fn.qualified_name "
		"LIMIT ?;"));
	Stmt.SetBindingValueByIndex(1, SymbolId);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(ProbeLimit));
	ReadOverrideEdges(Stmt, Result, SafeLimit);
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
// FTS COUNT(*) helpers (Survivor E — plan §3.E)
//
// Issued ONLY on page 0 of cursor-paginated search_source. Mirrors the
// JOIN / WHERE clauses of SearchSymbolsFTSFiltered / SearchSourceFTSFiltered
// so the COUNT matches what the paged result would surface across the full
// rerun. ORDER BY / LIMIT are intentionally omitted — COUNT short-circuits.
// ============================================================

int32 FMonolithSourceDatabase::CountSymbolsFTSFiltered(const FString& Query, const FString& Kind, const FString& Module, const FString& PathFilter)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	const FString FTSQuery = EscapeFTS(Query);

	FString SQL = TEXT("SELECT COUNT(*) FROM symbols_fts f JOIN symbols s ON s.id = f.rowid ");
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
	SQL += TEXT(";");

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

	int32 Count = 0;
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		Count = static_cast<int32>(C64);
	}
	return Count;
}

int32 FMonolithSourceDatabase::CountSourceFTSFiltered(const FString& Query, const FString& Scope, const FString& Module, const FString& PathFilter)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	const FString FTSQuery = EscapeFTS(Query);

	// Fast path: no joins needed when neither module nor path filter is set
	// AND scope is "all" — single FTS COUNT(*).
	if (Scope == TEXT("all") && Module.IsEmpty() && PathFilter.IsEmpty())
	{
		FSQLitePreparedStatement Stmt;
		Stmt.Create(*Database, TEXT("SELECT COUNT(*) FROM source_fts WHERE source_fts MATCH ?;"));
		Stmt.SetBindingValueByIndex(1, FTSQuery);

		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 C64 = 0;
			Stmt.GetColumnValueByIndex(0, C64);
			return static_cast<int32>(C64);
		}
		return 0;
	}

	FString SQL = TEXT("SELECT COUNT(*) FROM source_fts sf JOIN files fi ON fi.id = sf.file_id ");
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
	SQL += TEXT(";");

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

	int32 Count = 0;
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		Count = static_cast<int32>(C64);
	}
	return Count;
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

	if (!ApplyEngineSourceDeletePragmas(*Database, DbPath))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("OpenForWriting: failed to configure journal mode: %s"), *DbPath);
		Database->Close();
		delete Database;
		Database = nullptr;
		return false;
	}
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
	if (CachedDbPath.IsEmpty())
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: no cached DB path"));
		return false;
	}

	if (Database)
	{
		Database->Close();
		delete Database;
		Database = nullptr;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(CachedDbPath));
	if (!DeleteSourceDatabaseFileIfPresent(PlatformFile, CachedDbPath, /*bRequired=*/true))
	{
		return false;
	}
	DeleteSourceDatabaseFileIfPresent(PlatformFile, CachedDbPath + TEXT("-journal"), /*bRequired=*/false);
	DeleteSourceDatabaseFileIfPresent(PlatformFile, CachedDbPath + TEXT("-wal"), /*bRequired=*/false);
	DeleteSourceDatabaseFileIfPresent(PlatformFile, CachedDbPath + TEXT("-shm"), /*bRequired=*/false);

	Database = new FSQLiteDatabase();
	if (!Database->Open(*CachedDbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: failed to recreate DB: %s"), *CachedDbPath);
		delete Database;
		Database = nullptr;
		return false;
	}

	if (!ApplyEngineSourceDeletePragmas(*Database, CachedDbPath))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: failed to configure journal mode: %s"), *CachedDbPath);
		Database->Close();
		delete Database;
		Database = nullptr;
		return false;
	}
	Database->Execute(TEXT("PRAGMA cache_size=-64000;"));

	UE_LOG(LogMonolithSource, Log, TEXT("ResetDatabase: recreated DB file at %s; recreating schema"), *CachedDbPath);

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
	if (!MetaStmt.Create(*Database, TEXT("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?);")))
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: meta statement failed — %s"), *Database->GetLastError());
		return false;
	}
	MetaStmt.SetBindingValueByIndex(1, FString(TEXT("schema_version")));
	MetaStmt.SetBindingValueByIndex(2, FString::FromInt(MonolithSourceSchema::SchemaVersion));
	if (MetaStmt.Step() != ESQLitePreparedStatementStepResult::Done)
	{
		UE_LOG(LogMonolithSource, Error, TEXT("ResetDatabase: meta write failed — %s"), *Database->GetLastError());
		return false;
	}

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

int32 FMonolithSourceDatabase::PruneIndexedFilesUnderRoots(const TArray<FString>& RootPaths)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return -1;

	TArray<FString> NormalizedRoots;
	NormalizedRoots.Reserve(RootPaths.Num());
	for (FString Root : RootPaths)
	{
		Root.TrimStartAndEndInline();
		if (Root.IsEmpty())
		{
			continue;
		}
		Root = FPaths::ConvertRelativePathToFull(Root);
		FPaths::NormalizeDirectoryName(Root);
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!Root.EndsWith(TEXT("/")))
		{
			Root += TEXT("/");
		}
		NormalizedRoots.AddUnique(Root);
	}
	if (NormalizedRoots.Num() == 0)
	{
		return 0;
	}

	TArray<int64> FileIds;
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, TEXT("SELECT id,path FROM files;")))
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("PruneIndexedFilesUnderRoots failed to read files table: %s"), *Database->GetLastError());
			return -1;
		}
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 FileId = 0;
			FString Path;
			Stmt.GetColumnValueByIndex(0, FileId);
			Stmt.GetColumnValueByIndex(1, Path);
			Path.ReplaceInline(TEXT("\\"), TEXT("/"));
			for (const FString& Root : NormalizedRoots)
			{
				if (Path.StartsWith(Root, ESearchCase::IgnoreCase))
				{
					FileIds.Add(FileId);
					break;
				}
			}
		}
	}
	if (FileIds.Num() == 0)
	{
		return 0;
	}

	auto ObjectExists = [&](const TCHAR* Type, const TCHAR* Name) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = ? AND name = ? LIMIT 1;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, FString(Type));
		Stmt.SetBindingValueByIndex(2, FString(Name));
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	};
	auto Exec = [&](const TCHAR* Sql) -> bool
	{
		if (!Database->Execute(Sql))
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("PruneIndexedFilesUnderRoots SQL failed: %s"), *Database->GetLastError());
			return false;
		}
		return true;
	};

	bool bOk = Exec(TEXT("BEGIN;"));
	if (bOk) bOk = Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_prune_files(id INTEGER PRIMARY KEY);"));
	if (bOk) bOk = Exec(TEXT("DELETE FROM monolith_prune_files;"));
	if (bOk) bOk = Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_prune_symbols(id INTEGER PRIMARY KEY);"));
	if (bOk) bOk = Exec(TEXT("DELETE FROM monolith_prune_symbols;"));
	if (bOk) bOk = Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_pending_ids(id INTEGER PRIMARY KEY);"));
	if (bOk) bOk = Exec(TEXT("DELETE FROM monolith_source_crg_pending_ids;"));

	if (bOk)
	{
		FSQLitePreparedStatement InsertFile;
		bOk = InsertFile.Create(*Database, TEXT("INSERT OR IGNORE INTO monolith_prune_files(id) VALUES(?);"));
		for (int64 FileId : FileIds)
		{
			if (!bOk) break;
			InsertFile.Reset();
			InsertFile.SetBindingValueByIndex(1, FileId);
			bOk = InsertFile.Step() == ESQLitePreparedStatementStepResult::Done;
		}
	}
	if (bOk)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_prune_symbols(id) "
			"SELECT id FROM symbols WHERE file_id IN (SELECT id FROM monolith_prune_files);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_prune_symbols(id) "
			"SELECT s.id FROM symbols s "
			"LEFT JOIN files f ON f.id = s.file_id "
			"WHERE f.id IS NULL;"));
	}

	const bool bHasCrgNodes = ObjectExists(TEXT("table"), TEXT("crg_nodes"));
	const bool bHasCrgEdges = ObjectExists(TEXT("table"), TEXT("crg_edges"));
	const bool bHasCrgMetrics = ObjectExists(TEXT("table"), TEXT("crg_node_metrics"));
	if (bOk && bHasCrgNodes)
	{
		bOk = Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_prune_crg_nodes(id INTEGER PRIMARY KEY);"));
	}
	if (bOk && bHasCrgNodes)
	{
		bOk = Exec(TEXT("DELETE FROM monolith_prune_crg_nodes;"));
	}
	if (bOk && bHasCrgNodes)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_prune_crg_nodes(id) "
			"SELECT id FROM crg_nodes "
			"WHERE domain='source' AND native_table='symbols' "
			"AND native_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_source_crg_pending_ids(id) "
			"SELECT id FROM monolith_prune_symbols;"));
	}
	if (bOk && bHasCrgNodes && bHasCrgEdges)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_source_crg_pending_ids(id) "
			"SELECT other.native_id FROM crg_edges e "
			"JOIN monolith_prune_crg_nodes old ON old.id = e.source_node_id "
			"JOIN crg_nodes other ON other.id = e.target_node_id "
			"WHERE e.domain='source' AND other.domain='source' AND other.native_table='symbols';"));
	}
	if (bOk && bHasCrgNodes && bHasCrgEdges)
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_source_crg_pending_ids(id) "
			"SELECT other.native_id FROM crg_edges e "
			"JOIN monolith_prune_crg_nodes old ON old.id = e.target_node_id "
			"JOIN crg_nodes other ON other.id = e.source_node_id "
			"WHERE e.domain='source' AND other.domain='source' AND other.native_table='symbols';"));
	}
	if (bOk && ObjectExists(TEXT("table"), TEXT("source_override_edges")))
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_source_crg_pending_ids(id) "
			"SELECT parent_symbol_id FROM source_override_edges "
			"WHERE child_symbol_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk && ObjectExists(TEXT("table"), TEXT("source_override_edges")))
	{
		bOk = Exec(TEXT(
			"INSERT OR IGNORE INTO monolith_source_crg_pending_ids(id) "
			"SELECT child_symbol_id FROM source_override_edges "
			"WHERE parent_symbol_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk && bHasCrgNodes && bHasCrgMetrics)
	{
		bOk = Exec(TEXT(
			"DELETE FROM crg_node_metrics "
			"WHERE node_id IN (SELECT id FROM monolith_prune_crg_nodes);"));
	}
	if (bOk && bHasCrgNodes && bHasCrgEdges)
	{
		bOk = Exec(TEXT(
			"DELETE FROM crg_edges "
			"WHERE domain='source' AND ("
			"source_node_id IN (SELECT id FROM monolith_prune_crg_nodes) "
			"OR target_node_id IN (SELECT id FROM monolith_prune_crg_nodes));"));
	}
	if (bOk && bHasCrgNodes)
	{
		bOk = Exec(TEXT("DELETE FROM crg_nodes WHERE id IN (SELECT id FROM monolith_prune_crg_nodes);"));
	}
	if (bOk && ObjectExists(TEXT("table"), TEXT("source_override_edges")))
	{
		bOk = Exec(TEXT(
			"DELETE FROM source_override_edges "
			"WHERE child_symbol_id IN (SELECT id FROM monolith_prune_symbols) "
			"OR parent_symbol_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT(
			"DELETE FROM \"references\" "
			"WHERE file_id IN (SELECT id FROM monolith_prune_files) "
			"OR from_symbol_id IN (SELECT id FROM monolith_prune_symbols) "
			"OR to_symbol_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT(
			"DELETE FROM inheritance "
			"WHERE child_id IN (SELECT id FROM monolith_prune_symbols) "
			"OR parent_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT("DELETE FROM includes WHERE file_id IN (SELECT id FROM monolith_prune_files);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT("DELETE FROM source_fts WHERE file_id IN (SELECT id FROM monolith_prune_files);"));
	}
	if (bOk && ObjectExists(TEXT("table"), TEXT("symbol_deprecations")))
	{
		bOk = Exec(TEXT("DELETE FROM symbol_deprecations WHERE symbol_id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT("DELETE FROM symbols WHERE id IN (SELECT id FROM monolith_prune_symbols);"));
	}
	if (bOk)
	{
		bOk = Exec(TEXT("DELETE FROM files WHERE id IN (SELECT id FROM monolith_prune_files);"));
	}

	if (bOk)
	{
		if (!Exec(TEXT("COMMIT;")))
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("Failed to commit project source prune before scoped source reindex"));
			return -1;
		}
		UE_LOG(LogMonolithSource, Log, TEXT("Pruned %d indexed project source file(s) before scoped source reindex"), FileIds.Num());
		return FileIds.Num();
	}

	Exec(TEXT("ROLLBACK;"));
	UE_LOG(LogMonolithSource, Warning, TEXT("Failed to prune indexed project source files before scoped source reindex; rolled back"));
	return -1;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::RefreshCrgCacheForFiles(const TSet<int64>& FileIds, const FString& Context)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("context"), Context);
	TArray<TSharedPtr<FJsonValue>> FileIdValues;
	TArray<int64> SortedFileIds = FileIds.Array();
	SortedFileIds.Sort();
	for (int64 FileId : SortedFileIds)
	{
		FileIdValues.Add(MakeShared<FJsonValueNumber>(static_cast<double>(FileId)));
	}
	Input->SetArrayField(TEXT("file_ids"), FileIdValues);
	Root->SetObjectField(TEXT("input"), Input);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}
	if (FileIds.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), TEXT("No indexed source files; skipped scoped source CRG refresh."));
		Root->SetStringField(TEXT("refresh_mode"), TEXT("skipped"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.health"), TEXT("source.risk_score"), TEXT("source.review_context") });
		return Root;
	}

	auto ObjectExists = [&](const TCHAR* Type, const TCHAR* Name) -> bool
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = ? AND name = ? LIMIT 1;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, FString(Type));
		Stmt.SetBindingValueByIndex(2, FString(Name));
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	};
	const bool bHasProjectionTables =
		ObjectExists(TEXT("table"), TEXT("crg_nodes"))
		&& ObjectExists(TEXT("table"), TEXT("crg_edges"))
		&& ObjectExists(TEXT("table"), TEXT("crg_node_metrics"))
		&& ObjectExists(TEXT("table"), TEXT("crg_meta"));
	if (!bHasProjectionTables)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), TEXT("Source CRG projection tables are missing; completion health-gate must run full source.repair_crg_cache."));
		Root->SetStringField(TEXT("refresh_mode"), TEXT("full_required"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.repair_crg_cache execute=true"), TEXT("source.health") });
		return Root;
	}

	bool bOk = ExecuteMulti(*Database, GCrgProjectionDdl);
	if (bOk)
	{
		bOk = ExecuteMulti(*Database, GSourceReviewIndexDdl);
	}
	auto Exec = [&](const TCHAR* Sql, const TCHAR* Label)
	{
		if (!bOk) return;
		const double StepStart = FPlatformTime::Seconds();
		UE_LOG(LogMonolithSource, Log, TEXT("Scoped source CRG refresh step begin: %s"), Label);
		if (!Database->Execute(Sql))
		{
			bOk = false;
			const FString Error = Database->GetLastError();
			UE_LOG(LogMonolithSource, Warning, TEXT("Scoped source CRG refresh step failed: %s error=%s"), Label, *Error);
			Warnings.Add(MakeShared<FJsonValueString>(
				Error.IsEmpty()
					? FString::Printf(TEXT("Scoped source CRG refresh failed at %s"), Label)
					: FString::Printf(TEXT("Scoped source CRG refresh failed at %s: %s"), Label, *Error)));
			return;
		}
		const double ElapsedSeconds = FPlatformTime::Seconds() - StepStart;
		UE_LOG(LogMonolithSource, Log, TEXT("Scoped source CRG refresh step end: %s elapsed=%.3fs"), Label, ElapsedSeconds);
	};
	auto CountTemp = [&](const TCHAR* TableName) -> int64
	{
		FSQLitePreparedStatement Stmt;
		const FString Sql = FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), TableName);
		if (!Stmt.Create(*Database, *Sql)) return 0;
		int64 Count = 0;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return Count;
	};
	auto CountSql = [&](const TCHAR* Sql) -> int64
	{
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Database, Sql)) return -1;
		int64 Count = 0;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return Count;
	};

	if (bOk) bOk = Database->Execute(TEXT("BEGIN;"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_files(id INTEGER PRIMARY KEY);"), TEXT("temp file ids"));
	Exec(TEXT("DELETE FROM monolith_source_crg_files;"), TEXT("clear temp file ids"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_seed_symbols(id INTEGER PRIMARY KEY);"), TEXT("temp seed symbols"));
	Exec(TEXT("DELETE FROM monolith_source_crg_seed_symbols;"), TEXT("clear temp seed symbols"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_node_symbols(id INTEGER PRIMARY KEY);"), TEXT("temp node symbols"));
	Exec(TEXT("DELETE FROM monolith_source_crg_node_symbols;"), TEXT("clear temp node symbols"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_refresh_symbols(id INTEGER PRIMARY KEY);"), TEXT("temp refresh symbols"));
	Exec(TEXT("DELETE FROM monolith_source_crg_refresh_symbols;"), TEXT("clear temp refresh symbols"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_old_nodes(id INTEGER PRIMARY KEY);"), TEXT("temp old nodes"));
	Exec(TEXT("DELETE FROM monolith_source_crg_old_nodes;"), TEXT("clear temp old nodes"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_pending_ids(id INTEGER PRIMARY KEY);"), TEXT("temp pending prune ids"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_reference_ids(id INTEGER PRIMARY KEY);"), TEXT("temp reference ids"));
	Exec(TEXT("DELETE FROM monolith_source_crg_reference_ids;"), TEXT("clear temp reference ids"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_crg_inheritance_ids(id INTEGER PRIMARY KEY);"), TEXT("temp inheritance ids"));
	Exec(TEXT("DELETE FROM monolith_source_crg_inheritance_ids;"), TEXT("clear temp inheritance ids"));
	Exec(TEXT("CREATE TEMP TABLE IF NOT EXISTS monolith_source_override_refresh_children(id INTEGER PRIMARY KEY);"), TEXT("temp override refresh children"));
	Exec(TEXT("DELETE FROM monolith_source_override_refresh_children;"), TEXT("clear temp override refresh children"));

	if (bOk)
	{
		FSQLitePreparedStatement InsertFile;
		bOk = InsertFile.Create(*Database, TEXT("INSERT OR IGNORE INTO monolith_source_crg_files(id) VALUES(?);"));
		for (int64 FileId : SortedFileIds)
		{
			if (!bOk) break;
			InsertFile.Reset();
			InsertFile.SetBindingValueByIndex(1, FileId);
			bOk = InsertFile.Step() == ESQLitePreparedStatementStepResult::Done;
		}
		if (!bOk)
		{
			Warnings.Add(MakeShared<FJsonValueString>(TEXT("Scoped source CRG refresh failed while binding file ids")));
		}
	}

	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_seed_symbols(id) "
		"SELECT id FROM symbols WHERE file_id IN (SELECT id FROM monolith_source_crg_files);"), TEXT("seed file symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_node_symbols(id) "
		"SELECT id FROM monolith_source_crg_seed_symbols;"), TEXT("copy seed node symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT s.id FROM symbols s "
		"JOIN monolith_source_crg_pending_ids p ON p.id = s.id;"), TEXT("pending prune neighbor symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT id FROM monolith_source_crg_seed_symbols;"), TEXT("copy seed symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT from_symbol_id FROM \"references\" "
		"WHERE to_symbol_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("reference inbound neighbors"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT to_symbol_id FROM \"references\" "
		"WHERE from_symbol_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("reference outbound neighbors"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT child_id FROM inheritance "
		"WHERE parent_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("inheritance child neighbors"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_refresh_symbols(id) "
		"SELECT parent_id FROM inheritance "
		"WHERE child_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("inheritance parent neighbors"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_old_nodes(id) "
		"SELECT id FROM crg_nodes "
		"WHERE domain='source' AND native_table='symbols' "
		"AND native_id IN (SELECT id FROM monolith_source_crg_node_symbols);"), TEXT("old source CRG nodes"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_reference_ids(id) "
		"SELECT id FROM \"references\" "
		"WHERE from_symbol_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("reference ids from seed symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_reference_ids(id) "
		"SELECT id FROM \"references\" "
		"WHERE to_symbol_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("reference ids to seed symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_inheritance_ids(id) "
		"SELECT id FROM inheritance "
		"WHERE child_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("inheritance ids from seed symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_crg_inheritance_ids(id) "
		"SELECT id FROM inheritance "
		"WHERE parent_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("inheritance ids to seed symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_override_refresh_children(id) "
		"SELECT id FROM symbols "
		"WHERE kind='function' AND signature LIKE '%override%' "
		"AND id IN (SELECT id FROM monolith_source_crg_refresh_symbols);"), TEXT("override child seed from scoped symbols"));
	Exec(TEXT(
		"INSERT OR IGNORE INTO monolith_source_override_refresh_children(id) "
		"SELECT child_symbol_id FROM source_override_edges "
		"WHERE child_symbol_id IN (SELECT id FROM monolith_source_crg_refresh_symbols) "
		"   OR parent_symbol_id IN (SELECT id FROM monolith_source_crg_refresh_symbols);"), TEXT("override child seed from existing edges"));
	Exec(TEXT(
		"WITH RECURSIVE refreshed_base(id,name,kind,parent_symbol_id) AS ("
		"  SELECT id,name,kind,parent_symbol_id FROM symbols "
		"  WHERE kind='function' AND id IN (SELECT id FROM monolith_source_crg_seed_symbols)"
		"), descendants(child_class_id, ancestor_class_id) AS ("
		"  SELECT child_id,parent_id FROM inheritance "
		"  WHERE parent_id IN (SELECT parent_symbol_id FROM refreshed_base)"
		"  UNION "
		"  SELECT i.child_id,d.ancestor_class_id "
		"  FROM inheritance i JOIN descendants d ON d.child_class_id = i.parent_id"
		") "
		"INSERT OR IGNORE INTO monolith_source_override_refresh_children(id) "
		"SELECT child_fn.id "
		"FROM refreshed_base base_fn "
		"JOIN symbols base_cls ON base_cls.id = base_fn.parent_symbol_id "
		"JOIN descendants d ON d.ancestor_class_id = base_fn.parent_symbol_id "
		"JOIN symbols child_fn INDEXED BY idx_symbols_parent_name_kind ON child_fn.parent_symbol_id = d.child_class_id "
		"  AND child_fn.kind = base_fn.kind "
		"  AND (child_fn.name = base_fn.name OR base_fn.name = base_cls.name || '::' || child_fn.name) "
		"WHERE child_fn.kind='function' AND child_fn.signature LIKE '%override%';"), TEXT("override child seed from refreshed bases"));

	const int64 FileCount = bOk ? CountTemp(TEXT("monolith_source_crg_files")) : 0;
	const int64 NodeSymbolCount = bOk ? CountTemp(TEXT("monolith_source_crg_node_symbols")) : 0;
	const int64 RefreshSymbolCount = bOk ? CountTemp(TEXT("monolith_source_crg_refresh_symbols")) : 0;
	const int64 OldNodeCount = bOk ? CountTemp(TEXT("monolith_source_crg_old_nodes")) : 0;
	const int64 ReferenceEdgeCount = bOk ? CountTemp(TEXT("monolith_source_crg_reference_ids")) : 0;
	const int64 InheritanceEdgeCount = bOk ? CountTemp(TEXT("monolith_source_crg_inheritance_ids")) : 0;
	const int64 OverrideChildSeedCount = bOk ? CountTemp(TEXT("monolith_source_override_refresh_children")) : 0;

	Exec(TEXT("DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM monolith_source_crg_refresh_symbols);"), TEXT("delete scoped metrics"));
	Exec(TEXT(
		"DELETE FROM crg_edges "
		"WHERE domain='source' AND ("
		"source_node_id IN (SELECT id FROM monolith_source_crg_old_nodes) "
		"OR target_node_id IN (SELECT id FROM monolith_source_crg_old_nodes) "
		"OR (native_table='references' AND native_id IN (SELECT id FROM monolith_source_crg_reference_ids)) "
		"OR (native_table='inheritance' AND native_id IN (SELECT id FROM monolith_source_crg_inheritance_ids)));"), TEXT("delete scoped edges"));
	Exec(TEXT("DELETE FROM crg_nodes WHERE id IN (SELECT id FROM monolith_source_crg_old_nodes);"), TEXT("delete scoped nodes"));
	Exec(TEXT(
		"INSERT OR REPLACE INTO crg_nodes(id,domain,native_table,native_id,stable_key,kind,name,path,module,source_revision,extra,updated_at) "
		"SELECT s.id,'source','symbols',s.id,COALESCE(s.qualified_name,s.name) || '#' || s.id,"
		"s.kind,s.name,COALESCE(f.path,''),COALESCE(m.name,''),'','{}',CAST(strftime('%s','now') AS INTEGER) "
		"FROM symbols s "
		"LEFT JOIN files f ON f.id = s.file_id "
		"LEFT JOIN modules m ON m.id = f.module_id "
		"JOIN monolith_source_crg_node_symbols ns ON ns.id = s.id;"), TEXT("insert scoped nodes"));
	Exec(TEXT(
		"INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at) "
		"SELECT 'source',r.from_symbol_id,r.to_symbol_id,COALESCE(r.ref_kind,'reference'),'reference',1.0,'references',r.id,CAST(strftime('%s','now') AS INTEGER) "
		"FROM monolith_source_crg_reference_ids rid "
		"JOIN \"references\" r ON r.id = rid.id "
		"WHERE EXISTS (SELECT 1 FROM crg_nodes fs WHERE fs.id = r.from_symbol_id AND fs.domain='source' AND fs.native_table='symbols') "
		"  AND EXISTS (SELECT 1 FROM crg_nodes ts WHERE ts.id = r.to_symbol_id AND ts.domain='source' AND ts.native_table='symbols');"), TEXT("insert scoped reference edges"));
	Exec(TEXT(
		"INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at) "
		"SELECT 'source',i.child_id,i.parent_id,'inheritance','extends',1.0,'inheritance',i.id,CAST(strftime('%s','now') AS INTEGER) "
		"FROM monolith_source_crg_inheritance_ids iid "
		"JOIN inheritance i ON i.id = iid.id "
		"WHERE EXISTS (SELECT 1 FROM crg_nodes cs WHERE cs.id = i.child_id AND cs.domain='source' AND cs.native_table='symbols') "
		"  AND EXISTS (SELECT 1 FROM crg_nodes ps WHERE ps.id = i.parent_id AND ps.domain='source' AND ps.native_table='symbols');"), TEXT("insert scoped inheritance edges"));
	Exec(TEXT(
		"WITH counts AS ("
		" SELECT s.id AS native_id,"
		"        (SELECT COUNT(*) FROM \"references\" r INDEXED BY idx_references_to_symbol WHERE r.to_symbol_id = s.id) AS fan_in,"
		"        (SELECT COUNT(*) FROM \"references\" r INDEXED BY idx_references_from_symbol WHERE r.from_symbol_id = s.id) AS fan_out,"
		"        (SELECT COUNT(*) FROM inheritance i INDEXED BY idx_inheritance_parent_child WHERE i.parent_id = s.id) AS descendants,"
		"        (SELECT COUNT(*) FROM inheritance i INDEXED BY idx_inheritance_child_parent WHERE i.child_id = s.id) AS ancestors,"
		"        (SELECT COUNT(DISTINCT r.file_id) FROM \"references\" r INDEXED BY idx_references_to_symbol WHERE r.to_symbol_id = s.id) AS caller_files,"
		"        s.is_ue_macro AS is_ue_macro,"
		"        CASE"
		"          WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%ufunction%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%server%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%client%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%netmulticast%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%onrep%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%replication%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%rpc%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%network%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%save%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%serialize%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%archive%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%auth%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%login%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%account%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%session%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%purchase%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%iap%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%store%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%entitlement%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%anticheat%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%crypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%encrypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%decrypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signature%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signed%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signing%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign %'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%hash%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%exec%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%eval%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%command%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%file%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%registry%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%process%'"
		"          THEN 1 ELSE 0 END AS sensitivity"
		" FROM symbols s"
		" JOIN monolith_source_crg_refresh_symbols rs ON rs.id = s.id"
		"), scored AS ("
		" SELECT c.*, MIN(1.0,"
		"        MIN(c.fan_in,50) / 50.0 * 0.35 +"
		"        MIN(c.descendants,30) / 30.0 * 0.25 +"
		"        MIN(c.fan_out,50) / 50.0 * 0.10 +"
		"        CASE WHEN c.is_ue_macro != 0 THEN 0.15 ELSE 0.0 END +"
		"        MIN(c.caller_files,20) / 20.0 * 0.15 +"
		"        CASE WHEN c.sensitivity != 0 THEN 0.15 ELSE 0.0 END) AS score"
		" FROM counts c"
		") "
		"INSERT OR REPLACE INTO crg_node_metrics(node_id,fan_in,fan_out,hard_in,descendants,risk_score,risk_tier,reasons_json,raw_counts_json,scoring_version,computed_at) "
		"SELECT s.native_id,s.fan_in,s.fan_out,0,s.descendants,ROUND(s.score,3),"
		"       CASE WHEN s.score >= 0.66 THEN 'high' WHEN s.score >= 0.33 THEN 'medium' ELSE 'low' END,"
		"       CASE WHEN s.sensitivity != 0 THEN"
		"         printf('[\"caller fan-in: %d\",\"inheritance descendants (1-hop): %d\",\"callee fan-out: %d\",\"module/file boundary crossing: %d distinct caller file(s)\",\"sensitivity: UE-domain sensitive surface (token-boundary matched)\"]',"
		"                s.fan_in,s.descendants,s.fan_out,s.caller_files)"
		"       ELSE"
		"         printf('[\"caller fan-in: %d\",\"inheritance descendants (1-hop): %d\",\"callee fan-out: %d\",\"module/file boundary crossing: %d distinct caller file(s)\"]',"
		"                s.fan_in,s.descendants,s.fan_out,s.caller_files)"
		"       END,"
		"       printf('{\"callers\":%d,\"callees\":%d,\"descendants\":%d,\"ancestors\":%d,\"caller_files\":%d,\"is_ue_macro\":%d,\"sensitivity\":%d}',"
		"              s.fan_in,s.fan_out,s.descendants,s.ancestors,s.caller_files,s.is_ue_macro,s.sensitivity),"
		"       '3',CAST(strftime('%s','now') AS INTEGER) "
		"FROM scored s;"), TEXT("insert scoped metrics"));

	Exec(TEXT(
		"DELETE FROM source_override_edges "
		"WHERE child_symbol_id IN (SELECT id FROM monolith_source_override_refresh_children) "
		"   OR parent_symbol_id IN (SELECT id FROM monolith_source_crg_seed_symbols);"), TEXT("delete scoped source override edges"));
	int64 OverrideEdgeCount = 0;
	if (bOk)
	{
		FString OverrideError;
		if (!PopulateSourceOverrideEdgeCacheLocked(*Database, TEXT("monolith_source_override_refresh_children"), OverrideEdgeCount, OverrideError))
		{
			bOk = false;
			Warnings.Add(MakeShared<FJsonValueString>(
				OverrideError.IsEmpty()
					? TEXT("Scoped source CRG refresh failed at source override edge cache")
					: FString::Printf(TEXT("Scoped source CRG refresh failed at source override edge cache: %s"), *OverrideError)));
		}
	}
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('cache_version','1');"), TEXT("cache_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('scoring_version','3');"), TEXT("scoring_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_override_edges_version','2');"), TEXT("source_override_edges_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_last_scoped_refresh_at',datetime('now'));"), TEXT("source_last_scoped_refresh_at"));
	Exec(TEXT("DELETE FROM monolith_source_crg_pending_ids;"), TEXT("clear pending prune ids"));

	if (bOk) Database->Execute(TEXT("COMMIT;"));
	else Database->Execute(TEXT("ROLLBACK;"));

	TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("file_ids"), static_cast<double>(FileCount));
	Counts->SetNumberField(TEXT("node_symbols"), static_cast<double>(NodeSymbolCount));
	Counts->SetNumberField(TEXT("affected_symbols"), static_cast<double>(RefreshSymbolCount));
	Counts->SetNumberField(TEXT("old_nodes_replaced"), static_cast<double>(OldNodeCount));
	Counts->SetNumberField(TEXT("reference_edges_refreshed"), static_cast<double>(ReferenceEdgeCount));
	Counts->SetNumberField(TEXT("inheritance_edges_refreshed"), static_cast<double>(InheritanceEdgeCount));
	Counts->SetNumberField(TEXT("source_override_child_seed"), static_cast<double>(OverrideChildSeedCount));
	Counts->SetNumberField(TEXT("source_override_edges_refreshed"), static_cast<double>(OverrideEdgeCount));
	Counts->SetNumberField(TEXT("source_override_edges"), static_cast<double>(
		CountSql(TEXT("SELECT COUNT(*) FROM source_override_edges;"))));
	Counts->SetNumberField(TEXT("crg_nodes"), static_cast<double>(
		CountSql(TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain='source';"))));
	Counts->SetNumberField(TEXT("crg_edges"), static_cast<double>(
		CountSql(TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain='source';"))));
	Counts->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(
		CountSql(TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain='source';"))));
	Root->SetObjectField(TEXT("counts"), Counts);
	Root->SetStringField(TEXT("refresh_mode"), TEXT("scoped_files"));
	Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
	FString FailureDetail;
	if (!bOk && Warnings.Num() > 0)
	{
		Warnings[0]->TryGetString(FailureDetail);
	}
	Root->SetStringField(TEXT("summary"), bOk
		? FString::Printf(TEXT("Scoped source CRG projection/cache refreshed for %lld file(s), %lld affected symbol(s)"), FileCount, RefreshSymbolCount)
		: (FailureDetail.IsEmpty()
			? TEXT("Scoped source CRG projection/cache refresh failed; rolled back")
			: FString::Printf(TEXT("Scoped source CRG projection/cache refresh failed; rolled back: %s"), *FailureDetail)));
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNextActions(Root, { TEXT("source.health"), TEXT("source.risk_score"), TEXT("source.review_context") });
	return Root;
}

// ============================================================
// CRG-inspired health / repair
//
// Adapted from code-review-graph (0919071a): non-fatal health post-processing
// and FTS rebuild. Engine-source-domain native: only the existing
// modules/files/symbols/inheritance/"references"/symbols_fts/source_fts schema.
// ============================================================

TSharedPtr<FJsonObject> FMonolithSourceDatabase::ComputeHealth(bool bIncludeCounts, bool bIncludeDeepChecks)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Warnings;
	const bool bRunExpensiveChecks = bIncludeCounts || bIncludeDeepChecks;
	bool bNeedsReindex = false;
	bool bNeedsFtsRepair = false;
	bool bNeedsCrgRepair = false;
	bool bNeedsOverrideRepair = false;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetBoolField(TEXT("include_counts"), bIncludeCounts);
	Input->SetBoolField(TEXT("include_deep_checks"), bIncludeDeepChecks);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetBoolField(TEXT("include_counts"), bIncludeCounts);
	Limits->SetBoolField(TEXT("include_deep_checks"), bIncludeDeepChecks);
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
	auto Info = [&](const FString& Name, const FString& Detail)
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("check"), Name);
		C->SetStringField(TEXT("result"), TEXT("info"));
		C->SetStringField(TEXT("detail"), Detail);
		Checks.Add(MakeShared<FJsonValueObject>(C));
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
		if (!bHas)
		{
			bNeedsReindex = true;
		}
		Check(FString::Printf(TEXT("table:%s"), T), bHas,
			bHas ? FString::Printf(TEXT("table %s present"), T)
				: FString::Printf(TEXT("missing table %s"), T));
	}

	for (const TCHAR* F : { TEXT("symbols_fts"), TEXT("source_fts") })
	{
		const bool bHas = Exists(TEXT("table"), F);
		if (!bHas)
		{
			if (FCString::Strcmp(F, TEXT("symbols_fts")) == 0)
			{
				bNeedsFtsRepair = true;
			}
			else
			{
				bNeedsReindex = true;
			}
		}
		Check(FString::Printf(TEXT("fts:%s"), F), bHas,
			bHas ? FString::Printf(TEXT("FTS table %s present"), F)
				: FString::Printf(TEXT("missing FTS table %s"), F));
	}

	// Source has exactly symbols_ai / symbols_ad (no _au, no source_fts trigger).
	for (const TCHAR* Tr : { TEXT("symbols_ai"), TEXT("symbols_ad") })
	{
		const bool bHas = Exists(TEXT("trigger"), Tr);
		if (!bHas)
		{
			bNeedsFtsRepair = true;
		}
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
	if (SchemaVer != TEXT("1"))
	{
		bNeedsReindex = true;
	}
	Check(TEXT("meta:schema_version"), SchemaVer == TEXT("1"),
		SchemaVer.IsEmpty() ? TEXT("meta.schema_version missing")
			: FString::Printf(TEXT("schema_version=%s (expected 1)"), *SchemaVer));

	int64 OrphanRefs = -1;
	int64 SymCnt = -1;
	int64 SymFtsCnt = -1;
	if (bRunExpensiveChecks)
	{
		const int64 OrphanSymbols = CountOf(TEXT(
			"SELECT COUNT(*) FROM symbols s "
			"LEFT JOIN files f ON f.id = s.file_id "
			"WHERE f.id IS NULL;"));
		if (OrphanSymbols != 0)
		{
			bNeedsReindex = true;
		}
		Check(TEXT("integrity:orphan_symbols"), OrphanSymbols == 0,
			OrphanSymbols == 0 ? TEXT("no orphan symbol rows")
				: FString::Printf(TEXT("%lld orphan symbol row(s); run project/source reindex so prune can remove invalid dependent rows"), OrphanSymbols));

		OrphanRefs = CountOf(TEXT(
			"SELECT COUNT(*) FROM \"references\" r "
			"WHERE (r.from_symbol_id != 0 AND r.from_symbol_id NOT IN (SELECT id FROM symbols)) "
			"   OR r.to_symbol_id NOT IN (SELECT id FROM symbols);"));
		if (OrphanRefs != 0)
		{
			bNeedsReindex = true;
		}
		Check(TEXT("integrity:orphan_references"), OrphanRefs == 0,
			OrphanRefs == 0 ? TEXT("no orphan reference rows")
				: FString::Printf(TEXT("%lld orphan reference row(s); rebuild source index rather than repeatedly repairing CRG cache"), OrphanRefs));

		SymCnt = CountOf(TEXT("SELECT COUNT(*) FROM symbols;"));
		SymFtsCnt = CountOf(TEXT("SELECT COUNT(*) FROM symbols_fts;"));
		if (SymCnt != SymFtsCnt)
		{
			bNeedsFtsRepair = true;
		}
		Check(TEXT("fts:symbols_row_parity"), SymCnt == SymFtsCnt,
			FString::Printf(TEXT("symbols=%lld symbols_fts=%lld%s"), SymCnt, SymFtsCnt,
				SymCnt == SymFtsCnt ? TEXT("") : TEXT(" (mismatch -> source.repair_fts target=symbols)")));
	}
	else
	{
		Info(TEXT("integrity:orphan_symbols"), TEXT("skipped; pass include_deep_checks=true or include_counts=true"));
		Info(TEXT("integrity:orphan_references"), TEXT("skipped; pass include_deep_checks=true or include_counts=true"));
		Info(TEXT("fts:symbols_row_parity"), TEXT("skipped; pass include_deep_checks=true or include_counts=true"));
	}

	bool bHasCoreCrg = true;
	for (const TCHAR* T : { TEXT("crg_nodes"), TEXT("crg_edges"), TEXT("crg_node_metrics"), TEXT("crg_meta") })
	{
		const bool bHas = Exists(TEXT("table"), T);
		bHasCoreCrg = bHasCoreCrg && bHas;
		if (!bHas)
		{
			bNeedsCrgRepair = true;
		}
		Check(FString::Printf(TEXT("crg:table:%s"), T), bHas,
			bHas ? FString::Printf(TEXT("CRG projection table %s present"), T)
				: FString::Printf(TEXT("missing CRG projection table %s (run source.repair_crg_cache)"), T));
	}
	const bool bHasSourceOverrideEdges = Exists(TEXT("table"), TEXT("source_override_edges"));
	if (!bHasSourceOverrideEdges)
	{
		bNeedsOverrideRepair = true;
	}
	Check(TEXT("crg:table:source_override_edges"), bHasSourceOverrideEdges,
		bHasSourceOverrideEdges ? TEXT("CRG projection table source_override_edges present")
			: TEXT("missing CRG projection table source_override_edges (run source.repair_crg_cache)"));

	for (const TCHAR* I : {
		TEXT("idx_crg_nodes_domain_native"), TEXT("idx_crg_nodes_stable"),
		TEXT("idx_crg_edges_domain_source"), TEXT("idx_crg_edges_domain_target"),
		TEXT("idx_crg_edges_kind_subkind"), TEXT("idx_crg_metrics_score") })
	{
		const bool bHas = Exists(TEXT("index"), I);
		if (!bHas)
		{
			bNeedsCrgRepair = true;
		}
		Check(FString::Printf(TEXT("crg:index:%s"), I), bHas,
			bHas ? FString::Printf(TEXT("CRG projection index %s present"), I)
				: FString::Printf(TEXT("missing CRG projection index %s (run source.repair_crg_cache)"), I));
	}
	for (const TCHAR* I : {
		TEXT("idx_source_override_edges_parent"),
		TEXT("idx_symbols_override_signature"),
		TEXT("idx_inheritance_parent_child"), TEXT("idx_inheritance_child_parent") })
	{
		const bool bHas = Exists(TEXT("index"), I);
		if (!bHas)
		{
			bNeedsOverrideRepair = true;
		}
		Check(FString::Printf(TEXT("crg:index:%s"), I), bHas,
			bHas ? FString::Printf(TEXT("CRG projection index %s present"), I)
				: FString::Printf(TEXT("missing CRG projection index %s (run source.repair_crg_cache)"), I));
	}
	int64 CrgNodeCnt = -1;
	int64 CrgEdgeCnt = -1;
	int64 CrgMetricCnt = -1;
	if (bHasCoreCrg && bRunExpensiveChecks)
	{
		const int64 ValidRefCnt = CountOf(TEXT(
			"SELECT COUNT(*) FROM \"references\" r "
			"JOIN symbols fs ON fs.id = r.from_symbol_id "
			"JOIN symbols ts ON ts.id = r.to_symbol_id;"));
		const int64 InhCnt = CountOf(TEXT("SELECT COUNT(*) FROM inheritance;"));
		CrgNodeCnt = CountOf(TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'source';"));
		CrgEdgeCnt = CountOf(TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'source';"));
		CrgMetricCnt = CountOf(TEXT(
			"SELECT COUNT(*) FROM crg_node_metrics m "
			"JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'source';"));
		if (CrgNodeCnt != SymCnt || CrgEdgeCnt != ValidRefCnt + InhCnt || CrgMetricCnt != CrgNodeCnt)
		{
			bNeedsCrgRepair = true;
		}
		Check(TEXT("crg:nodes_row_parity"), CrgNodeCnt == SymCnt,
			FString::Printf(TEXT("symbols=%lld crg_nodes(source)=%lld%s"), SymCnt, CrgNodeCnt,
				CrgNodeCnt == SymCnt ? TEXT("") : TEXT(" (mismatch -> source.repair_crg_cache)")));
		Check(TEXT("crg:edges_row_parity"), CrgEdgeCnt == ValidRefCnt + InhCnt,
			FString::Printf(TEXT("valid references+inheritance=%lld crg_edges(source)=%lld%s"), ValidRefCnt + InhCnt, CrgEdgeCnt,
				CrgEdgeCnt == ValidRefCnt + InhCnt ? TEXT("") : TEXT(" (mismatch -> source.repair_crg_cache)")));
		Check(TEXT("crg:metrics_row_parity"), CrgMetricCnt == CrgNodeCnt,
			FString::Printf(TEXT("crg_nodes(source)=%lld crg_node_metrics=%lld%s"), CrgNodeCnt, CrgMetricCnt,
				CrgMetricCnt == CrgNodeCnt ? TEXT("") : TEXT(" (mismatch -> source.repair_crg_cache)")));
		const int64 OrphanCrgEdges = CountOf(TEXT(
			"SELECT COUNT(*) FROM crg_edges e "
			"WHERE e.domain = 'source' AND ("
			" e.source_node_id NOT IN (SELECT id FROM crg_nodes) "
			" OR e.target_node_id NOT IN (SELECT id FROM crg_nodes));"));
		if (OrphanCrgEdges != 0)
		{
			bNeedsCrgRepair = true;
		}
		Check(TEXT("crg:orphan_edges"), OrphanCrgEdges == 0,
			OrphanCrgEdges == 0 ? TEXT("no orphan CRG projection edge rows")
				: FString::Printf(TEXT("%lld orphan CRG projection edge row(s)"), OrphanCrgEdges));
		FString CacheVersion;
		FSQLitePreparedStatement S;
		if (S.Create(*Database, TEXT("SELECT value FROM crg_meta WHERE key = 'cache_version';"))
			&& S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			S.GetColumnValueByIndex(0, CacheVersion);
		}
		if (CacheVersion.IsEmpty())
		{
			bNeedsCrgRepair = true;
		}
		Check(TEXT("crg:cache_version"), !CacheVersion.IsEmpty(),
			CacheVersion.IsEmpty() ? TEXT("crg_meta.cache_version missing (run source.repair_crg_cache)")
				: FString::Printf(TEXT("crg cache_version=%s"), *CacheVersion));
		FString CrgScoringVersion;
		FSQLitePreparedStatement S2;
		if (S2.Create(*Database, TEXT("SELECT value FROM crg_meta WHERE key = 'scoring_version';"))
			&& S2.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			S2.GetColumnValueByIndex(0, CrgScoringVersion);
		}
		if (CrgScoringVersion != TEXT("3"))
		{
			bNeedsCrgRepair = true;
		}
		Check(TEXT("crg:scoring_version"), CrgScoringVersion == TEXT("3"),
			CrgScoringVersion.IsEmpty() ? TEXT("crg_meta.scoring_version missing (run source.repair_crg_cache)")
				: FString::Printf(TEXT("crg scoring_version=%s (expected 3)"), *CrgScoringVersion));
	}
	else if (bHasCoreCrg)
	{
		Info(TEXT("crg:row_parity"), TEXT("skipped; pass include_deep_checks=true or include_counts=true"));
	}
	if (bHasSourceOverrideEdges && Exists(TEXT("table"), TEXT("crg_meta")) && bRunExpensiveChecks)
	{
		FString OverrideEdgesVersion;
		FSQLitePreparedStatement S3;
		if (S3.Create(*Database, TEXT("SELECT value FROM crg_meta WHERE key = 'source_override_edges_version';"))
			&& S3.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			S3.GetColumnValueByIndex(0, OverrideEdgesVersion);
		}
		if (OverrideEdgesVersion != TEXT("2"))
		{
			bNeedsOverrideRepair = true;
		}
		Check(TEXT("crg:source_override_edges_version"), OverrideEdgesVersion == TEXT("2"),
			OverrideEdgesVersion.IsEmpty() ? TEXT("crg_meta.source_override_edges_version missing (run source.repair_crg_cache)")
				: FString::Printf(TEXT("source override edge cache version=%s (expected 2)"), *OverrideEdgesVersion));
	}
	else if (bHasSourceOverrideEdges)
	{
		Info(TEXT("crg:source_override_edges_version"), TEXT("skipped; pass include_deep_checks=true or include_counts=true"));
	}

	// source_fts is a plain (non external-content) fts5 table — a row-count
	// difference is expected and informational, never a warning.
	const int64 SrcFtsCnt = bRunExpensiveChecks
		? CountOf(TEXT("SELECT COUNT(*) FROM source_fts;"))
		: -1;
	{
		const FString Detail = bRunExpensiveChecks
			? FString::Printf(TEXT("source_fts rows=%lld (plain fts5; not rebuildable — reindex to repair)"), SrcFtsCnt)
			: TEXT("source_fts row count skipped; pass include_deep_checks=true or include_counts=true");
		Info(TEXT("fts:source_fts_info"), Detail);
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

	if (Journal.Equals(TEXT("wal"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> Wal = MakeShared<FJsonObject>();
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString WalPath = CachedDbPath + TEXT("-wal");
		const FString ShmPath = CachedDbPath + TEXT("-shm");
		Wal->SetNumberField(TEXT("wal_bytes"), static_cast<double>(FMath::Max<int64>(0, PlatformFile.FileSize(*WalPath))));
		Wal->SetNumberField(TEXT("shm_bytes"), static_cast<double>(FMath::Max<int64>(0, PlatformFile.FileSize(*ShmPath))));

		FSQLitePreparedStatement S;
		if (S.Create(*Database, TEXT("PRAGMA wal_checkpoint(NOOP);"))
			&& S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Busy = 0;
			int64 Log = 0;
			int64 Checkpointed = 0;
			S.GetColumnValueByIndex(0, Busy);
			S.GetColumnValueByIndex(1, Log);
			S.GetColumnValueByIndex(2, Checkpointed);
			Wal->SetNumberField(TEXT("checkpoint_busy"), static_cast<double>(Busy));
			Wal->SetNumberField(TEXT("log_frames"), static_cast<double>(Log));
			Wal->SetNumberField(TEXT("checkpointed_frames"), static_cast<double>(Checkpointed));
		}
		Root->SetObjectField(TEXT("wal"), Wal);
	}

	if (bIncludeCounts)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("symbols"), static_cast<double>(SymCnt));
		Counts->SetNumberField(TEXT("references"),
			static_cast<double>(CountOf(TEXT("SELECT COUNT(*) FROM \"references\";"))));
		Counts->SetNumberField(TEXT("inheritance"),
			static_cast<double>(CountOf(TEXT("SELECT COUNT(*) FROM inheritance;"))));
		Counts->SetNumberField(TEXT("source_fts"), static_cast<double>(SrcFtsCnt));
		if (bHasCoreCrg)
		{
			Counts->SetNumberField(TEXT("crg_nodes"), static_cast<double>(CrgNodeCnt));
			Counts->SetNumberField(TEXT("crg_edges"), static_cast<double>(CrgEdgeCnt));
			Counts->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(CrgMetricCnt));
		}
		if (bHasSourceOverrideEdges)
		{
			Counts->SetNumberField(TEXT("source_override_edges"),
				static_cast<double>(CountOf(TEXT("SELECT COUNT(*) FROM source_override_edges;"))));
		}
		Root->SetObjectField(TEXT("row_counts"), Counts);
	}

	const bool bHealthy = Warnings.Num() == 0;
	Root->SetStringField(TEXT("check_depth"), bRunExpensiveChecks ? TEXT("deep") : TEXT("shallow"));
	Root->SetStringField(TEXT("status"), bHealthy ? TEXT("ok") : TEXT("warning"));
	Root->SetStringField(TEXT("summary"), bHealthy
		? (bRunExpensiveChecks
			? TEXT("EngineSource schema, triggers, symbols_fts parity and integrity OK")
			: TEXT("EngineSource schema, required tables/triggers, and CRG structure OK; deep parity checks skipped"))
		: FString::Printf(TEXT("%d health warning(s)"), Warnings.Num()));
	Root->SetArrayField(TEXT("checks"), Checks);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	TArray<FString> NextActions;
	auto AddNextUnique = [&NextActions](const FString& Action)
	{
		if (!NextActions.Contains(Action))
		{
			NextActions.Add(Action);
		}
	};
	if (!bRunExpensiveChecks)
	{
		AddNextUnique(TEXT("source.health include_deep_checks=true"));
	}
	if (bNeedsCrgRepair)
	{
		AddNextUnique(TEXT("source.repair_crg_cache"));
	}
	if (bNeedsOverrideRepair)
	{
		AddNextUnique(TEXT("source.repair_crg_cache scope=override_edges"));
	}
	if (bNeedsFtsRepair)
	{
		AddNextUnique(TEXT("source.repair_fts target=symbols"));
	}
	if (bNeedsReindex)
	{
		AddNextUnique(TEXT("source.trigger_project_reindex"));
		AddNextUnique(TEXT("source.trigger_reindex"));
	}
	if (bHealthy)
	{
		AddNextUnique(TEXT("source.search_source"));
		AddNextUnique(TEXT("source.review_context"));
		AddNextUnique(TEXT("source.risk_score"));
	}
	else
	{
		AddNextUnique(TEXT("source.health"));
		AddNextUnique(TEXT("source.search_source"));
	}
	AddNextActions(Root, NextActions);
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
		if (bOk)
		{
			Database->Execute(TEXT("COMMIT;"));
		}
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

TSharedPtr<FJsonObject> FMonolithSourceDatabase::RepairCrgCache(bool bExecute)
{
	return RepairCrgCache(TEXT("all"), bExecute);
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::RepairCrgCache(const FString& Scope, bool bExecute)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString NormalizedScope = Scope.IsEmpty() ? TEXT("all") : Scope.ToLower();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetBoolField(TEXT("execute"), bExecute);
	Input->SetStringField(TEXT("scope"), NormalizedScope);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetBoolField(TEXT("execute"), bExecute);
	Limits->SetStringField(TEXT("scope"), NormalizedScope);
	Root->SetObjectField(TEXT("limits"), Limits);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Plan;
	if (NormalizedScope == TEXT("override_edges"))
	{
		Plan.Add(MakeShared<FJsonValueString>(TEXT("CREATE IF MISSING source_override_edges and helper indexes")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("DELETE existing source_override_edges rows")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("Recompute signature-aware source_override_edges cache")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("Update crg_meta.source_override_edges_version")));
	}
	else
	{
		Plan.Add(MakeShared<FJsonValueString>(TEXT("CREATE IF MISSING crg_nodes/crg_edges/crg_node_metrics/crg_meta")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("CREATE IF MISSING review/override helper indexes")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("DELETE existing source CRG projection rows")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("SOURCE symbols -> crg_nodes; references/inheritance -> crg_edges")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("Recompute caller/callee/descendant/risk_score into crg_node_metrics")));
		Plan.Add(MakeShared<FJsonValueString>(TEXT("Recompute signature-aware source_override_edges cache")));
	}
	Root->SetArrayField(TEXT("plan"), Plan);

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}
	if (NormalizedScope != TEXT("all") && NormalizedScope != TEXT("override_edges"))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Unsupported scope for source.repair_crg_cache (expected all|override_edges)"));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.repair_crg_cache scope=all"), TEXT("source.repair_crg_cache scope=override_edges"), TEXT("source.health") });
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
	auto Count = [&](const TCHAR* Sql) -> int64
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, Sql)) return -1;
		int64 N = 0;
		if (S.Step() == ESQLitePreparedStatementStepResult::Row) S.GetColumnValueByIndex(0, N);
		return N;
	};
	const bool bHadCrg = Exists(TEXT("table"), TEXT("crg_nodes"))
		&& Exists(TEXT("table"), TEXT("crg_edges"))
		&& Exists(TEXT("table"), TEXT("crg_node_metrics"))
		&& Exists(TEXT("table"), TEXT("crg_meta"));

	TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetNumberField(TEXT("symbols"), static_cast<double>(Count(TEXT("SELECT COUNT(*) FROM symbols;"))));
	Before->SetNumberField(TEXT("references"), static_cast<double>(Count(TEXT("SELECT COUNT(*) FROM \"references\";"))));
	Before->SetNumberField(TEXT("inheritance"), static_cast<double>(Count(TEXT("SELECT COUNT(*) FROM inheritance;"))));
	if (bHadCrg)
	{
		Before->SetNumberField(TEXT("crg_nodes"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'source';"))));
		Before->SetNumberField(TEXT("crg_edges"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'source';"))));
		Before->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'source';"))));
		if (Exists(TEXT("table"), TEXT("source_override_edges")))
		{
			Before->SetNumberField(TEXT("source_override_edges"), static_cast<double>(
				Count(TEXT("SELECT COUNT(*) FROM source_override_edges;"))));
		}
	}
	Root->SetObjectField(TEXT("before"), Before);

	TArray<TSharedPtr<FJsonValue>> FreshnessChecks;
	bool bRepairNeeded = false;
	auto AddFreshnessCheck = [&](const FString& Name, bool bPass, const FString& Detail)
	{
		TSharedPtr<FJsonObject> CheckObj = MakeShared<FJsonObject>();
		CheckObj->SetStringField(TEXT("check"), Name);
		CheckObj->SetStringField(TEXT("result"), bPass ? TEXT("ok") : TEXT("stale"));
		CheckObj->SetStringField(TEXT("detail"), Detail);
		FreshnessChecks.Add(MakeShared<FJsonValueObject>(CheckObj));
		if (!bPass)
		{
			bRepairNeeded = true;
		}
	};
	auto MetaValue = [&](const TCHAR* Key) -> FString
	{
		FString Value;
		if (!Exists(TEXT("table"), TEXT("crg_meta")))
		{
			return Value;
		}
		FSQLitePreparedStatement S;
		if (S.Create(*Database, TEXT("SELECT value FROM crg_meta WHERE key = ?;")))
		{
			S.SetBindingValueByIndex(1, FString(Key));
			if (S.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				S.GetColumnValueByIndex(0, Value);
			}
		}
		return Value;
	};

	if (NormalizedScope == TEXT("override_edges"))
	{
		const bool bHasOverrideTable = Exists(TEXT("table"), TEXT("source_override_edges"));
		AddFreshnessCheck(TEXT("table:source_override_edges"), bHasOverrideTable,
			bHasOverrideTable ? TEXT("source_override_edges table present")
				: TEXT("source_override_edges table missing"));
		const bool bHasParentIndex = Exists(TEXT("index"), TEXT("idx_source_override_edges_parent"));
		AddFreshnessCheck(TEXT("index:idx_source_override_edges_parent"), bHasParentIndex,
			bHasParentIndex ? TEXT("source override parent index present")
				: TEXT("source override parent index missing"));
		const bool bHasSignatureIndex = Exists(TEXT("index"), TEXT("idx_symbols_override_signature"));
		AddFreshnessCheck(TEXT("index:idx_symbols_override_signature"), bHasSignatureIndex,
			bHasSignatureIndex ? TEXT("symbol override signature index present")
				: TEXT("symbol override signature index missing"));
		const FString OverrideVersion = MetaValue(TEXT("source_override_edges_version"));
		AddFreshnessCheck(TEXT("meta:source_override_edges_version"), OverrideVersion == TEXT("2"),
			OverrideVersion.IsEmpty() ? TEXT("source_override_edges_version missing")
				: FString::Printf(TEXT("source_override_edges_version=%s (expected 2)"), *OverrideVersion));
	}
	else
	{
		AddFreshnessCheck(TEXT("tables:crg_core"), bHadCrg,
			bHadCrg ? TEXT("core CRG projection tables present")
				: TEXT("one or more core CRG projection tables are missing"));
		if (bHadCrg)
		{
			for (const TCHAR* I : {
				TEXT("idx_crg_nodes_domain_native"), TEXT("idx_crg_nodes_stable"),
				TEXT("idx_crg_edges_domain_source"), TEXT("idx_crg_edges_domain_target"),
				TEXT("idx_crg_edges_kind_subkind"), TEXT("idx_crg_metrics_score"),
				TEXT("idx_source_override_edges_parent"),
				TEXT("idx_symbols_override_signature"),
				TEXT("idx_inheritance_parent_child"), TEXT("idx_inheritance_child_parent") })
			{
				const bool bHasIndex = Exists(TEXT("index"), I);
				AddFreshnessCheck(FString::Printf(TEXT("index:%s"), I), bHasIndex,
					bHasIndex ? FString::Printf(TEXT("index %s present"), I)
						: FString::Printf(TEXT("index %s missing"), I));
			}
			const int64 FreshValidRefCnt = Count(TEXT(
				"SELECT COUNT(*) FROM \"references\" r "
				"JOIN symbols fs ON fs.id = r.from_symbol_id "
				"JOIN symbols ts ON ts.id = r.to_symbol_id;"));
			const int64 FreshInhCnt = Count(TEXT("SELECT COUNT(*) FROM inheritance;"));
			const int64 FreshSymCnt = Count(TEXT("SELECT COUNT(*) FROM symbols;"));
			const int64 FreshCrgNodeCnt = Count(TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'source';"));
			const int64 FreshCrgEdgeCnt = Count(TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'source';"));
			const int64 FreshCrgMetricCnt = Count(TEXT(
				"SELECT COUNT(*) FROM crg_node_metrics m "
				"JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'source';"));
			AddFreshnessCheck(TEXT("crg:nodes_row_parity"), FreshCrgNodeCnt == FreshSymCnt,
				FString::Printf(TEXT("symbols=%lld crg_nodes(source)=%lld"), FreshSymCnt, FreshCrgNodeCnt));
			AddFreshnessCheck(TEXT("crg:edges_row_parity"), FreshCrgEdgeCnt == FreshValidRefCnt + FreshInhCnt,
				FString::Printf(TEXT("valid references+inheritance=%lld crg_edges(source)=%lld"), FreshValidRefCnt + FreshInhCnt, FreshCrgEdgeCnt));
			AddFreshnessCheck(TEXT("crg:metrics_row_parity"), FreshCrgMetricCnt == FreshCrgNodeCnt,
				FString::Printf(TEXT("crg_nodes(source)=%lld crg_node_metrics=%lld"), FreshCrgNodeCnt, FreshCrgMetricCnt));
			const FString CacheVersion = MetaValue(TEXT("cache_version"));
			AddFreshnessCheck(TEXT("meta:cache_version"), !CacheVersion.IsEmpty(),
				CacheVersion.IsEmpty() ? TEXT("cache_version missing")
					: FString::Printf(TEXT("cache_version=%s"), *CacheVersion));
			const FString ScoringVersion = MetaValue(TEXT("scoring_version"));
			AddFreshnessCheck(TEXT("meta:scoring_version"), ScoringVersion == TEXT("3"),
				ScoringVersion.IsEmpty() ? TEXT("scoring_version missing")
					: FString::Printf(TEXT("scoring_version=%s (expected 3)"), *ScoringVersion));
			const FString OverrideVersion = MetaValue(TEXT("source_override_edges_version"));
			AddFreshnessCheck(TEXT("meta:source_override_edges_version"), OverrideVersion == TEXT("2"),
				OverrideVersion.IsEmpty() ? TEXT("source_override_edges_version missing")
					: FString::Printf(TEXT("source_override_edges_version=%s (expected 2)"), *OverrideVersion));
		}
	}
	Root->SetArrayField(TEXT("freshness_checks"), FreshnessChecks);
	Root->SetBoolField(TEXT("repair_needed"), bRepairNeeded);

	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"),
			bRepairNeeded
				? (NormalizedScope == TEXT("override_edges")
					? TEXT("Dry-run: source override edge cache is stale and would be rebuilt. Pass execute=true to apply.")
					: TEXT("Dry-run: source CRG projection/cache is stale and would be rebuilt. Pass execute=true to apply."))
				: TEXT("Dry-run: source CRG projection/cache is already fresh; execute=true would skip the rebuild."));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		if (bRepairNeeded)
		{
			AddNextActions(Root, { TEXT("source.repair_crg_cache execute=true"), TEXT("source.repair_crg_cache scope=override_edges execute=true"), TEXT("source.health include_deep_checks=true"), TEXT("source.risk_score") });
		}
		else
		{
			AddNextActions(Root, { TEXT("source.health"), TEXT("source.risk_score"), TEXT("source.review_context") });
		}
		return Root;
	}

	if (!bRepairNeeded)
	{
		Root->SetObjectField(TEXT("after"), Before);
		Root->SetBoolField(TEXT("skipped"), true);
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), TEXT("Source CRG projection/cache already fresh; skipped rebuild."));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.health"), TEXT("source.risk_score"), TEXT("source.review_context") });
		return Root;
	}

	bool bOk = ExecuteMulti(*Database, GCrgProjectionDdl);
	if (bOk)
	{
		bOk = ExecuteMulti(*Database, GSourceReviewIndexDdl);
	}
	auto Exec = [&](const TCHAR* Sql, const TCHAR* Label)
	{
		if (!bOk) return;
		if (!Database->Execute(Sql))
		{
			bOk = false;
			Warnings.Add(MakeShared<FJsonValueString>(
				FString::Printf(TEXT("CRG cache rebuild failed at %s"), Label)));
		}
	};

	if (NormalizedScope == TEXT("override_edges"))
	{
		if (bOk)
		{
			bOk = Database->Execute(TEXT("BEGIN;"));
		}
		Exec(TEXT("DELETE FROM source_override_edges;"), TEXT("clear source override edges"));

		int64 OverrideEdgeCount = 0;
		if (bOk)
		{
			FString OverrideError;
			if (!PopulateSourceOverrideEdgeCacheLocked(*Database, OverrideEdgeCount, OverrideError))
			{
				bOk = false;
				Warnings.Add(MakeShared<FJsonValueString>(
					OverrideError.IsEmpty()
						? TEXT("CRG cache rebuild failed at source override edge cache")
						: FString::Printf(TEXT("CRG cache rebuild failed at source override edge cache: %s"), *OverrideError)));
			}
		}
		Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_override_edges_version','2');"), TEXT("source_override_edges_version"));
		if (bOk)
		{
			Root->SetNumberField(TEXT("source_override_edges_built"), static_cast<double>(OverrideEdgeCount));
			Database->Execute(TEXT("COMMIT;"));
		}
		else
		{
			Database->Execute(TEXT("ROLLBACK;"));
		}

		TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
		if (Exists(TEXT("table"), TEXT("source_override_edges")))
		{
			After->SetNumberField(TEXT("source_override_edges"), static_cast<double>(
				Count(TEXT("SELECT COUNT(*) FROM source_override_edges;"))));
		}
		Root->SetObjectField(TEXT("after"), After);
		Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
		Root->SetStringField(TEXT("summary"), bOk
			? TEXT("Rebuilt source override edge cache")
			: TEXT("source override edge cache rebuild failed; rolled back"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.health"), TEXT("source.find_overrides"), TEXT("source.review_hotspots kind=override") });
		return Root;
	}

	if (bOk)
	{
		bOk = Database->Execute(TEXT("BEGIN;"));
	}
	Exec(TEXT("DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM crg_nodes WHERE domain = 'source');"), TEXT("clear metrics"));
	Exec(TEXT("DELETE FROM crg_edges WHERE domain = 'source';"), TEXT("clear edges"));
	Exec(TEXT("DELETE FROM crg_nodes WHERE domain = 'source';"), TEXT("clear nodes"));
	Exec(TEXT("DELETE FROM source_override_edges;"), TEXT("clear source override edges"));
	Exec(TEXT(
		"INSERT INTO crg_nodes(id,domain,native_table,native_id,stable_key,kind,name,path,module,source_revision,extra,updated_at) "
		"SELECT s.id,'source','symbols',s.id,COALESCE(s.qualified_name,s.name) || '#' || s.id,"
		"s.kind,s.name,COALESCE(f.path,''),COALESCE(m.name,''),'','{}',CAST(strftime('%s','now') AS INTEGER) "
		"FROM symbols s "
		"LEFT JOIN files f ON f.id = s.file_id "
		"LEFT JOIN modules m ON m.id = f.module_id;"), TEXT("source nodes"));
	Exec(TEXT(
		"INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at) "
		"SELECT 'source',r.from_symbol_id,r.to_symbol_id,COALESCE(r.ref_kind,'reference'),'reference',1.0,'references',r.id,CAST(strftime('%s','now') AS INTEGER) "
		"FROM \"references\" r "
		"JOIN symbols fs ON fs.id = r.from_symbol_id "
		"JOIN symbols ts ON ts.id = r.to_symbol_id;"), TEXT("source reference edges"));
	Exec(TEXT(
		"INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at) "
		"SELECT 'source',i.child_id,i.parent_id,'inheritance','extends',1.0,'inheritance',i.id,CAST(strftime('%s','now') AS INTEGER) "
		"FROM inheritance i "
		"JOIN symbols cs ON cs.id = i.child_id "
		"JOIN symbols ps ON ps.id = i.parent_id;"), TEXT("source inheritance edges"));
	Exec(TEXT(
		"WITH ref_in AS ("
		"   SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files"
		"   FROM \"references\" r "
		"   JOIN symbols fs ON fs.id = r.from_symbol_id "
		"   JOIN symbols ts ON ts.id = r.to_symbol_id "
		"   GROUP BY to_symbol_id"
		" ), ref_out AS ("
		"   SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out"
		"   FROM \"references\" r "
		"   JOIN symbols fs ON fs.id = r.from_symbol_id "
		"   JOIN symbols ts ON ts.id = r.to_symbol_id "
		"   GROUP BY from_symbol_id"
		" ), inh_desc AS ("
		"   SELECT parent_id AS symbol_id, COUNT(*) AS descendants"
		"   FROM inheritance i JOIN symbols cs ON cs.id = i.child_id JOIN symbols ps ON ps.id = i.parent_id GROUP BY parent_id"
		" ), inh_anc AS ("
		"   SELECT child_id AS symbol_id, COUNT(*) AS ancestors"
		"   FROM inheritance i JOIN symbols cs ON cs.id = i.child_id JOIN symbols ps ON ps.id = i.parent_id GROUP BY child_id"
		" ), counts AS ("
		" SELECT s.id AS native_id,"
		"        COALESCE(ri.fan_in,0) AS fan_in,"
		"        COALESCE(ro.fan_out,0) AS fan_out,"
		"        COALESCE(id.descendants,0) AS descendants,"
		"        COALESCE(ia.ancestors,0) AS ancestors,"
		"        COALESCE(ri.caller_files,0) AS caller_files,"
		"        s.is_ue_macro AS is_ue_macro,"
		"        CASE"
		"          WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%ufunction%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%server%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%client%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%netmulticast%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%onrep%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%replication%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%rpc%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%network%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%save%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%serialize%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%archive%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%auth%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%login%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%account%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%session%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%purchase%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%iap%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%store%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%entitlement%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%anticheat%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%crypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%encrypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%decrypt%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signature%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signed%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%signing%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign %'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '% sign'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%hash%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%exec%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%eval%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%command%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%file%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%registry%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%process%'"
		"          THEN 1 ELSE 0 END AS sensitivity"
		" FROM symbols s"
		" LEFT JOIN ref_in ri ON ri.symbol_id = s.id"
		" LEFT JOIN ref_out ro ON ro.symbol_id = s.id"
		" LEFT JOIN inh_desc id ON id.symbol_id = s.id"
		" LEFT JOIN inh_anc ia ON ia.symbol_id = s.id"
		"), scored AS ("
		" SELECT c.*, MIN(1.0,"
		"        MIN(c.fan_in,50) / 50.0 * 0.35 +"
		"        MIN(c.descendants,30) / 30.0 * 0.25 +"
		"        MIN(c.fan_out,50) / 50.0 * 0.10 +"
		"        CASE WHEN c.is_ue_macro != 0 THEN 0.15 ELSE 0.0 END +"
		"        MIN(c.caller_files,20) / 20.0 * 0.15 +"
		"        CASE WHEN c.sensitivity != 0 THEN 0.15 ELSE 0.0 END) AS score"
		" FROM counts c"
		") "
		"INSERT INTO crg_node_metrics(node_id,fan_in,fan_out,hard_in,descendants,risk_score,risk_tier,reasons_json,raw_counts_json,scoring_version,computed_at) "
		"SELECT s.native_id,s.fan_in,s.fan_out,0,s.descendants,ROUND(s.score,3),"
		"       CASE WHEN s.score >= 0.66 THEN 'high' WHEN s.score >= 0.33 THEN 'medium' ELSE 'low' END,"
		"       CASE WHEN s.sensitivity != 0 THEN"
		"         printf('[\"caller fan-in: %d\",\"inheritance descendants (1-hop): %d\",\"callee fan-out: %d\",\"module/file boundary crossing: %d distinct caller file(s)\",\"sensitivity: UE-domain sensitive surface (token-boundary matched)\"]',"
		"                s.fan_in,s.descendants,s.fan_out,s.caller_files)"
		"       ELSE"
		"         printf('[\"caller fan-in: %d\",\"inheritance descendants (1-hop): %d\",\"callee fan-out: %d\",\"module/file boundary crossing: %d distinct caller file(s)\"]',"
		"                s.fan_in,s.descendants,s.fan_out,s.caller_files)"
		"       END,"
		"       printf('{\"callers\":%d,\"callees\":%d,\"descendants\":%d,\"ancestors\":%d,\"caller_files\":%d,\"is_ue_macro\":%d,\"sensitivity\":%d}',"
		"              s.fan_in,s.fan_out,s.descendants,s.ancestors,s.caller_files,s.is_ue_macro,s.sensitivity),"
		"       '3',CAST(strftime('%s','now') AS INTEGER) "
		"FROM scored s;"), TEXT("source metrics"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('cache_version','1');"), TEXT("cache_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('scoring_version','3');"), TEXT("scoring_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('built_at',datetime('now'));"), TEXT("built_at"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_built_at',datetime('now'));"), TEXT("source_built_at"));

	int64 OverrideEdgeCount = 0;
	if (bOk)
	{
		FString OverrideError;
		if (!PopulateSourceOverrideEdgeCacheLocked(*Database, OverrideEdgeCount, OverrideError))
		{
			bOk = false;
			Warnings.Add(MakeShared<FJsonValueString>(
				OverrideError.IsEmpty()
					? TEXT("CRG cache rebuild failed at source override edge cache")
					: FString::Printf(TEXT("CRG cache rebuild failed at source override edge cache: %s"), *OverrideError)));
		}
	}
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('source_override_edges_version','2');"), TEXT("source_override_edges_version"));
	if (bOk)
	{
		Root->SetNumberField(TEXT("source_override_edges_built"), static_cast<double>(OverrideEdgeCount));
	}

	if (bOk)
	{
		Database->Execute(TEXT("COMMIT;"));
	}
	else Database->Execute(TEXT("ROLLBACK;"));

	TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
	if (Exists(TEXT("table"), TEXT("crg_nodes")))
	{
		After->SetNumberField(TEXT("crg_nodes"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'source';"))));
		After->SetNumberField(TEXT("crg_edges"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'source';"))));
		After->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'source';"))));
		After->SetNumberField(TEXT("source_override_edges"), static_cast<double>(
			Count(TEXT("SELECT COUNT(*) FROM source_override_edges;"))));
	}
	Root->SetObjectField(TEXT("after"), After);
	Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
	Root->SetStringField(TEXT("summary"), bOk
		? TEXT("Rebuilt source CRG projection/cache from EngineSource symbols, references and inheritance")
		: TEXT("Source CRG projection/cache rebuild failed; rolled back"));
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNextActions(Root, { TEXT("source.health"), TEXT("source.risk_score"), TEXT("source.review_context") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::GetCachedRiskForSymbol(int64 SymbolId)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return nullptr;

	auto Exists = [&](const TCHAR* Name) -> bool
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;")))
		{
			return false;
		}
		S.SetBindingValueByIndex(1, FString(Name));
		return S.Step() == ESQLitePreparedStatementStepResult::Row;
	};
	if (!Exists(TEXT("crg_nodes")) || !Exists(TEXT("crg_node_metrics")) || !Exists(TEXT("crg_meta")))
	{
		return nullptr;
	}

	FSQLitePreparedStatement S;
	if (!S.Create(*Database, TEXT(
		"SELECT s.name,s.qualified_name,s.kind,COALESCE(f.path,''),s.line_start,"
		"       m.risk_score,m.risk_tier,m.reasons_json,m.raw_counts_json,m.scoring_version,"
		"       COALESCE((SELECT value FROM crg_meta WHERE key = 'cache_version'), '1') "
		"FROM crg_nodes n "
		"JOIN crg_node_metrics m ON m.node_id = n.id "
		"JOIN symbols s ON s.id = n.native_id "
		"LEFT JOIN files f ON f.id = s.file_id "
		"WHERE n.domain = 'source' AND n.native_table = 'symbols' AND n.native_id = ? "
		"LIMIT 1;")))
	{
		return nullptr;
	}
	S.SetBindingValueByIndex(1, SymbolId);
	if (S.Step() != ESQLitePreparedStatementStepResult::Row)
	{
		return nullptr;
	}

	FString Name, QualifiedName, Kind, File, Tier, ReasonsJson, RawCountsJson, ScoringVersion, CacheVersion;
	int32 Line = 0;
	double Score = 0.0;
	S.GetColumnValueByIndex(0, Name);
	S.GetColumnValueByIndex(1, QualifiedName);
	S.GetColumnValueByIndex(2, Kind);
	S.GetColumnValueByIndex(3, File);
	S.GetColumnValueByIndex(4, Line);
	S.GetColumnValueByIndex(5, Score);
	S.GetColumnValueByIndex(6, Tier);
	S.GetColumnValueByIndex(7, ReasonsJson);
	S.GetColumnValueByIndex(8, RawCountsJson);
	S.GetColumnValueByIndex(9, ScoringVersion);
	S.GetColumnValueByIndex(10, CacheVersion);

	TArray<TSharedPtr<FJsonValue>> Reasons;
	if (!ParseJsonArray(ReasonsJson, Reasons))
	{
		Reasons.Add(MakeShared<FJsonValueString>(TEXT("cached reasons_json could not be parsed")));
	}
	TSharedPtr<FJsonObject> RawCounts = ParseJsonObject(RawCountsJson);
	if (!RawCounts.IsValid())
	{
		RawCounts = MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("id"), static_cast<double>(SymbolId));
	O->SetStringField(TEXT("name"), Name);
	O->SetStringField(TEXT("qualified_name"), QualifiedName);
	O->SetStringField(TEXT("kind"), Kind);
	O->SetStringField(TEXT("file"), File);
	O->SetNumberField(TEXT("line"), Line);
	O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
	O->SetStringField(TEXT("tier"), Tier);
	O->SetArrayField(TEXT("reasons"), Reasons);
	O->SetObjectField(TEXT("raw_counts"), RawCounts);
	O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("hit"), CacheVersion, ScoringVersion));
	return O;
}

// RX-1.1: unified-diff parsing (port of code_review_graph/changes.py
// _parse_unified_diff). Pure text; no VCS shell-out — the caller produces
// the diff (parent RX-1 contract; CRG incremental.py is the caller's job).
TMap<FString, TArray<TPair<int32, int32>>> FMonolithSourceDatabase::ParseUnifiedDiffRanges(const FString& DiffText)
{
	TMap<FString, TArray<TPair<int32, int32>>> Ranges;
	if (DiffText.IsEmpty())
	{
		return Ranges;
	}

	TArray<FString> Lines;
	DiffText.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
	FString CurrentFile;
	bool bHaveFile = false;
	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FString& Line = Lines[LineIdx];
		// Accept both git-style ("+++ b/path") and plain ("+++ path")
		// unified-diff new-file headers. The RX-1 contract feeds git diffs,
		// but non-git producers omit the "b/" prefix; without this the parser
		// never sets CurrentFile and every hunk range is silently dropped,
		// degrading RX-1.1 line precision back to file-level matching. A bare
		// "+++ " can also be added file content, so a plain header is only
		// honored when the previous line was the matching "--- " header.
		const bool bGitNewFileHeader =
			Line.StartsWith(TEXT("+++ b/"), ESearchCase::CaseSensitive);
		const bool bPlainNewFileHeader =
			!bGitNewFileHeader
			&& Line.StartsWith(TEXT("+++ "), ESearchCase::CaseSensitive)
			&& LineIdx > 0
			&& Lines[LineIdx - 1].StartsWith(TEXT("--- "), ESearchCase::CaseSensitive);
		if (bGitNewFileHeader || bPlainNewFileHeader)
		{
			FString HeaderPath = Line.RightChop(bGitNewFileHeader ? 6 : 4);
			int32 TabIdx = INDEX_NONE;
			if (HeaderPath.FindChar(TEXT('\t'), TabIdx))
			{
				HeaderPath = HeaderPath.Left(TabIdx);
			}
			HeaderPath.TrimStartAndEndInline();
			if (HeaderPath == TEXT("/dev/null"))
			{
				// Deleted file: no new-side ranges to record.
				CurrentFile.Reset();
				bHaveFile = false;
				continue;
			}
			CurrentFile = NormalizeChangedPath(HeaderPath);
			bHaveFile = !CurrentFile.IsEmpty();
			continue;
		}
		if (!bHaveFile || !Line.StartsWith(TEXT("@@ "), ESearchCase::CaseSensitive))
		{
			continue;
		}
		// "@@ -<old>[,<n>] +<start>[,<count>] @@ ..." — read the new-file side.
		int32 PlusIdx = INDEX_NONE;
		if (!Line.FindChar(TEXT('+'), PlusIdx) || PlusIdx + 1 >= Line.Len())
		{
			continue;
		}
		int32 Cursor = PlusIdx + 1;
		auto ReadInt = [&Line, &Cursor]() -> int32
		{
			int32 Value = 0;
			bool bAny = false;
			while (Cursor < Line.Len() && FChar::IsDigit(Line[Cursor]))
			{
				Value = Value * 10 + (Line[Cursor] - TEXT('0'));
				++Cursor;
				bAny = true;
			}
			return bAny ? Value : -1;
		};
		const int32 Start = ReadInt();
		if (Start <= 0)
		{
			continue;
		}
		int32 Count = 1;
		if (Cursor < Line.Len() && Line[Cursor] == TEXT(','))
		{
			++Cursor;
			const int32 Parsed = ReadInt();
			Count = (Parsed >= 0) ? Parsed : 1;
		}
		const int32 End = (Count == 0) ? Start : (Start + Count - 1);
		Ranges.FindOrAdd(CurrentFile).Add(TPair<int32, int32>(Start, End));
	}
	return Ranges;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::DetectChanges(
	const TArray<FString>& ChangedPaths,
	int32 MaxResults,
	const FString& DetailLevel,
	const TMap<FString, TArray<TPair<int32, int32>>>& ChangedRanges)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Cap = FMath::Clamp(MaxResults <= 0 ? 200 : MaxResults, 1, 2000);
	const bool bStandard = DetailLevel.Equals(TEXT("standard"), ESearchCase::IgnoreCase);
	constexpr int32 MaxRangesPerPath = 256;

	// Resolve per-path line ranges (normalized keys, sanitized). A path that
	// only appears in ChangedRanges still counts as a changed path.
	TMap<FString, TArray<TPair<int32, int32>>> PathRanges;
	for (const TPair<FString, TArray<TPair<int32, int32>>>& Kv : ChangedRanges)
	{
		const FString Key = NormalizeChangedPath(Kv.Key);
		if (Key.IsEmpty())
		{
			continue;
		}
		TArray<TPair<int32, int32>>& Out = PathRanges.FindOrAdd(Key);
		for (const TPair<int32, int32>& R : Kv.Value)
		{
			if (Out.Num() >= MaxRangesPerPath)
			{
				break;
			}
			const int32 RStart = R.Key;
			const int32 REnd = R.Value;
			if (RStart > 0 && REnd >= RStart)
			{
				Out.Add(TPair<int32, int32>(RStart, REnd));
			}
		}
		if (Out.Num() == 0)
		{
			PathRanges.Remove(Key);
		}
	}

	TArray<FString> NormalizedPaths;
	for (const FString& RawPath : ChangedPaths)
	{
		const FString Normalized = NormalizeChangedPath(RawPath);
		if (!Normalized.IsEmpty() && !NormalizedPaths.Contains(Normalized))
		{
			NormalizedPaths.Add(Normalized);
		}
	}
	for (const TPair<FString, TArray<TPair<int32, int32>>>& Kv : PathRanges)
	{
		if (!NormalizedPaths.Contains(Kv.Key))
		{
			NormalizedPaths.Add(Kv.Key);
		}
	}

	const bool bLinePrecision = PathRanges.Num() > 0;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetArrayField(TEXT("changed_paths"), StringArray(NormalizedPaths));
	Input->SetStringField(TEXT("detail_level"), bStandard ? TEXT("standard") : TEXT("minimal"));
	Input->SetStringField(TEXT("precision"), bLinePrecision ? TEXT("line") : TEXT("file"));
	Input->SetNumberField(TEXT("range_paths"), PathRanges.Num());
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_results"), Cap);
	Root->SetObjectField(TEXT("limits"), Limits);
	Root->SetStringField(TEXT("scoring_version"), TEXT("3"));
	Root->SetNumberField(TEXT("risk_score"), 0.0);

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("changed_entities"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}

	if (NormalizedPaths.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("changed_paths/paths, changed_ranges, or diff_text must yield at least one path"));
		Root->SetArrayField(TEXT("changed_entities"), TArray<TSharedPtr<FJsonValue>>());
		TSharedPtr<FJsonObject> Impact = MakeShared<FJsonObject>();
		Impact->SetNumberField(TEXT("depth"), 1);
		Impact->SetNumberField(TEXT("impacted_count"), 0);
		Root->SetObjectField(TEXT("impact"), Impact);
		Root->SetNumberField(TEXT("changed_entity_count"), 0);
		Root->SetNumberField(TEXT("impacted_count"), 0);
		Root->SetNumberField(TEXT("test_gap_count"), 0);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.search_source"), TEXT("source.read_file") });
		return Root;
	}

	TSet<int64> ChangedIds;
	TArray<TSharedPtr<FJsonValue>> ChangedEntities;
	bool bTruncated = false;

	for (const FString& Path : NormalizedPaths)
	{
		if (ChangedEntities.Num() >= Cap)
		{
			bTruncated = true;
			break;
		}
		const FString EscapedPath = Path
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"), TEXT("\\%"))
			.Replace(TEXT("_"), TEXT("\\_"));

		const TArray<TPair<int32, int32>>* Ranges = PathRanges.Find(Path);
		const bool bLineMode = Ranges != nullptr && Ranges->Num() > 0;

		FString Sql =
			TEXT("SELECT s.id,s.name,s.qualified_name,s.kind,s.file_id,COALESCE(f.path,''),")
			TEXT("       s.line_start,s.line_end,COALESCE(s.signature,''),s.is_ue_macro ")
			TEXT("FROM symbols s JOIN files f ON f.id = s.file_id ")
			TEXT("WHERE replace(f.path,'\\','/') LIKE ? ESCAPE '\\' ");
		if (bLineMode)
		{
			// CRG changes.py:204 overlap rule: line_start <= end AND line_end >= start.
			// Non-positive spans are not line-provable (Monolith analog of CRG's None-skip).
			Sql += TEXT("AND s.line_start > 0 AND s.line_end > 0 AND (");
			for (int32 RIdx = 0; RIdx < Ranges->Num(); ++RIdx)
			{
				Sql += (RIdx == 0)
					? TEXT("(s.line_start <= ? AND s.line_end >= ?)")
					: TEXT(" OR (s.line_start <= ? AND s.line_end >= ?)");
			}
			Sql += TEXT(") ");
		}
		Sql += TEXT("ORDER BY s.id LIMIT ?;");

		FSQLitePreparedStatement S;
		if (!S.Create(*Database, *Sql))
		{
			continue;
		}
		int32 BindIdx = 1;
		S.SetBindingValueByIndex(BindIdx++, FString::Printf(TEXT("%%%s"), *EscapedPath));
		if (bLineMode)
		{
			for (const TPair<int32, int32>& R : *Ranges)
			{
				S.SetBindingValueByIndex(BindIdx++, static_cast<int64>(R.Value)); // range end
				S.SetBindingValueByIndex(BindIdx++, static_cast<int64>(R.Key));   // range start
			}
		}
		S.SetBindingValueByIndex(BindIdx, static_cast<int64>(Cap + 1));

		while (S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			if (ChangedEntities.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}

			FDetectSymbolRow Sym;
			int32 UeMacro = 0;
			S.GetColumnValueByIndex(0, Sym.Id);
			S.GetColumnValueByIndex(1, Sym.Name);
			S.GetColumnValueByIndex(2, Sym.QualifiedName);
			S.GetColumnValueByIndex(3, Sym.Kind);
			S.GetColumnValueByIndex(4, Sym.FileId);
			S.GetColumnValueByIndex(5, Sym.File);
			S.GetColumnValueByIndex(6, Sym.LineStart);
			S.GetColumnValueByIndex(7, Sym.LineEnd);
			S.GetColumnValueByIndex(8, Sym.Signature);
			S.GetColumnValueByIndex(9, UeMacro);
			Sym.bIsUEMacro = UeMacro != 0;

			if (ChangedIds.Contains(Sym.Id))
			{
				continue;
			}
			ChangedIds.Add(Sym.Id);

			TSharedPtr<FJsonObject> Scored = ScoreSymbolLocked(*Database, Sym);
			Scored->SetStringField(TEXT("matched_path"), Path);
			if (bStandard && bLineMode)
			{
				TArray<TSharedPtr<FJsonValue>> MatchedRanges;
				for (const TPair<int32, int32>& R : *Ranges)
				{
					if (Sym.LineStart <= R.Value && Sym.LineEnd >= R.Key)
					{
						TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
						RangeObj->SetNumberField(TEXT("start"), R.Key);
						RangeObj->SetNumberField(TEXT("end"), R.Value);
						MatchedRanges.Add(MakeShared<FJsonValueObject>(RangeObj));
					}
				}
				Scored->SetArrayField(TEXT("matched_ranges"), MatchedRanges);
			}
			ChangedEntities.Add(MakeShared<FJsonValueObject>(Scored));
		}
	}

	ChangedEntities.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return JsonScore(A->AsObject()) > JsonScore(B->AsObject());
	});

	TSet<int64> ImpactedIds;
	for (int64 ChangedId : ChangedIds)
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, TEXT("SELECT DISTINCT from_symbol_id FROM \"references\" WHERE to_symbol_id = ? LIMIT 201;")))
		{
			continue;
		}
		S.SetBindingValueByIndex(1, ChangedId);
		while (S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 FromId = 0;
			S.GetColumnValueByIndex(0, FromId);
			if (!ChangedIds.Contains(FromId))
			{
				ImpactedIds.Add(FromId);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> ImpactedEntities;
	if (bStandard)
	{
		int32 Emitted = 0;
		for (int64 Id : ImpactedIds)
		{
			if (Emitted >= 200)
			{
				break;
			}
			if (TSharedPtr<FJsonObject> Symbol = SymbolByIdLocked(*Database, Id))
			{
				ImpactedEntities.Add(MakeShared<FJsonValueObject>(Symbol));
				++Emitted;
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> TestGaps;
	for (const TSharedPtr<FJsonValue>& ChangedValue : ChangedEntities)
	{
		const TSharedPtr<FJsonObject> Changed = ChangedValue->AsObject();
		FString Kind;
		if (!Changed.IsValid() || !Changed->TryGetStringField(TEXT("kind"), Kind) || !Kind.Contains(TEXT("function")))
		{
			continue;
		}
		int64 Id = 0;
		double IdNumber = 0.0;
		if (Changed->TryGetNumberField(TEXT("id"), IdNumber))
		{
			Id = static_cast<int64>(IdNumber);
		}
		if (Id <= 0 || HasIndexedTestReferenceLocked(*Database, Id))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Gap = MakeShared<FJsonObject>();
		Gap->SetNumberField(TEXT("id"), static_cast<double>(Id));

		FString Name;
		Changed->TryGetStringField(TEXT("name"), Name);
		Gap->SetStringField(TEXT("name"), Name);

		FString QualifiedName;
		Changed->TryGetStringField(TEXT("qualified_name"), QualifiedName);
		Gap->SetStringField(TEXT("qualified_name"), QualifiedName);

		Gap->SetStringField(TEXT("reason"), TEXT("no indexed inbound test or automation reference"));
		TestGaps.Add(MakeShared<FJsonValueObject>(Gap));
	}

	TArray<TSharedPtr<FJsonValue>> Priorities;
	const int32 PriorityLimit = bStandard ? FMath::Min(ChangedEntities.Num(), 10) : FMath::Min(ChangedEntities.Num(), 3);
	for (int32 Index = 0; Index < PriorityLimit; ++Index)
	{
		const TSharedPtr<FJsonObject> O = ChangedEntities[Index]->AsObject();
		if (!O.IsValid())
		{
			continue;
		}
		if (bStandard)
		{
			Priorities.Add(MakeShared<FJsonValueObject>(O));
		}
		else
		{
			FString Name;
			if (!O->TryGetStringField(TEXT("qualified_name"), Name) || Name.IsEmpty())
			{
				O->TryGetStringField(TEXT("name"), Name);
			}
			Priorities.Add(MakeShared<FJsonValueString>(Name));
		}
	}

	double MaxRisk = 0.0;
	if (ChangedEntities.Num() > 0)
	{
		MaxRisk = JsonScore(ChangedEntities[0]->AsObject());
	}

	TSharedPtr<FJsonObject> Impact = MakeShared<FJsonObject>();
	Impact->SetNumberField(TEXT("depth"), 1);
	Impact->SetNumberField(TEXT("impacted_count"), ImpactedIds.Num());
	if (bStandard)
	{
		Impact->SetArrayField(TEXT("impacted_entities"), ImpactedEntities);
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d changed source symbol(s), %d direct impacted caller(s), %d heuristic test gap(s), %d review priorit%s"),
		ChangedEntities.Num(), ImpactedIds.Num(), TestGaps.Num(), Priorities.Num(), Priorities.Num() == 1 ? TEXT("y") : TEXT("ies")));
	Root->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(MaxRisk * 1000.0) / 1000.0);
	Root->SetNumberField(TEXT("changed_entity_count"), ChangedEntities.Num());
	Root->SetNumberField(TEXT("impacted_count"), ImpactedIds.Num());
	Root->SetNumberField(TEXT("test_gap_count"), TestGaps.Num());
	Root->SetObjectField(TEXT("impact"), Impact);
	Root->SetArrayField(TEXT("review_priorities"), Priorities);
	if (bStandard)
	{
		Root->SetArrayField(TEXT("changed_entities"), ChangedEntities);
		Root->SetArrayField(TEXT("test_gaps"), TestGaps);
	}
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	if (ChangedEntities.Num() == 0)
	{
		AddNextActions(Root, { TEXT("source.search_source"), TEXT("source.read_file") });
	}
	else
	{
		AddNextActions(Root, { TEXT("source.review_context"), TEXT("source.find_callers"), TEXT("source.risk_score") });
	}
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::PreMergeCheck(
	const TArray<FString>& ChangedPaths,
	int32 MaxResults,
	int32 UnusedLimit,
	const FString& DetailLevel,
	bool bIncludeUnused)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 ChangeCap = FMath::Clamp(MaxResults <= 0 ? 200 : MaxResults, 1, 2000);
	const int32 UnusedCap = FMath::Clamp(UnusedLimit <= 0 ? 20 : UnusedLimit, 1, 200);
	const bool bStandard = DetailLevel.Equals(TEXT("standard"), ESearchCase::IgnoreCase);

	TArray<FString> NormalizedPaths;
	for (const FString& RawPath : ChangedPaths)
	{
		const FString Normalized = NormalizeChangedPath(RawPath);
		if (!Normalized.IsEmpty() && !NormalizedPaths.Contains(Normalized))
		{
			NormalizedPaths.Add(Normalized);
		}
	}

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetArrayField(TEXT("changed_paths"), StringArray(NormalizedPaths));
	Input->SetStringField(TEXT("detail_level"), bStandard ? TEXT("standard") : TEXT("minimal"));
	Input->SetBoolField(TEXT("include_unused"), bIncludeUnused);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_results"), ChangeCap);
	Limits->SetNumberField(TEXT("unused_limit"), UnusedCap);
	Root->SetObjectField(TEXT("limits"), Limits);
	Root->SetStringField(TEXT("scoring_version"), TEXT("3"));

	TSharedPtr<FJsonObject> HealthResult = ComputeHealth(false, true);
	// pre_merge_check stays file-level (no line ranges): default empty range map = original behavior.
	TSharedPtr<FJsonObject> ChangeResult = DetectChanges(NormalizedPaths, ChangeCap, bStandard ? TEXT("standard") : TEXT("minimal"));
	TSharedPtr<FJsonObject> UnusedResult = bIncludeUnused
		? FindUnused(TEXT("all"), UnusedCap, TEXT("low"))
		: nullptr;

	TArray<TSharedPtr<FJsonValue>> Checks;
	TArray<TSharedPtr<FJsonValue>> Findings;
	int32 Severity = 0; // 0 pass, 1 warn, 2 fail
	auto Promote = [&](int32 Value)
	{
		Severity = FMath::Max(Severity, Value);
	};
	auto StatusOf = [](const TSharedPtr<FJsonObject>& Object) -> FString
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(TEXT("status"), Value) ? Value : FString(TEXT("error"));
	};
	auto SummaryOf = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Fallback) -> FString
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(TEXT("summary"), Value) ? Value : FString(Fallback);
	};
	auto IntField = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) -> int32
	{
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value)
			? static_cast<int32>(Value)
			: 0;
	};
	auto NumField = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) -> double
	{
		double Value = 0.0;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return Value;
	};
	auto BoolField = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Field) -> bool
	{
		bool Value = false;
		return Object.IsValid() && Object->TryGetBoolField(Field, Value) ? Value : false;
	};
	auto AddCheck = [&](const TCHAR* Name, const FString& Status, const FString& Summary, int32 CheckSeverity)
	{
		TSharedPtr<FJsonObject> Check = MakeShared<FJsonObject>();
		Check->SetStringField(TEXT("name"), Name);
		Check->SetStringField(TEXT("status"), Status);
		Check->SetStringField(TEXT("severity"), CheckSeverity >= 2 ? TEXT("fail") : CheckSeverity == 1 ? TEXT("warn") : TEXT("pass"));
		Check->SetStringField(TEXT("summary"), Summary);
		Checks.Add(MakeShared<FJsonValueObject>(Check));
		Promote(CheckSeverity);
	};
	auto AddFinding = [&](const TCHAR* SeverityName, const TCHAR* CheckName, const FString& Message)
	{
		TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
		Finding->SetStringField(TEXT("severity"), SeverityName);
		Finding->SetStringField(TEXT("check"), CheckName);
		Finding->SetStringField(TEXT("message"), Message);
		Findings.Add(MakeShared<FJsonValueObject>(Finding));
	};

	const FString HealthStatus = StatusOf(HealthResult);
	const int32 HealthSeverity = HealthStatus == TEXT("error") ? 2 : HealthStatus == TEXT("warning") ? 1 : 0;
	AddCheck(TEXT("health"), HealthStatus, SummaryOf(HealthResult, TEXT("Source health could not run")), HealthSeverity);
	if (HealthSeverity > 0)
	{
		AddFinding(HealthSeverity >= 2 ? TEXT("error") : TEXT("warning"), TEXT("health"),
			SummaryOf(HealthResult, TEXT("Source health failed")));
	}

	const FString ChangeStatus = StatusOf(ChangeResult);
	const int32 ChangedCount = IntField(ChangeResult, TEXT("changed_entity_count"));
	const int32 ImpactCount = IntField(ChangeResult, TEXT("impacted_count"));
	const int32 TestGapCount = IntField(ChangeResult, TEXT("test_gap_count"));
	const double RiskScore = NumField(ChangeResult, TEXT("risk_score"));
	int32 ChangeSeverity = ChangeStatus == TEXT("error") ? 2 : 0;
	if (ChangeSeverity == 0 && ChangedCount == 0)
	{
		ChangeSeverity = 1;
		AddFinding(TEXT("warning"), TEXT("detect_changes"), TEXT("No indexed source symbol matched the changed path set"));
	}
	if (RiskScore >= 0.66)
	{
		ChangeSeverity = FMath::Max(ChangeSeverity, 1);
		AddFinding(TEXT("warning"), TEXT("detect_changes"),
			FString::Printf(TEXT("Changed source risk score is high: %.3f"), RiskScore));
	}
	if (ImpactCount > 50)
	{
		ChangeSeverity = FMath::Max(ChangeSeverity, 1);
		AddFinding(TEXT("warning"), TEXT("detect_changes"),
			FString::Printf(TEXT("Changed source set has broad direct caller impact: %d caller(s)"), ImpactCount));
	}
	if (TestGapCount > 0)
	{
		ChangeSeverity = FMath::Max(ChangeSeverity, 1);
		AddFinding(TEXT("warning"), TEXT("detect_changes"),
			FString::Printf(TEXT("%d changed function(s) have no indexed test/automation reference"), TestGapCount));
	}
	if (ChangeStatus == TEXT("error"))
	{
		AddFinding(TEXT("error"), TEXT("detect_changes"), SummaryOf(ChangeResult, TEXT("detect_changes failed")));
	}
	AddCheck(TEXT("detect_changes"), ChangeStatus, FString::Printf(
		TEXT("%d changed source symbol(s), %d impacted caller(s), %d test gap(s), risk=%.3f"),
		ChangedCount, ImpactCount, TestGapCount, RiskScore), ChangeSeverity);

	int32 UnusedCount = 0;
	if (bIncludeUnused && UnusedResult.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		UnusedCount = UnusedResult->TryGetArrayField(TEXT("items"), Items) && Items ? Items->Num() : 0;
		const FString UnusedStatus = StatusOf(UnusedResult);
		const int32 UnusedSeverity = UnusedStatus == TEXT("error") ? 2 : UnusedCount > 0 ? 1 : 0;
		AddCheck(TEXT("find_unused"), UnusedStatus, FString::Printf(
			TEXT("%d advisory unused source candidate(s) sampled"), UnusedCount), UnusedSeverity);
		if (UnusedCount > 0)
		{
			AddFinding(TEXT("warning"), TEXT("find_unused"),
				FString::Printf(TEXT("%d advisory unused source candidate(s) present in sampled index"), UnusedCount));
		}
	}

	const FString Decision = Severity >= 2 ? TEXT("fail") : Severity == 1 ? TEXT("warn") : TEXT("pass");
	Root->SetStringField(TEXT("status"), Severity >= 2 ? TEXT("error") : Severity == 1 ? TEXT("warning") : TEXT("ok"));
	Root->SetStringField(TEXT("decision"), Decision);
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("Source pre-merge check %s: %d changed symbol(s), %d impacted caller(s), %d test gap(s), %d finding(s)"),
		*Decision, ChangedCount, ImpactCount, TestGapCount, Findings.Num()));
	Root->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(RiskScore * 1000.0) / 1000.0);
	Root->SetArrayField(TEXT("checks"), Checks);
	Root->SetArrayField(TEXT("findings"), Findings);
	Root->SetNumberField(TEXT("changed_entity_count"), ChangedCount);
	Root->SetNumberField(TEXT("impacted_count"), ImpactCount);
	Root->SetNumberField(TEXT("test_gap_count"), TestGapCount);
	Root->SetNumberField(TEXT("unused_count"), UnusedCount);
	Root->SetBoolField(TEXT("truncated"), BoolField(ChangeResult, TEXT("truncated")) || BoolField(UnusedResult, TEXT("truncated")));
	if (bStandard)
	{
		Root->SetObjectField(TEXT("health"), HealthResult);
		Root->SetObjectField(TEXT("change_analysis"), ChangeResult);
		if (UnusedResult.IsValid())
		{
			Root->SetObjectField(TEXT("unused"), UnusedResult);
		}
	}
	if (Severity >= 2)
	{
		AddNextActions(Root, { TEXT("source.health"), TEXT("source.search_source") });
	}
	else
	{
		AddNextActions(Root, { TEXT("source.detect_changes"), TEXT("source.review_context"), TEXT("source.find_unused") });
	}
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::Snapshot(const FString& Label, bool bExecute)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString RequestedLabel = Label.TrimStartAndEnd();
	const FString CleanLabel = RequestedLabel.IsEmpty()
		? MakeAutoSnapshotLabel(TEXT("source"))
		: RequestedLabel;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("label"), CleanLabel);
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}
	if (!TableExistsLocked(*Database, TEXT("crg_nodes")) || !TableExistsLocked(*Database, TEXT("crg_edges")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("CRG projection tables are missing; run source.repair_crg_cache execute=true first"));
		AddNextActions(Root, { TEXT("source.repair_crg_cache"), TEXT("source.health") });
		return Root;
	}

	FSnapshotManifest Manifest;
	if (!LoadCurrentManifestLocked(*Database, TEXT("source"), Manifest))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to read current source CRG projection"));
		AddNextActions(Root, { TEXT("source.health"), TEXT("source.repair_crg_cache") });
		return Root;
	}

	Root->SetNumberField(TEXT("node_count"), Manifest.Nodes.Num());
	Root->SetNumberField(TEXT("edge_count"), Manifest.Edges.Num());
	Root->SetBoolField(TEXT("executed"), bExecute);
	Root->SetBoolField(TEXT("truncated"), false);
	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), FString::Printf(
			TEXT("Would capture source CRG snapshot '%s' with %d node(s), %d edge(s)"),
			*CleanLabel, Manifest.Nodes.Num(), Manifest.Edges.Num()));
		AddNextActions(Root, { TEXT("source.snapshot execute=true"), TEXT("source.diff_snapshots") });
		return Root;
	}

	if (!EnsureSnapshotTable(*Database))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to create crg_snapshots table"));
		AddNextActions(Root, { TEXT("source.health") });
		return Root;
	}

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*Database, TEXT(
		"INSERT OR REPLACE INTO crg_snapshots(label,domain,captured_at,node_count,edge_count,manifest_json) "
		"VALUES(?,?,?,?,?,?);")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to prepare source snapshot insert"));
		AddNextActions(Root, { TEXT("source.health") });
		return Root;
	}
	Stmt.SetBindingValueByIndex(1, CleanLabel);
	Stmt.SetBindingValueByIndex(2, FString(TEXT("source")));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()));
	Stmt.SetBindingValueByIndex(4, static_cast<int64>(Manifest.Nodes.Num()));
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Manifest.Edges.Num()));
	Stmt.SetBindingValueByIndex(6, SerializeManifest(Manifest));
	if (!Stmt.Execute())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to store source CRG snapshot"));
		AddNextActions(Root, { TEXT("source.health") });
		return Root;
	}

	Root->SetNumberField(TEXT("id"), static_cast<double>(Database->GetLastInsertRowId()));
	Root->SetStringField(TEXT("label"), CleanLabel);
	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("Captured source CRG snapshot '%s' with %d node(s), %d edge(s)"),
		*CleanLabel, Manifest.Nodes.Num(), Manifest.Edges.Num()));
	AddNextActions(Root, { TEXT("source.diff_snapshots"), TEXT("source.repair_crg_cache") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::DiffSnapshots(
	const FString& Before,
	const FString& After,
	int32 Limit)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 100 : Limit, 1, 1000);
	const FString BeforeRef = Before.TrimStartAndEnd();
	const FString AfterRef = After.TrimStartAndEnd().IsEmpty() ? TEXT("current") : After.TrimStartAndEnd();

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("before"), BeforeRef);
	Input->SetStringField(TEXT("after"), AfterRef);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Root->SetObjectField(TEXT("limits"), Limits);

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}
	if (BeforeRef.IsEmpty())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("before snapshot label/id is required"));
		AddNextActions(Root, { TEXT("source.snapshot execute=true") });
		return Root;
	}
	if (!TableExistsLocked(*Database, TEXT("crg_snapshots")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("crg_snapshots table is missing; capture a source.snapshot first"));
		AddNextActions(Root, { TEXT("source.snapshot execute=true") });
		return Root;
	}

	FSnapshotRecord BeforeRecord;
	FSnapshotRecord AfterRecord;
	if (!LoadSnapshotRecordLocked(*Database, TEXT("source"), BeforeRef, BeforeRecord))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("Before snapshot not found or invalid: %s"), *BeforeRef));
		AddNextActions(Root, { TEXT("source.snapshot execute=true") });
		return Root;
	}
	if (!LoadSnapshotRecordLocked(*Database, TEXT("source"), AfterRef, AfterRecord))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("After snapshot not found or invalid: %s"), *AfterRef));
		AddNextActions(Root, { TEXT("source.snapshot execute=true") });
		return Root;
	}

	TSet<FString> NewNodes = SetDifference(AfterRecord.Manifest.Nodes, BeforeRecord.Manifest.Nodes);
	TSet<FString> RemovedNodes = SetDifference(BeforeRecord.Manifest.Nodes, AfterRecord.Manifest.Nodes);
	TSet<FString> NewEdges = SetDifference(AfterRecord.Manifest.Edges, BeforeRecord.Manifest.Edges);
	TSet<FString> RemovedEdges = SetDifference(BeforeRecord.Manifest.Edges, AfterRecord.Manifest.Edges);

	bool bTruncated = false;
	Root->SetArrayField(TEXT("new_nodes"), TakeStringSamples(NewNodes, Cap, bTruncated));
	Root->SetArrayField(TEXT("removed_nodes"), TakeStringSamples(RemovedNodes, Cap, bTruncated));
	Root->SetArrayField(TEXT("new_edges"), TakeEdgeSamples(NewEdges, Cap, bTruncated));
	Root->SetArrayField(TEXT("removed_edges"), TakeEdgeSamples(RemovedEdges, Cap, bTruncated));

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("nodes_added"), NewNodes.Num());
	Summary->SetNumberField(TEXT("nodes_removed"), RemovedNodes.Num());
	Summary->SetNumberField(TEXT("edges_added"), NewEdges.Num());
	Summary->SetNumberField(TEXT("edges_removed"), RemovedEdges.Num());
	Summary->SetNumberField(TEXT("before_total_nodes"), BeforeRecord.Manifest.Nodes.Num());
	Summary->SetNumberField(TEXT("after_total_nodes"), AfterRecord.Manifest.Nodes.Num());
	Summary->SetNumberField(TEXT("before_total_edges"), BeforeRecord.Manifest.Edges.Num());
	Summary->SetNumberField(TEXT("after_total_edges"), AfterRecord.Manifest.Edges.Num());
	Root->SetObjectField(TEXT("summary_counts"), Summary);
	Root->SetStringField(TEXT("before_label"), BeforeRecord.Label);
	Root->SetStringField(TEXT("after_label"), AfterRecord.Label);
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("Source CRG diff %s -> %s: +%d/-%d node(s), +%d/-%d edge(s)"),
		*BeforeRecord.Label, *AfterRecord.Label, NewNodes.Num(), RemovedNodes.Num(), NewEdges.Num(), RemovedEdges.Num()));
	AddNextActions(Root, { TEXT("source.snapshot"), TEXT("source.review_hotspots"), TEXT("source.health") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::FindUnused(
	const FString& Kind,
	int32 Limit,
	const FString& MinConfidence)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	FString NormalizedKind = Kind.TrimStartAndEnd().ToLower();
	if (NormalizedKind != TEXT("function") && NormalizedKind != TEXT("class") && NormalizedKind != TEXT("struct"))
	{
		NormalizedKind = TEXT("all");
	}
	FString MinConf = MinConfidence.IsEmpty() ? TEXT("low") : MinConfidence.ToLower();
	if (MinConf != TEXT("low") && MinConf != TEXT("medium") && MinConf != TEXT("high"))
	{
		MinConf = TEXT("low");
	}
	const int32 MinRank = ConfidenceRank(MinConf);
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 100 : Limit, 1, 1000);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("kind"), NormalizedKind);
	Input->SetStringField(TEXT("min_confidence"), MinConf);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Root->SetObjectField(TEXT("limits"), Limits);

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}

	if (MinRank >= ConfidenceRank(TEXT("high")))
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), TEXT("0 advisory source unused candidate(s) found (find_unused never reports high confidence)"));
		Root->SetArrayField(TEXT("items"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.find_callers"), TEXT("source.review_context"), TEXT("source.impact_radius") });
		return Root;
	}

	const bool bFilterKind = NormalizedKind != TEXT("all");
	const FString KindClause = bFilterKind
		? TEXT("AND s.kind = ? ")
		: TEXT("AND s.kind IN ('function','class','struct') ");
	const FString ConfidenceClause = MinRank >= ConfidenceRank(TEXT("medium"))
		? TEXT("AND nc.name_count = 1 ")
		: TEXT("");
	const FString Sql = FString::Printf(TEXT(
		"WITH name_counts AS ("
		"  SELECT name, COUNT(*) AS name_count FROM symbols GROUP BY name"
		") "
		"SELECT s.id,s.name,s.qualified_name,s.kind,s.file_id,COALESCE(f.path,''),"
		"       s.line_start,s.line_end,COALESCE(s.signature,''),nc.name_count "
		"FROM symbols s "
		"LEFT JOIN files f ON f.id = s.file_id "
		"JOIN name_counts nc ON nc.name = s.name "
		"WHERE s.is_ue_macro = 0 "
		"%s"
		"%s"
		"AND NOT EXISTS (SELECT 1 FROM \"references\" r WHERE r.to_symbol_id = s.id) "
		"AND NOT EXISTS (SELECT 1 FROM inheritance i WHERE i.parent_id = s.id) "
		"AND s.name NOT LIKE '~%%' "
		"AND s.name NOT IN ('main','WinMain','DllMain','StaticClass','StaticRegisterNatives','GetPrivateStaticClass') "
		"AND s.name NOT LIKE 'Execute_%%' "
		"AND s.name NOT LIKE 'exec%%' "
		"AND s.name NOT LIKE '%%AutomationTest%%' "
		"AND s.name NOT LIKE '%%Spec' "
		"AND s.qualified_name NOT LIKE '%%AutomationTest%%' "
		"AND COALESCE(s.signature,'') NOT LIKE '%%UFUNCTION%%' "
		"AND COALESCE(s.signature,'') NOT LIKE '%%UPROPERTY%%' "
		"ORDER BY s.id LIMIT ?;"), *KindClause, *ConfidenceClause);

	FSQLitePreparedStatement S;
	TArray<TSharedPtr<FJsonValue>> Items;
	bool bTruncated = false;
	if (S.Create(*Database, *Sql))
	{
		int32 BindIndex = 1;
		if (bFilterKind)
		{
			S.SetBindingValueByIndex(BindIndex++, NormalizedKind);
		}
		S.SetBindingValueByIndex(BindIndex, static_cast<int64>(Cap + 1));

		while (S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			if (Items.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}

			int64 Id = 0, FileId = 0;
			FString Name, QualifiedName, SymKind, File, Signature;
			int32 LineStart = 0, LineEnd = 0, NameCount = 0;
			S.GetColumnValueByIndex(0, Id);
			S.GetColumnValueByIndex(1, Name);
			S.GetColumnValueByIndex(2, QualifiedName);
			S.GetColumnValueByIndex(3, SymKind);
			S.GetColumnValueByIndex(4, FileId);
			S.GetColumnValueByIndex(5, File);
			S.GetColumnValueByIndex(6, LineStart);
			S.GetColumnValueByIndex(7, LineEnd);
			S.GetColumnValueByIndex(8, Signature);
			S.GetColumnValueByIndex(9, NameCount);

			const FString Confidence = NameCount == 1 ? TEXT("medium") : TEXT("low");
			if (ConfidenceRank(Confidence) < MinRank)
			{
				continue;
			}

			TArray<TSharedPtr<FJsonValue>> Reasons;
			Reasons.Add(MakeShared<FJsonValueString>(TEXT("no indexed inbound references")));
			Reasons.Add(MakeShared<FJsonValueString>(TEXT("not an inheritance parent")));
			Reasons.Add(MakeShared<FJsonValueString>(TEXT("UE macro, reflection, automation, and entry-point markers excluded")));
			if (Confidence == TEXT("medium"))
			{
				Reasons.Add(MakeShared<FJsonValueString>(TEXT("unique symbol name in index")));
			}
			else
			{
				Reasons.Add(MakeShared<FJsonValueString>(TEXT("overloaded symbol name reduces confidence")));
			}

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("id"), static_cast<double>(Id));
			O->SetStringField(TEXT("name"), Name);
			O->SetStringField(TEXT("qualified_name"), QualifiedName);
			O->SetStringField(TEXT("kind"), SymKind);
			O->SetNumberField(TEXT("file_id"), static_cast<double>(FileId));
			O->SetStringField(TEXT("file"), File);
			O->SetNumberField(TEXT("line_start"), LineStart);
			O->SetNumberField(TEXT("line_end"), LineEnd);
			O->SetStringField(TEXT("signature"), Signature);
			O->SetStringField(TEXT("confidence"), Confidence);
			O->SetArrayField(TEXT("reasons"), Reasons);
			Items.Add(MakeShared<FJsonValueObject>(O));
		}
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d advisory source unused candidate(s) found (never high confidence; no mutation)"), Items.Num()));
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNextActions(Root, { TEXT("source.find_callers"), TEXT("source.review_context"), TEXT("source.impact_radius") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceDatabase::ReviewHotspots(
	const FString& Kind,
	int32 Limit,
	int32 MinLines,
	bool bIncludeQuestions)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString NormalizedKind = Kind.IsEmpty() ? TEXT("all") : Kind.ToLower();
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 50 : Limit, 1, 200);
	const int32 LocFloor = FMath::Max(MinLines <= 0 ? 100 : MinLines, 0);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("kind"), NormalizedKind);
	Input->SetBoolField(TEXT("include_questions"), bIncludeQuestions);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Limits->SetNumberField(TEXT("min_lines"), LocFloor);
	Root->SetObjectField(TEXT("limits"), Limits);

	if (NormalizedKind != TEXT("fan_in") && NormalizedKind != TEXT("fan_out")
		&& NormalizedKind != TEXT("risk") && NormalizedKind != TEXT("large")
		&& NormalizedKind != TEXT("override") && NormalizedKind != TEXT("all"))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Unsupported kind for source.review_hotspots (expected fan_in|fan_out|risk|large|override|all)"));
		Root->SetArrayField(TEXT("hotspots"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.review_hotspots kind=all"), TEXT("source.risk_score") });
		return Root;
	}

	if (!Database || !Database->IsValid())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("EngineSource DB is not open"));
		Root->SetArrayField(TEXT("hotspots"), TArray<TSharedPtr<FJsonValue>>());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.trigger_reindex"), TEXT("source.health") });
		return Root;
	}

	auto Exists = [&](const TCHAR* Name) -> bool
	{
		FSQLitePreparedStatement S;
		if (!S.Create(*Database, TEXT("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;")))
		{
			return false;
		}
		S.SetBindingValueByIndex(1, FString(Name));
		return S.Step() == ESQLitePreparedStatementStepResult::Row;
	};
	const bool bHasCrg = Exists(TEXT("crg_nodes")) && Exists(TEXT("crg_node_metrics"));
	const bool bHasOverrideCache = bHasCrg && SourceOverrideEdgeCacheReadyLocked(*Database);

	FString WhereClause;
	if (NormalizedKind == TEXT("large"))
	{
		WhereClause = FString::Printf(TEXT("WHERE lines >= %d "), LocFloor);
	}
	else if (NormalizedKind == TEXT("override"))
	{
		WhereClause = TEXT("WHERE override_children > 0 OR overridden_parents > 0 ");
	}
	else
	{
		WhereClause = TEXT("WHERE fan_in > 0 OR fan_out > 0 OR descendants > 0 OR risk_score > 0 OR lines >= ") + FString::FromInt(LocFloor) + TEXT(" ");
	}
	FString OrderBy = TEXT("ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, lines DESC ");
	if (NormalizedKind == TEXT("fan_in")) OrderBy = TEXT("ORDER BY fan_in DESC, risk_score DESC, lines DESC ");
	else if (NormalizedKind == TEXT("fan_out")) OrderBy = TEXT("ORDER BY fan_out DESC, risk_score DESC, lines DESC ");
	else if (NormalizedKind == TEXT("risk")) OrderBy = TEXT("ORDER BY risk_score DESC, fan_in DESC, lines DESC ");
	else if (NormalizedKind == TEXT("large")) OrderBy = TEXT("ORDER BY lines DESC, risk_score DESC, fan_in DESC ");
	else if (NormalizedKind == TEXT("override")) OrderBy = TEXT("ORDER BY override_children DESC, overridden_parents DESC, risk_score DESC, fan_in DESC, lines DESC ");
	const int32 FetchLimit = NormalizedKind == TEXT("override")
		? FMath::Clamp(Cap * 8 + 50, Cap + 1, 2000)
		: Cap + 1;
	const int32 OverrideSeedLimit = FMath::Clamp(Cap * 1000, 5000, 10000);

	FString Sql;
	if (NormalizedKind == TEXT("override"))
	{
		if (bHasOverrideCache)
		{
			Sql = FString::Printf(TEXT(
			"WITH override_symbols AS ("
			"  SELECT parent_symbol_id AS symbol_id FROM source_override_edges"
			"  UNION SELECT child_symbol_id AS symbol_id FROM source_override_edges"
			"), override_children AS ("
			"  SELECT parent_symbol_id AS symbol_id, COUNT(*) AS override_children FROM source_override_edges GROUP BY parent_symbol_id"
			"), overridden_parents AS ("
			"  SELECT child_symbol_id AS symbol_id, COUNT(*) AS overridden_parents FROM source_override_edges GROUP BY child_symbol_id"
			"), scored AS ("
			"  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
			"         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
			"         COALESCE(m.fan_in,0) AS fan_in,COALESCE(m.fan_out,0) AS fan_out,"
			"         COALESCE(m.descendants,0) AS descendants,"
			"         COALESCE(oc.override_children,0) AS override_children,"
			"         COALESCE(op.overridden_parents,0) AS overridden_parents,"
			"         COALESCE(m.risk_score,0.0) AS risk_score,COALESCE(m.risk_tier, 'low') AS risk_tier "
			"  FROM override_symbols os "
			"  JOIN symbols s ON s.id=os.symbol_id "
			"  LEFT JOIN files f ON f.id=s.file_id "
			"  LEFT JOIN override_children oc ON oc.symbol_id=s.id "
			"  LEFT JOIN overridden_parents op ON op.symbol_id=s.id "
			"  LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=s.id "
			"  LEFT JOIN crg_node_metrics m ON m.node_id=n.id"
			") "
			"SELECT *, MAX(risk_score, MIN(override_children,30)/30.0, MIN(overridden_parents,10)/10.0, MIN(lines,500)/500.0) AS hotspot_score "
			"FROM scored %s%sLIMIT %d;"),
			*WhereClause, *OrderBy, FetchLimit);
		}
		else if (bHasCrg)
		{
			Sql = FString::Printf(TEXT(
			"WITH RECURSIVE child_seed AS ("
			"  SELECT id,name,kind,parent_symbol_id FROM symbols "
			"  WHERE kind='function' AND signature LIKE '%%override%%' LIMIT %d"
			"), ancestors(child_class_id, ancestor_class_id) AS ("
			"  SELECT child_id, parent_id FROM inheritance"
			"  UNION "
			"  SELECT a.child_class_id, i.parent_id "
			"  FROM ancestors a "
			"  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id"
			"), override_children AS ("
			"  SELECT base_fn.id AS symbol_id, COUNT(*) AS override_children "
			"  FROM child_seed child_fn "
			"  JOIN symbols child_cls ON child_cls.id=child_fn.parent_symbol_id "
			"  JOIN ancestors AS a ON a.child_class_id=child_cls.id "
			"  JOIN symbols base_cls ON base_cls.id=a.ancestor_class_id "
			"  JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id=base_cls.id "
			"    AND (base_fn.name=child_fn.name "
			"      OR base_fn.name=base_cls.name || '::' || child_fn.name) "
			"    AND base_fn.kind=child_fn.kind "
			"  GROUP BY base_fn.id"
			"), scored AS ("
			"  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
			"         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
			"         COALESCE(m.fan_in,0) AS fan_in,COALESCE(m.fan_out,0) AS fan_out,"
			"         COALESCE(m.descendants,0) AS descendants,oc.override_children,0 AS overridden_parents,"
			"         COALESCE(m.risk_score,0.0) AS risk_score,COALESCE(m.risk_tier, 'low') AS risk_tier "
			"  FROM override_children oc "
			"  JOIN symbols s ON s.id=oc.symbol_id "
			"  LEFT JOIN files f ON f.id=s.file_id "
			"  LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=s.id "
			"  LEFT JOIN crg_node_metrics m ON m.node_id=n.id"
			") "
			"SELECT *, MAX(risk_score, MIN(override_children,30)/30.0, MIN(lines,500)/500.0) AS hotspot_score "
			"FROM scored %s%sLIMIT %d;"),
			OverrideSeedLimit, *WhereClause, *OrderBy, FetchLimit);
		}
		else
		{
			const FString RiskScoreExpr = TEXT("c.estimated_risk");
			const FString RiskTierExpr = TEXT("CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END");
			Sql = FString::Printf(TEXT(
			"WITH RECURSIVE child_seed AS ("
			"  SELECT id,name,kind,parent_symbol_id FROM symbols "
			"  WHERE kind='function' AND signature LIKE '%%override%%' LIMIT %d"
			"), ancestors(child_class_id, ancestor_class_id) AS ("
			"  SELECT child_id, parent_id FROM inheritance"
			"  UNION "
			"  SELECT a.child_class_id, i.parent_id "
			"  FROM ancestors a "
			"  JOIN inheritance AS i ON i.child_id = a.ancestor_class_id"
			"), override_children AS ("
			"  SELECT base_fn.id AS symbol_id, COUNT(*) AS override_children "
			"  FROM child_seed child_fn "
			"  JOIN symbols child_cls ON child_cls.id=child_fn.parent_symbol_id "
			"  JOIN ancestors AS a ON a.child_class_id=child_cls.id "
			"  JOIN symbols base_cls ON base_cls.id=a.ancestor_class_id "
			"  JOIN symbols base_fn INDEXED BY idx_symbols_parent_name_kind ON base_fn.parent_symbol_id=base_cls.id "
			"    AND (base_fn.name=child_fn.name "
			"      OR base_fn.name=base_cls.name || '::' || child_fn.name) "
			"    AND base_fn.kind=child_fn.kind "
			"  GROUP BY base_fn.id"
			"), ref_in AS ("
			"  SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files FROM \"references\" r GROUP BY to_symbol_id"
			"), ref_out AS ("
			"  SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out FROM \"references\" r GROUP BY from_symbol_id"
			"), inh_desc AS ("
			"  SELECT parent_id AS symbol_id, COUNT(*) AS descendants FROM inheritance GROUP BY parent_id"
			"), counts AS ("
			"  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
			"         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
			"         COALESCE(ri.fan_in,0) AS fan_in,COALESCE(ro.fan_out,0) AS fan_out,COALESCE(id.descendants,0) AS descendants,"
			"         oc.override_children,0 AS overridden_parents,COALESCE(ri.caller_files,0) AS caller_files,s.is_ue_macro AS is_ue_macro,"
			"         CASE WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%ufunction%%' THEN 1 ELSE 0 END AS sensitivity,"
			"         MIN(1.0, MIN(COALESCE(ri.fan_in,0),50)/50.0*0.35 + MIN(COALESCE(id.descendants,0),30)/30.0*0.25 + MIN(COALESCE(ro.fan_out,0),50)/50.0*0.10 + CASE WHEN s.is_ue_macro != 0 THEN 0.15 ELSE 0 END + MIN(COALESCE(ri.caller_files,0),20)/20.0*0.15) AS estimated_risk "
			"  FROM override_children oc JOIN symbols s ON s.id=oc.symbol_id LEFT JOIN files f ON f.id=s.file_id "
			"  LEFT JOIN ref_in ri ON ri.symbol_id=s.id LEFT JOIN ref_out ro ON ro.symbol_id=s.id LEFT JOIN inh_desc id ON id.symbol_id=s.id"
			"), scored AS ("
			"  SELECT c.id,c.name,c.qualified_name,c.kind,c.file,c.line_start,c.line_end,c.lines,c.fan_in,c.fan_out,c.descendants,c.override_children,c.overridden_parents,%s AS risk_score,%s AS risk_tier FROM counts c"
			") "
			"SELECT *, MAX(risk_score, MIN(override_children,30)/30.0, MIN(lines,500)/500.0) AS hotspot_score FROM scored %s%sLIMIT %d;"),
			OverrideSeedLimit, *RiskScoreExpr, *RiskTierExpr, *WhereClause, *OrderBy, FetchLimit);
		}
	}
	else if (bHasCrg)
	{
		Sql = FString::Printf(TEXT(
		"WITH scored AS ("
		"  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
		"         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
		"         COALESCE(m.fan_in,0) AS fan_in,COALESCE(m.fan_out,0) AS fan_out,"
		"         COALESCE(m.descendants,0) AS descendants,0 AS override_children,0 AS overridden_parents,"
		"         COALESCE(m.risk_score,0.0) AS risk_score,"
		"         COALESCE(m.risk_tier, 'low') AS risk_tier "
		"  FROM symbols s "
		"  LEFT JOIN files f ON f.id=s.file_id "
		"  LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=s.id "
		"  LEFT JOIN crg_node_metrics m ON m.node_id=n.id"
		") "
		"SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score "
		"FROM scored %s%sLIMIT %d;"),
		*WhereClause, *OrderBy, FetchLimit);
	}
	else
	{
		const FString RiskScoreExpr = TEXT("c.estimated_risk");
		const FString RiskTierExpr = TEXT("CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END");
		Sql = FString::Printf(TEXT(
		"WITH ref_in AS ("
		"  SELECT to_symbol_id AS symbol_id, COUNT(*) AS fan_in, COUNT(DISTINCT r.file_id) AS caller_files "
		"  FROM \"references\" r JOIN symbols fs ON fs.id=r.from_symbol_id JOIN symbols ts ON ts.id=r.to_symbol_id GROUP BY to_symbol_id"
		"), ref_out AS ("
		"  SELECT from_symbol_id AS symbol_id, COUNT(*) AS fan_out "
		"  FROM \"references\" r JOIN symbols fs ON fs.id=r.from_symbol_id JOIN symbols ts ON ts.id=r.to_symbol_id GROUP BY from_symbol_id"
		"), inh_desc AS ("
		"  SELECT parent_id AS symbol_id, COUNT(*) AS descendants FROM inheritance i "
		"  JOIN symbols cs ON cs.id=i.child_id JOIN symbols ps ON ps.id=i.parent_id GROUP BY parent_id"
		"), base AS ("
		"  SELECT s.id,s.name,s.qualified_name,s.kind,COALESCE(f.path,'') AS file,s.line_start,s.line_end,"
		"         CASE WHEN s.line_end >= s.line_start THEN s.line_end - s.line_start + 1 ELSE 0 END AS lines,"
		"         COALESCE(ri.fan_in,0) AS fan_in,COALESCE(ro.fan_out,0) AS fan_out,"
		"         COALESCE(id.descendants,0) AS descendants,COALESCE(ri.caller_files,0) AS caller_files,"
		"         0 AS override_children,0 AS overridden_parents,"
		"         s.is_ue_macro AS is_ue_macro,"
		"         CASE WHEN lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%ufunction%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%server%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%client%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%netmulticast%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%save%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%serialize%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%auth%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%purchase%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%anticheat%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%crypt%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%exec%%'"
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%%file%%'"
		"          THEN 1 ELSE 0 END AS sensitivity "
		"  FROM symbols s LEFT JOIN files f ON f.id=s.file_id "
		"  LEFT JOIN ref_in ri ON ri.symbol_id=s.id LEFT JOIN ref_out ro ON ro.symbol_id=s.id LEFT JOIN inh_desc id ON id.symbol_id=s.id"
		"), counts AS ("
		"  SELECT *, MIN(1.0, MIN(fan_in,50)/50.0*0.35 + MIN(descendants,30)/30.0*0.25 + "
		"         MIN(fan_out,50)/50.0*0.10 + CASE WHEN is_ue_macro != 0 THEN 0.15 ELSE 0 END + "
		"         MIN(caller_files,20)/20.0*0.15 + CASE WHEN sensitivity != 0 THEN 0.15 ELSE 0 END) AS estimated_risk "
		"  FROM base"
		"), scored AS ("
		"  SELECT c.id,c.name,c.qualified_name,c.kind,c.file,c.line_start,c.line_end,c.lines,"
		"         c.fan_in,c.fan_out,c.descendants,c.override_children,c.overridden_parents,%s AS risk_score,%s AS risk_tier "
		"  FROM counts c"
		") "
		"SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score "
		"FROM scored %s%sLIMIT %d;"),
		*RiskScoreExpr, *RiskTierExpr, *WhereClause, *OrderBy, FetchLimit);
	}

	FSQLitePreparedStatement S;
	TArray<TSharedPtr<FJsonValue>> Hotspots;
	TArray<TSharedPtr<FJsonValue>> Questions;
	bool bTruncated = false;
	if (S.Create(*Database, *Sql))
	{
		while (S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Id = 0;
			FString Name, QualifiedName, SymKind, File, Tier;
			int32 LineStart = 0, LineEnd = 0, Lines = 0, FanIn = 0, FanOut = 0, Desc = 0, OverrideChildren = 0, OverriddenParents = 0;
			double Risk = 0.0;
			S.GetColumnValueByIndex(0, Id);
			S.GetColumnValueByIndex(1, Name);
			S.GetColumnValueByIndex(2, QualifiedName);
			S.GetColumnValueByIndex(3, SymKind);
			S.GetColumnValueByIndex(4, File);
			S.GetColumnValueByIndex(5, LineStart);
			S.GetColumnValueByIndex(6, LineEnd);
			S.GetColumnValueByIndex(7, Lines);
			S.GetColumnValueByIndex(8, FanIn);
			S.GetColumnValueByIndex(9, FanOut);
			S.GetColumnValueByIndex(10, Desc);
			S.GetColumnValueByIndex(11, OverrideChildren);
			S.GetColumnValueByIndex(12, OverriddenParents);
			S.GetColumnValueByIndex(13, Risk);
			S.GetColumnValueByIndex(14, Tier);

			if (NormalizedKind == TEXT("override") && !bHasOverrideCache)
			{
				OverrideChildren = CountOverrideEdgesToUnlocked(*Database, Id, 1000);
				OverriddenParents = CountOverrideEdgesFromUnlocked(*Database, Id, 1000);
			}
			if (NormalizedKind == TEXT("override") && OverrideChildren <= 0 && OverriddenParents <= 0)
			{
				continue;
			}
			Risk = FMath::Min(1.0, Risk
				+ FMath::Min<double>(OverrideChildren, 30) / 30.0 * 0.20
				+ FMath::Min<double>(OverriddenParents, 10) / 10.0 * 0.05);
			Tier = Risk >= 0.66 ? TEXT("high") : (Risk >= 0.33 ? TEXT("medium") : TEXT("low"));
			if (Hotspots.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}

			FString Primary = NormalizedKind;
			if (Primary == TEXT("all"))
			{
				const double InSignal = FMath::Min<double>(FanIn, 50) / 50.0;
				const double OutSignal = FMath::Min<double>(FanOut, 50) / 50.0;
				const double OverrideSignal = FMath::Max(
					FMath::Min<double>(OverrideChildren, 30) / 30.0,
					FMath::Min<double>(OverriddenParents, 10) / 10.0);
				const double LargeSignal = FMath::Min<double>(Lines, 500) / 500.0;
				Primary = TEXT("risk");
				double Best = Risk;
				if (InSignal > Best) { Best = InSignal; Primary = TEXT("fan_in"); }
				if (OutSignal > Best) { Best = OutSignal; Primary = TEXT("fan_out"); }
				if (OverrideSignal > Best) { Best = OverrideSignal; Primary = TEXT("override"); }
				if (LargeSignal > Best) { Primary = TEXT("large"); }
			}

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("primary_kind"), Primary);
			O->SetNumberField(TEXT("id"), static_cast<double>(Id));
			O->SetStringField(TEXT("name"), Name);
			O->SetStringField(TEXT("qualified_name"), QualifiedName);
			O->SetStringField(TEXT("kind"), SymKind);
			O->SetStringField(TEXT("file"), File);
			O->SetNumberField(TEXT("line_start"), LineStart);
			O->SetNumberField(TEXT("line_end"), LineEnd);
			TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
			Metrics->SetNumberField(TEXT("fan_in"), FanIn);
			Metrics->SetNumberField(TEXT("fan_out"), FanOut);
			Metrics->SetNumberField(TEXT("descendants"), Desc);
			Metrics->SetNumberField(TEXT("override_children"), OverrideChildren);
			Metrics->SetNumberField(TEXT("overridden_parents"), OverriddenParents);
			Metrics->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(Risk * 1000.0) / 1000.0);
			Metrics->SetStringField(TEXT("risk_tier"), Tier);
			Metrics->SetNumberField(TEXT("lines"), Lines);
			O->SetObjectField(TEXT("signals"), Metrics);
			O->SetObjectField(TEXT("metrics"), Metrics);
			Hotspots.Add(MakeShared<FJsonValueObject>(O));

			if (bIncludeQuestions && Questions.Num() < 5)
			{
				TSharedPtr<FJsonObject> Q = MakeShared<FJsonObject>();
				Q->SetStringField(TEXT("target"), QualifiedName.IsEmpty() ? Name : QualifiedName);
				Q->SetStringField(TEXT("reason"), Primary);
				Q->SetStringField(TEXT("question"), Primary == TEXT("large")
					? TEXT("Can this large symbol be split or covered by focused tests before risky edits?")
					: (Primary == TEXT("override")
						? TEXT("Which child overrides and parent contracts must be reviewed before changing this method?")
						: TEXT("Which callers and tests cover this hotspot before changing it?")));
				Questions.Add(MakeShared<FJsonValueObject>(Q));
			}
		}
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d source review hotspot(s) ranked by %s%s"),
		Hotspots.Num(), *NormalizedKind, bHasCrg ? TEXT(" using CRG cache when available") : TEXT(" using native fallback")));
	TSharedPtr<FJsonObject> Cache = MakeShared<FJsonObject>();
	Cache->SetStringField(TEXT("status"), bHasCrg ? TEXT("hit") : TEXT("miss"));
	Cache->SetStringField(TEXT("source"), bHasOverrideCache ? TEXT("crg_node_metrics + source_override_edges")
		: (bHasCrg ? TEXT("crg_node_metrics + query-time override aggregation") : TEXT("query-time references/inheritance/override aggregation")));
	Root->SetObjectField(TEXT("cache"), Cache);
	Root->SetArrayField(TEXT("hotspots"), Hotspots);
	if (bIncludeQuestions)
	{
		Root->SetArrayField(TEXT("questions"), Questions);
	}
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNextActions(Root, { TEXT("source.review_context"), TEXT("source.find_overrides"), TEXT("source.risk_score"), TEXT("source.impact_radius") });
	return Root;
}

// ============================================================
// Insert helpers
// ============================================================

int64 FMonolithSourceDatabase::InsertModule(const FString& Name, const FString& Path, const FString& ModuleType, const FString& BuildCsPath)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement InsStmt;
	if (!InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO modules (name, path, module_type, build_cs_path) VALUES (?, ?, ?, ?);")))
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("InsertModule: insert statement failed for '%s': %s"), *Name, *Database->GetLastError());
	}
	else
	{
		InsStmt.SetBindingValueByIndex(1, Name);
		InsStmt.SetBindingValueByIndex(2, Path);
		InsStmt.SetBindingValueByIndex(3, ModuleType);
		InsStmt.SetBindingValueByIndex(4, BuildCsPath);
		if (InsStmt.Step() != ESQLitePreparedStatementStepResult::Done)
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("InsertModule: insert failed for '%s': %s"), *Name, *Database->GetLastError());
		}
	}

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
	if (!InsStmt.Create(*Database, TEXT("INSERT OR IGNORE INTO files (path, module_id, file_type, line_count, last_modified) VALUES (?, ?, ?, ?, ?);")))
	{
		UE_LOG(LogMonolithSource, Warning, TEXT("InsertFile: insert statement failed for '%s': %s"), *FilePath, *Database->GetLastError());
	}
	else
	{
		InsStmt.SetBindingValueByIndex(1, FilePath);
		InsStmt.SetBindingValueByIndex(2, ModuleId);
		InsStmt.SetBindingValueByIndex(3, FileType);
		InsStmt.SetBindingValueByIndex(4, static_cast<int64>(LineCount));
		InsStmt.SetBindingValueByIndex(5, LastModified);
		if (InsStmt.Step() != ESQLitePreparedStatementStepResult::Done)
		{
			UE_LOG(LogMonolithSource, Warning, TEXT("InsertFile: insert failed for '%s': %s"), *FilePath, *Database->GetLastError());
		}
	}

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
// Deprecation queries (item 3)
// ============================================================

void FMonolithSourceDatabase::InsertDeprecation(int64 SymbolId, const FString& SymbolName, const FString& Version, const FString& Message, const FString& Kind)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database,
		TEXT("INSERT INTO symbol_deprecations (symbol_id, symbol_name, version, message, kind) ")
		TEXT("VALUES (?, ?, ?, ?, ?);"));

	// symbol_id — bind NULL if 0 (class-body methods have no symbols row)
	if (SymbolId != 0)
	{
		Stmt.SetBindingValueByIndex(1, SymbolId);
	}
	// else: leave unbound — SQLite defaults to NULL

	Stmt.SetBindingValueByIndex(2, SymbolName);
	Stmt.SetBindingValueByIndex(3, Version);
	Stmt.SetBindingValueByIndex(4, Message);
	Stmt.SetBindingValueByIndex(5, Kind);
	Stmt.Step();
}

TOptional<FMonolithDeprecationRow> FMonolithSourceDatabase::GetDeprecation(const FString& SymbolName)
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return {};

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1;"));
	Stmt.SetBindingValueByIndex(1, SymbolName);

	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FMonolithDeprecationRow Row;
		Stmt.GetColumnValueByIndex(0, Row.Version);
		Stmt.GetColumnValueByIndex(1, Row.Message);
		Stmt.GetColumnValueByIndex(2, Row.Kind);
		return Row;
	}
	return {};
}

TMap<FString, FMonolithDeprecationRow> FMonolithSourceDatabase::GetDeprecationsBatch(const TArray<FString>& SymbolNames)
{
	FScopeLock Lock(&DbLock);
	TMap<FString, FMonolithDeprecationRow> Result;
	if (!Database || !Database->IsValid()) return Result;

	// One prepared statement reused per name — symbol counts are typically small.
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT version, message, kind FROM symbol_deprecations WHERE symbol_name = ? LIMIT 1;"));

	for (const FString& Name : SymbolNames)
	{
		Stmt.Reset();
		Stmt.SetBindingValueByIndex(1, Name);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FMonolithDeprecationRow Row;
			Stmt.GetColumnValueByIndex(0, Row.Version);
			Stmt.GetColumnValueByIndex(1, Row.Message);
			Stmt.GetColumnValueByIndex(2, Row.Kind);
			Result.Add(Name, Row);
		}
	}
	return Result;
}

int32 FMonolithSourceDatabase::GetDeprecationCount()
{
	FScopeLock Lock(&DbLock);
	if (!Database || !Database->IsValid()) return 0;

	FSQLitePreparedStatement Stmt;
	Stmt.Create(*Database, TEXT("SELECT COUNT(*) FROM symbol_deprecations;"));
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 C64 = 0;
		Stmt.GetColumnValueByIndex(0, C64);
		return static_cast<int32>(C64);
	}
	return 0;
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
