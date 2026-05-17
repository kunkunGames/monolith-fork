#include "MonolithSourceDatabase.h"
#include "MonolithSourceSchema.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <initializer_list>

DEFINE_LOG_CATEGORY(LogMonolithSource);

static FString MakeAutoSnapshotLabel(const TCHAR* Prefix)
{
	return FString::Printf(TEXT("%s-%lld"), Prefix, FDateTime::UtcNow().GetTicks());
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

static bool ContainsAnyToken(const FString& LowerText, std::initializer_list<const TCHAR*> Tokens)
{
	for (const TCHAR* Token : Tokens)
	{
		if (LowerText.Contains(FString(Token)))
		{
			return true;
		}
	}
	return false;
}

static double SourceSensitivityFactor(const FString& Text, FString& OutReason)
{
	const FString Lower = Text.ToLower();
	if (ContainsAnyToken(Lower, { TEXT("ufunction"), TEXT("server"), TEXT("client"), TEXT("netmulticast"), TEXT("onrep"), TEXT("replication"), TEXT("rpc"), TEXT("network") }))
	{
		OutReason = TEXT("sensitivity: replication/RPC or network surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("save"), TEXT("serialize"), TEXT("archive") }))
	{
		OutReason = TEXT("sensitivity: save/serialization surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("auth"), TEXT("login"), TEXT("account"), TEXT("session") }))
	{
		OutReason = TEXT("sensitivity: auth/account/session surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("purchase"), TEXT("iap"), TEXT("store"), TEXT("entitlement") }))
	{
		OutReason = TEXT("sensitivity: purchase/store entitlement surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("anticheat"), TEXT("anti_cheat"), TEXT("cheat") }))
	{
		OutReason = TEXT("sensitivity: anticheat surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("crypt"), TEXT("encrypt"), TEXT("decrypt"), TEXT("sign"), TEXT("hash") }))
	{
		OutReason = TEXT("sensitivity: crypto/signing/hash surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("exec"), TEXT("eval"), TEXT("command") }))
	{
		OutReason = TEXT("sensitivity: exec/eval/command surface");
		return 0.15;
	}
	if (ContainsAnyToken(Lower, { TEXT("file"), TEXT("registry"), TEXT("process") }))
	{
		OutReason = TEXT("sensitivity: file/registry/process surface");
		return 0.15;
	}
	return 0.0;
}

static TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
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
		if (Value.IsValid())
		{
			Out.Add(Value->AsString());
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
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_domain_native ON crg_nodes(domain, native_table, native_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_stable ON crg_nodes(domain, stable_key);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_source ON crg_edges(domain, source_node_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_target ON crg_edges(domain, target_node_id);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_kind_subkind ON crg_edges(domain, edge_kind, edge_subkind);")
	TEXT("CREATE INDEX IF NOT EXISTS idx_crg_metrics_score ON crg_node_metrics(risk_score DESC);");

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

	bool bHasAllCrg = true;
	for (const TCHAR* T : { TEXT("crg_nodes"), TEXT("crg_edges"), TEXT("crg_node_metrics"), TEXT("crg_meta") })
	{
		const bool bHas = Exists(TEXT("table"), T);
		bHasAllCrg = bHasAllCrg && bHas;
		Check(FString::Printf(TEXT("crg:table:%s"), T), bHas,
			bHas ? FString::Printf(TEXT("CRG projection table %s present"), T)
				: FString::Printf(TEXT("missing CRG projection table %s (run source.repair_crg_cache)"), T));
	}
	for (const TCHAR* I : {
		TEXT("idx_crg_nodes_domain_native"), TEXT("idx_crg_nodes_stable"),
		TEXT("idx_crg_edges_domain_source"), TEXT("idx_crg_edges_domain_target"),
		TEXT("idx_crg_edges_kind_subkind"), TEXT("idx_crg_metrics_score") })
	{
		const bool bHas = Exists(TEXT("index"), I);
		Check(FString::Printf(TEXT("crg:index:%s"), I), bHas,
			bHas ? FString::Printf(TEXT("CRG projection index %s present"), I)
				: FString::Printf(TEXT("missing CRG projection index %s (run source.repair_crg_cache)"), I));
	}
	int64 CrgNodeCnt = -1;
	int64 CrgEdgeCnt = -1;
	int64 CrgMetricCnt = -1;
	if (bHasAllCrg)
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
		Check(TEXT("crg:scoring_version"), CrgScoringVersion == TEXT("3"),
			CrgScoringVersion.IsEmpty() ? TEXT("crg_meta.scoring_version missing (run source.repair_crg_cache)")
				: FString::Printf(TEXT("crg scoring_version=%s (expected 3)"), *CrgScoringVersion));
	}

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
		if (bHasAllCrg)
		{
			Counts->SetNumberField(TEXT("crg_nodes"), static_cast<double>(CrgNodeCnt));
			Counts->SetNumberField(TEXT("crg_edges"), static_cast<double>(CrgEdgeCnt));
			Counts->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(CrgMetricCnt));
		}
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
	AddNextActions(Root, { TEXT("source.repair_crg_cache"), TEXT("source.repair_fts"), TEXT("source.trigger_project_reindex"), TEXT("source.search_source") });
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

TSharedPtr<FJsonObject> FMonolithSourceDatabase::RepairCrgCache(bool bExecute)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("limits"), Limits);

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> Plan;
	Plan.Add(MakeShared<FJsonValueString>(TEXT("CREATE IF MISSING crg_nodes/crg_edges/crg_node_metrics/crg_meta")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("DELETE existing source CRG projection rows")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("SOURCE symbols -> crg_nodes; references/inheritance -> crg_edges")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("Recompute caller/callee/descendant/risk_score into crg_node_metrics")));
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
	}
	Root->SetObjectField(TEXT("before"), Before);

	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"),
			TEXT("Dry-run: source CRG projection/cache would be rebuilt. Pass execute=true to apply."));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNextActions(Root, { TEXT("source.repair_crg_cache (execute=true)"), TEXT("source.health"), TEXT("source.risk_score") });
		return Root;
	}

	bool bOk = ExecuteMulti(*Database, GCrgProjectionDdl);
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

	if (bOk)
	{
		bOk = Database->Execute(TEXT("BEGIN;"));
	}
	Exec(TEXT("DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM crg_nodes WHERE domain = 'source');"), TEXT("clear metrics"));
	Exec(TEXT("DELETE FROM crg_edges WHERE domain = 'source';"), TEXT("clear edges"));
	Exec(TEXT("DELETE FROM crg_nodes WHERE domain = 'source';"), TEXT("clear nodes"));
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
		"            OR lower(COALESCE(s.qualified_name,'') || ' ' || COALESCE(s.name,'') || ' ' || COALESCE(s.signature,'')) LIKE '%sign%'"
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
		"         printf('[\"caller fan-in: %d\",\"inheritance descendants (1-hop): %d\",\"callee fan-out: %d\",\"module/file boundary crossing: %d distinct caller file(s)\",\"sensitivity: UE-domain sensitive surface\"]',"
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

	if (bOk) Database->Execute(TEXT("COMMIT;"));
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

TSharedPtr<FJsonObject> FMonolithSourceDatabase::DetectChanges(
	const TArray<FString>& ChangedPaths,
	int32 MaxResults,
	const FString& DetailLevel)
{
	FScopeLock Lock(&DbLock);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Cap = FMath::Clamp(MaxResults <= 0 ? 200 : MaxResults, 1, 2000);
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
		Root->SetStringField(TEXT("summary"), TEXT("changed_paths or paths must include at least one path"));
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

		FSQLitePreparedStatement S;
		if (!S.Create(*Database, TEXT(
			"SELECT s.id,s.name,s.qualified_name,s.kind,s.file_id,COALESCE(f.path,''),"
			"       s.line_start,s.line_end,COALESCE(s.signature,''),s.is_ue_macro "
			"FROM symbols s JOIN files f ON f.id = s.file_id "
			"WHERE replace(f.path,'\\','/') LIKE ? ESCAPE '\\' "
			"ORDER BY s.id LIMIT ?;")))
		{
			continue;
		}
		S.SetBindingValueByIndex(1, FString::Printf(TEXT("%%%s"), *EscapedPath));
		S.SetBindingValueByIndex(2, static_cast<int64>(Cap + 1));

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
		if (!Changed.IsValid() || !Changed->GetStringField(TEXT("kind")).Contains(TEXT("function")))
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
		Gap->SetStringField(TEXT("name"), Changed->GetStringField(TEXT("name")));
		Gap->SetStringField(TEXT("qualified_name"), Changed->GetStringField(TEXT("qualified_name")));
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

	TSharedPtr<FJsonObject> HealthResult = ComputeHealth(false);
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
		&& NormalizedKind != TEXT("all"))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Unsupported kind for source.review_hotspots (expected fan_in|fan_out|risk|large|all)"));
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

	const FString CacheJoin = bHasCrg
		? TEXT("LEFT JOIN crg_nodes n ON n.domain='source' AND n.native_table='symbols' AND n.native_id=c.id "
			"LEFT JOIN crg_node_metrics m ON m.node_id=n.id ")
		: TEXT("");
	const FString RiskScoreExpr = bHasCrg
		? TEXT("COALESCE(m.risk_score, c.estimated_risk)")
		: TEXT("c.estimated_risk");
	const FString RiskTierExpr = bHasCrg
		? TEXT("COALESCE(m.risk_tier, CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END)")
		: TEXT("CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END");
	const FString WhereClause = NormalizedKind == TEXT("large")
		? FString::Printf(TEXT("WHERE lines >= %d "), LocFloor)
		: TEXT("WHERE fan_in > 0 OR fan_out > 0 OR descendants > 0 OR risk_score > 0 OR lines >= ") + FString::FromInt(LocFloor) + TEXT(" ");
	FString OrderBy = TEXT("ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, lines DESC ");
	if (NormalizedKind == TEXT("fan_in")) OrderBy = TEXT("ORDER BY fan_in DESC, risk_score DESC, lines DESC ");
	else if (NormalizedKind == TEXT("fan_out")) OrderBy = TEXT("ORDER BY fan_out DESC, risk_score DESC, lines DESC ");
	else if (NormalizedKind == TEXT("risk")) OrderBy = TEXT("ORDER BY risk_score DESC, fan_in DESC, lines DESC ");
	else if (NormalizedKind == TEXT("large")) OrderBy = TEXT("ORDER BY lines DESC, risk_score DESC, fan_in DESC ");

	const FString Sql = FString::Printf(TEXT(
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
		"         c.fan_in,c.fan_out,c.descendants,%s AS risk_score,%s AS risk_tier "
		"  FROM counts c %s"
		") "
		"SELECT *, MAX(risk_score, MIN(fan_in,50)/50.0, MIN(fan_out,50)/50.0, MIN(lines,500)/500.0) AS hotspot_score "
		"FROM scored %s%sLIMIT %d;"),
		*RiskScoreExpr, *RiskTierExpr, *CacheJoin, *WhereClause, *OrderBy, Cap + 1);

	FSQLitePreparedStatement S;
	TArray<TSharedPtr<FJsonValue>> Hotspots;
	TArray<TSharedPtr<FJsonValue>> Questions;
	bool bTruncated = false;
	if (S.Create(*Database, *Sql))
	{
		while (S.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			if (Hotspots.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}
			int64 Id = 0;
			FString Name, QualifiedName, SymKind, File, Tier;
			int32 LineStart = 0, LineEnd = 0, Lines = 0, FanIn = 0, FanOut = 0, Desc = 0;
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
			S.GetColumnValueByIndex(11, Risk);
			S.GetColumnValueByIndex(12, Tier);

			FString Primary = NormalizedKind;
			if (Primary == TEXT("all"))
			{
				const double InSignal = FMath::Min<double>(FanIn, 50) / 50.0;
				const double OutSignal = FMath::Min<double>(FanOut, 50) / 50.0;
				const double LargeSignal = FMath::Min<double>(Lines, 500) / 500.0;
				Primary = TEXT("risk");
				double Best = Risk;
				if (InSignal > Best) { Best = InSignal; Primary = TEXT("fan_in"); }
				if (OutSignal > Best) { Best = OutSignal; Primary = TEXT("fan_out"); }
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
					: TEXT("Which callers and tests cover this hotspot before changing it?"));
				Questions.Add(MakeShared<FJsonValueObject>(Q));
			}
		}
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d source review hotspot(s) ranked by %s%s"),
		Hotspots.Num(), *NormalizedKind, bHasCrg ? TEXT(" using CRG cache when available") : TEXT(" using native fallback")));
	Root->SetArrayField(TEXT("hotspots"), Hotspots);
	if (bIncludeQuestions)
	{
		Root->SetArrayField(TEXT("questions"), Questions);
	}
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNextActions(Root, { TEXT("source.review_context"), TEXT("source.risk_score"), TEXT("source.impact_radius") });
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
