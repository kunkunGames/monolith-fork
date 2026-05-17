#include "MonolithIndexReview.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexLog.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <initializer_list>

// ============================================================================
// Local helpers
// ============================================================================
namespace
{
	using FJsonArr = TArray<TSharedPtr<FJsonValue>>;
	constexpr const TCHAR* ExpectedScoringVersion = TEXT("3");

	int64 CountRows(FMonolithIndexDatabase& Db, const FString& Sql)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return -1;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, *Sql)) return -1;
		int64 Count = 0;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Count);
		}
		return Count;
	}

	FString ScalarStr(FMonolithIndexDatabase& Db, const FString& Sql)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return FString();
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, *Sql)) return FString();
		FString Out;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, Out);
		}
		return Out;
	}

	bool ObjectExists(FMonolithIndexDatabase& Db, const FString& Type, const FString& Name)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return false;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("SELECT 1 FROM sqlite_master WHERE type = ? AND name = ?;")))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, Type);
		Stmt.SetBindingValueByIndex(2, Name);
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	}

	/** id -> compact asset object; empty shared ptr if not found. */
	TSharedPtr<FJsonObject> AssetJsonById(FMonolithIndexDatabase& Db, int64 AssetId)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return nullptr;
		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT("SELECT package_path, asset_name, asset_class, module_name FROM assets WHERE id = ?;")))
		{
			return nullptr;
		}
		Stmt.SetBindingValueByIndex(1, AssetId);
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return nullptr;
		}
		FString Path, Name, Class, Module;
		Stmt.GetColumnValueByIndex(0, Path);
		Stmt.GetColumnValueByIndex(1, Name);
		Stmt.GetColumnValueByIndex(2, Class);
		Stmt.GetColumnValueByIndex(3, Module);
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("id"), static_cast<double>(AssetId));
		O->SetStringField(TEXT("asset_path"), Path);
		O->SetStringField(TEXT("asset_name"), Name);
		O->SetStringField(TEXT("asset_class"), Class);
		O->SetStringField(TEXT("module_name"), Module);
		return O;
	}

	int32 AssetClassWeight(const FString& AssetClass)
	{
		// UE-domain risk weighting. Sensitivity is a separate decomposed factor.
		if (AssetClass.Contains(TEXT("World")) || AssetClass.Contains(TEXT("Level"))) return 5;
		if (AssetClass.Contains(TEXT("GameplayAbility")) || AssetClass.Contains(TEXT("AttributeSet"))
			|| AssetClass.Contains(TEXT("GameplayEffect"))) return 4;
		if (AssetClass.Contains(TEXT("Blueprint"))) return 3;
		if (AssetClass.Contains(TEXT("NiagaraSystem")) || AssetClass.Contains(TEXT("Material"))) return 2;
		if (AssetClass.Contains(TEXT("DataTable")) || AssetClass.Contains(TEXT("DataAsset"))) return 2;
		return 1;
	}

	bool ContainsAnyToken(const FString& LowerText, std::initializer_list<const TCHAR*> Tokens)
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

	double SensitivityFactor(const FString& Text, FString& OutReason)
	{
		const FString Lower = Text.ToLower();
		if (ContainsAnyToken(Lower, { TEXT("replication"), TEXT("network"), TEXT("rpc"), TEXT("netmulticast"), TEXT("onrep"), TEXT("server"), TEXT("client") }))
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

	FString TierFor(double Score)
	{
		if (Score >= 0.66) return TEXT("high");
		if (Score >= 0.33) return TEXT("medium");
		return TEXT("low");
	}

	int32 TierRank(const FString& Tier)
	{
		if (Tier == TEXT("high")) return 2;
		if (Tier == TEXT("medium")) return 1;
		return 0;
	}

	int32 ConfidenceRank(const FString& Confidence)
	{
		if (Confidence == TEXT("high")) return 2;
		if (Confidence == TEXT("medium")) return 1;
		return 0;
	}

	FString ConfidenceForUnusedAssetClass(const FString& AssetClass)
	{
		if (AssetClass.Contains(TEXT("Texture")) || AssetClass.Contains(TEXT("Material"))
			|| AssetClass.Contains(TEXT("Sound")) || AssetClass.Contains(TEXT("DataTable"))
			|| AssetClass.Contains(TEXT("DataAsset")) || AssetClass.Contains(TEXT("Paper")))
		{
			return TEXT("low");
		}
		return TEXT("medium");
	}

	FJsonArr TopRiskReasons(const TSharedPtr<FJsonObject>& Risk, int32 MaxItems)
	{
		FJsonArr Out;
		const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
		if (Risk.IsValid() && Risk->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons)
		{
			for (int32 i = 0; i < Reasons->Num() && i < MaxItems; ++i)
			{
				Out.Add((*Reasons)[i]);
			}
		}
		return Out;
	}

	void AddNext(const TSharedPtr<FJsonObject>& Root, std::initializer_list<const TCHAR*> Actions)
	{
		FJsonArr Arr;
		for (const TCHAR* A : Actions)
		{
			Arr.Add(MakeShared<FJsonValueString>(FString(A)));
		}
		Root->SetArrayField(TEXT("next_actions"), Arr);
	}

	FJsonArr StringArray(const TArray<FString>& Values)
	{
		FJsonArr Arr;
		for (const FString& Value : Values)
		{
			Arr.Add(MakeShared<FJsonValueString>(Value));
		}
		return Arr;
	}

	FString NormalizeChangedPath(FString Path)
	{
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Path;
	}

	double JsonScore(const TSharedPtr<FJsonObject>& Object)
	{
		double Score = 0.0;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(TEXT("score"), Score);
		}
		return Score;
	}

	TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json);

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

	FJsonArr SetToJsonArray(const TSet<FString>& Values)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		FJsonArr Arr;
		for (const FString& Value : Sorted)
		{
			Arr.Add(MakeShared<FJsonValueString>(Value));
		}
		return Arr;
	}

	bool JsonArrayToSet(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TSet<FString>& Out)
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

	FString SerializeManifest(const FSnapshotManifest& Manifest)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetArrayField(TEXT("nodes"), SetToJsonArray(Manifest.Nodes));
		Object->SetArrayField(TEXT("edges"), SetToJsonArray(Manifest.Edges));
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	bool ParseManifest(const FString& Json, FSnapshotManifest& Out)
	{
		TSharedPtr<FJsonObject> Object = ParseJsonObject(Json);
		if (!Object.IsValid())
		{
			return false;
		}
		return JsonArrayToSet(Object, TEXT("nodes"), Out.Nodes)
			&& JsonArrayToSet(Object, TEXT("edges"), Out.Edges);
	}

	bool EnsureSnapshotTable(FSQLiteDatabase& Raw)
	{
		return Raw.Execute(TEXT(
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

	bool LoadCurrentManifest(FSQLiteDatabase& Raw, const TCHAR* Domain, FSnapshotManifest& Out)
	{
		FSQLitePreparedStatement NodeStmt;
		if (!NodeStmt.Create(Raw, TEXT(
			"SELECT stable_key FROM crg_nodes WHERE domain = ? ORDER BY stable_key;")))
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
		if (!EdgeStmt.Create(Raw, TEXT(
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
			Out.Edges.Add(FString::Printf(TEXT("%s|%s|%s|%s"), *Source, *Target, *Kind, *Subkind));
		}
		return true;
	}

	bool LoadSnapshotRecord(FSQLiteDatabase& Raw, const TCHAR* Domain, const FString& Ref, FSnapshotRecord& Out)
	{
		if (Ref.IsEmpty() || Ref.Equals(TEXT("current"), ESearchCase::IgnoreCase))
		{
			Out.Label = TEXT("current");
			return LoadCurrentManifest(Raw, Domain, Out.Manifest);
		}

		const bool bNumeric = Ref.IsNumeric();
		FSQLitePreparedStatement Stmt;
		const TCHAR* Sql = bNumeric
			? TEXT("SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND id = ? LIMIT 1;")
			: TEXT("SELECT id,label,manifest_json FROM crg_snapshots WHERE domain = ? AND label = ? LIMIT 1;");
		if (!Stmt.Create(Raw, Sql))
		{
			return false;
		}
		Stmt.SetBindingValueByIndex(1, FString(Domain));
		if (bNumeric)
		{
			Stmt.SetBindingValueByIndex(2, static_cast<int64>(FCString::Atoi64(*Ref)));
		}
		else
		{
			Stmt.SetBindingValueByIndex(2, Ref);
		}
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return false;
		}

		FString ManifestJson;
		Stmt.GetColumnValueByIndex(0, Out.Id);
		Stmt.GetColumnValueByIndex(1, Out.Label);
		Stmt.GetColumnValueByIndex(2, ManifestJson);
		return ParseManifest(ManifestJson, Out.Manifest);
	}

	FJsonArr TakeStringSamples(const TSet<FString>& Values, int32 Limit, bool& bTruncated)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		FJsonArr Arr;
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

	TSharedPtr<FJsonObject> EdgeObject(const FString& Key)
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

	FJsonArr TakeEdgeSamples(const TSet<FString>& Values, int32 Limit, bool& bTruncated)
	{
		TArray<FString> Sorted = Values.Array();
		Sorted.Sort();
		FJsonArr Arr;
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

	TSet<FString> SetDifference(const TSet<FString>& Left, const TSet<FString>& Right)
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

	bool ParseJsonArray(const FString& Json, FJsonArr& Out)
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Out);
	}

	TSharedPtr<FJsonObject> ParseJsonObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		return FJsonSerializer::Deserialize(Reader, Out) ? Out : nullptr;
	}

	TSharedPtr<FJsonObject> CacheMeta(const FString& Status, const FString& CacheVersion, const FString& ScoringVersion)
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

	static const TCHAR* CrgProjectionDdl[] = {
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
		TEXT(");"),
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
		TEXT(");"),
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
		TEXT(");"),
		TEXT("CREATE TABLE IF NOT EXISTS crg_meta (")
		TEXT("key TEXT PRIMARY KEY,")
		TEXT("value TEXT NOT NULL")
		TEXT(");"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_domain_native ON crg_nodes(domain, native_table, native_id);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_nodes_stable ON crg_nodes(domain, stable_key);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_source ON crg_edges(domain, source_node_id);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_domain_target ON crg_edges(domain, target_node_id);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_edges_kind_subkind ON crg_edges(domain, edge_kind, edge_subkind);"),
		TEXT("CREATE INDEX IF NOT EXISTS idx_crg_metrics_score ON crg_node_metrics(risk_score DESC);")
	};

	bool EnsureCrgProjectionTables(FMonolithIndexDatabase& Db)
	{
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return false;
		for (const TCHAR* Sql : CrgProjectionDdl)
		{
			if (!Raw->Execute(Sql))
			{
				UE_LOG(LogMonolithIndex, Warning, TEXT("Failed to create CRG projection object: %s"), Sql);
				return false;
			}
		}
		return true;
	}

	bool HasCrgProjectionTables(FMonolithIndexDatabase& Db)
	{
		return ObjectExists(Db, TEXT("table"), TEXT("crg_nodes"))
			&& ObjectExists(Db, TEXT("table"), TEXT("crg_edges"))
			&& ObjectExists(Db, TEXT("table"), TEXT("crg_node_metrics"))
			&& ObjectExists(Db, TEXT("table"), TEXT("crg_meta"));
	}

	TSharedPtr<FJsonObject> TryCachedAssetRisk(FMonolithIndexDatabase& Db, const FIndexedAsset& Asset)
	{
		if (!HasCrgProjectionTables(Db)) return nullptr;
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		if (!Raw) return nullptr;

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT(
			"SELECT m.risk_score, m.risk_tier, m.reasons_json, m.raw_counts_json, "
			"       m.scoring_version, COALESCE((SELECT value FROM crg_meta WHERE key = 'cache_version'), '1') "
			"FROM crg_nodes n "
			"JOIN crg_node_metrics m ON m.node_id = n.id "
			"WHERE n.domain = 'project' AND n.native_table = 'assets' AND n.native_id = ? "
			"LIMIT 1;")))
		{
			return nullptr;
		}
		Stmt.SetBindingValueByIndex(1, Asset.Id);
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			return nullptr;
		}

		double Score = 0.0;
		FString Tier;
		FString ReasonsJson;
		FString RawCountsJson;
		FString ScoringVersion;
		FString CacheVersion;
		Stmt.GetColumnValueByIndex(0, Score);
		Stmt.GetColumnValueByIndex(1, Tier);
		Stmt.GetColumnValueByIndex(2, ReasonsJson);
		Stmt.GetColumnValueByIndex(3, RawCountsJson);
		Stmt.GetColumnValueByIndex(4, ScoringVersion);
		Stmt.GetColumnValueByIndex(5, CacheVersion);

		FJsonArr Reasons;
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
		O->SetNumberField(TEXT("id"), static_cast<double>(Asset.Id));
		O->SetStringField(TEXT("asset_path"), Asset.PackagePath);
		O->SetStringField(TEXT("asset_name"), Asset.AssetName);
		O->SetStringField(TEXT("asset_class"), Asset.AssetClass);
		O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
		O->SetStringField(TEXT("tier"), Tier.IsEmpty() ? TierFor(Score) : Tier);
		O->SetArrayField(TEXT("reasons"), Reasons);
		O->SetObjectField(TEXT("raw_counts"), RawCounts);
		O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("hit"), CacheVersion, ScoringVersion));
		return O;
	}

	/** Single-asset risk computation shared by risk_score and review_context. */
	TSharedPtr<FJsonObject> ScoreAsset(FMonolithIndexDatabase& Db, const FIndexedAsset& Asset)
	{
		if (TSharedPtr<FJsonObject> Cached = TryCachedAssetRisk(Db, Asset))
		{
			return Cached;
		}

		const int64 Id = Asset.Id;
		const TArray<FIndexedDependency> Out = Db.GetDependenciesForAsset(Id);
		const TArray<FIndexedDependency> In = Db.GetReferencersOfAsset(Id);

		int32 HardIn = 0;
		for (const FIndexedDependency& D : In)
		{
			if (D.DependencyType == TEXT("Hard")) ++HardIn;
		}

		const int64 NodeCount = CountRows(Db,
			FString::Printf(TEXT("SELECT COUNT(*) FROM nodes WHERE asset_id = %lld;"), Id));
		const int64 VarCount = CountRows(Db,
			FString::Printf(TEXT("SELECT COUNT(*) FROM variables WHERE asset_id = %lld;"), Id));
		const int64 ParamCount = CountRows(Db,
			FString::Printf(TEXT("SELECT COUNT(*) FROM parameters WHERE asset_id = %lld;"), Id));
		const int64 TagRefs = CountRows(Db,
			FString::Printf(TEXT("SELECT COUNT(*) FROM tag_references WHERE asset_id = %lld;"), Id));

		const int32 ClassW = AssetClassWeight(Asset.AssetClass);
		FString SensitivityReason;
		const double Sensitivity = SensitivityFactor(
			FString::Printf(TEXT("%s %s %s"), *Asset.AssetClass, *Asset.PackagePath, *Asset.AssetName),
			SensitivityReason);

		// Transparent additive factors, each capped, normalized to 0..1.
		FJsonArr Reasons;
		double Raw = 0.0;

		auto Factor = [&](double Contribution, const FString& Why)
		{
			if (Contribution > 0.0)
			{
				Raw += Contribution;
				Reasons.Add(MakeShared<FJsonValueString>(Why));
			}
		};

		Factor(FMath::Min<double>(In.Num(), 30) / 30.0 * 0.30,
			FString::Printf(TEXT("inbound dependency fan-in: %d referencer(s)"), In.Num()));
		Factor(FMath::Min<double>(HardIn, 20) / 20.0 * 0.20,
			FString::Printf(TEXT("hard inbound dependencies: %d"), HardIn));
		Factor(FMath::Min<double>(Out.Num(), 30) / 30.0 * 0.10,
			FString::Printf(TEXT("outbound dependencies: %d"), Out.Num()));
		Factor((ClassW - 1) / 4.0 * 0.20,
			FString::Printf(TEXT("asset class weight: %s (w=%d)"), *Asset.AssetClass, ClassW));
		Factor(Sensitivity, SensitivityReason);
		Factor(FMath::Min<double>(NodeCount, 400) / 400.0 * 0.15,
			FString::Printf(TEXT("graph density: %lld node(s), %lld var(s), %lld param(s)"),
				NodeCount, VarCount, ParamCount));
		Factor(FMath::Min<double>(TagRefs, 20) / 20.0 * 0.05,
			FString::Printf(TEXT("gameplay tag involvement: %lld reference(s)"), TagRefs));

		if (NodeCount == 0 && Asset.AssetClass.Contains(TEXT("Blueprint")))
		{
			Reasons.Add(MakeShared<FJsonValueString>(
				TEXT("missing detail signal: Blueprint indexed with 0 graph nodes (stale index?)")));
		}

		const double Score = FMath::Clamp(Raw, 0.0, 1.0);

		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("id"), static_cast<double>(Id));
		O->SetStringField(TEXT("asset_path"), Asset.PackagePath);
		O->SetStringField(TEXT("asset_name"), Asset.AssetName);
		O->SetStringField(TEXT("asset_class"), Asset.AssetClass);
		O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
		O->SetStringField(TEXT("tier"), TierFor(Score));
		O->SetArrayField(TEXT("reasons"), Reasons);

		TSharedPtr<FJsonObject> RawCounts = MakeShared<FJsonObject>();
		RawCounts->SetNumberField(TEXT("inbound"), In.Num());
		RawCounts->SetNumberField(TEXT("inbound_hard"), HardIn);
		RawCounts->SetNumberField(TEXT("outbound"), Out.Num());
		RawCounts->SetNumberField(TEXT("nodes"), static_cast<double>(NodeCount));
		RawCounts->SetNumberField(TEXT("variables"), static_cast<double>(VarCount));
		RawCounts->SetNumberField(TEXT("parameters"), static_cast<double>(ParamCount));
		RawCounts->SetNumberField(TEXT("tag_references"), static_cast<double>(TagRefs));
		RawCounts->SetNumberField(TEXT("class_weight"), ClassW);
		RawCounts->SetNumberField(TEXT("sensitivity"), Sensitivity);
		O->SetObjectField(TEXT("raw_counts"), RawCounts);
		O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("miss"), TEXT(""), ExpectedScoringVersion));
		return O;
	}
} // namespace

// ============================================================================
// impact_radius
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::ImpactRadius(
	FMonolithIndexDatabase& Db,
	const FString& AssetPath,
	const FString& Direction,
	int32 MaxDepth,
	int32 MaxResults,
	const FString& DependencyType)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Depth = ClampDepth(MaxDepth);
	const int32 Limit = ClampResults(MaxResults);
	const bool bIn = Direction != TEXT("out");
	const bool bOut = Direction != TEXT("in");

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("asset_path"), AssetPath);
	Input->SetStringField(TEXT("direction"), Direction);
	if (!DependencyType.IsEmpty()) Input->SetStringField(TEXT("dependency_type"), DependencyType);
	Root->SetObjectField(TEXT("input"), Input);

	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_depth"), Depth);
	Limits->SetNumberField(TEXT("max_results"), Limit);
	Root->SetObjectField(TEXT("limits"), Limits);

	const int64 SeedId = Db.GetAssetId(AssetPath);
	if (SeedId <= 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Asset not found in ProjectIndex: %s"), *AssetPath));
		Root->SetBoolField(TEXT("truncated"), false);
		Root->SetArrayField(TEXT("impacted_assets"), FJsonArr());
		Root->SetArrayField(TEXT("edges"), FJsonArr());
		AddNext(Root, { TEXT("project.search"), TEXT("project.find_by_type") });
		return Root;
	}

	if (TSharedPtr<FJsonObject> Seed = AssetJsonById(Db, SeedId))
	{
		Root->SetObjectField(TEXT("seed"), Seed);
	}

	// Bounded BFS with a visited set (cycle-safe), level by level.
	TSet<int64> Visited;
	Visited.Add(SeedId);
	TArray<int64> Frontier;
	Frontier.Add(SeedId);

	FJsonArr ImpactedArr;
	FJsonArr EdgeArr;
	bool bTruncated = false;
	int32 Emitted = 0;

	for (int32 D = 1; D <= Depth && Frontier.Num() > 0 && !bTruncated; ++D)
	{
		TArray<int64> Next;
		for (int64 Cur : Frontier)
		{
			TArray<FIndexedDependency> Edges;
			if (bOut) Edges.Append(Db.GetDependenciesForAsset(Cur));
			if (bIn) Edges.Append(Db.GetReferencersOfAsset(Cur));

			for (const FIndexedDependency& E : Edges)
			{
				if (!DependencyType.IsEmpty() && E.DependencyType != DependencyType)
				{
					continue;
				}
				const int64 Other = (E.SourceAssetId == Cur) ? E.TargetAssetId : E.SourceAssetId;
				if (Other == Cur) continue;

				TSharedPtr<FJsonObject> Edge = MakeShared<FJsonObject>();
				Edge->SetNumberField(TEXT("from"), static_cast<double>(E.SourceAssetId));
				Edge->SetNumberField(TEXT("to"), static_cast<double>(E.TargetAssetId));
				Edge->SetStringField(TEXT("dependency_type"), E.DependencyType);
				Edge->SetNumberField(TEXT("depth"), D);
				EdgeArr.Add(MakeShared<FJsonValueObject>(Edge));

				if (Visited.Contains(Other)) continue;
				Visited.Add(Other);

				if (Emitted >= Limit)
				{
					bTruncated = true;
					break;
				}
				if (TSharedPtr<FJsonObject> AJson = AssetJsonById(Db, Other))
				{
					AJson->SetNumberField(TEXT("depth"), D);
					ImpactedArr.Add(MakeShared<FJsonValueObject>(AJson));
					++Emitted;
				}
				Next.Add(Other);
			}
			if (bTruncated) break;
		}
		Frontier = MoveTemp(Next);
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d impacted asset(s) within depth %d (%s) of %s%s"),
		Emitted, Depth, *Direction, *AssetPath,
		bTruncated ? TEXT(" [truncated]") : TEXT("")));
	Root->SetArrayField(TEXT("impacted_assets"), ImpactedArr);
	Root->SetArrayField(TEXT("edges"), EdgeArr);
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNext(Root, { TEXT("project.review_context"), TEXT("project.risk_score"), TEXT("project.get_asset_details") });
	return Root;
}

// ============================================================================
// health (read-only)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::Health(FMonolithIndexDatabase& Db, bool bIncludeCounts)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	FJsonArr Checks;
	FJsonArr Warnings;

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

	if (!Db.IsOpen() || !Db.GetRawDatabase())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		Root->SetArrayField(TEXT("checks"), Checks);
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.get_stats") });
		return Root;
	}

	static const TCHAR* ExpectedTables[] = {
		TEXT("assets"), TEXT("nodes"), TEXT("connections"), TEXT("variables"),
		TEXT("parameters"), TEXT("dependencies"), TEXT("actors"), TEXT("tags"),
		TEXT("tag_references"), TEXT("configs"), TEXT("cpp_symbols"),
		TEXT("datatable_rows"), TEXT("meta") };
	for (const TCHAR* T : ExpectedTables)
	{
		Check(FString::Printf(TEXT("table:%s"), T), ObjectExists(Db, TEXT("table"), T),
			ObjectExists(Db, TEXT("table"), T) ? FString::Printf(TEXT("table %s present"), T)
				: FString::Printf(TEXT("missing table %s"), T));
	}

	static const TCHAR* ExpectedFts[] = { TEXT("fts_assets"), TEXT("fts_nodes") };
	for (const TCHAR* F : ExpectedFts)
	{
		const bool bHas = ObjectExists(Db, TEXT("table"), F);
		Check(FString::Printf(TEXT("fts:%s"), F), bHas,
			bHas ? FString::Printf(TEXT("FTS table %s present"), F)
				: FString::Printf(TEXT("missing FTS table %s"), F));
	}

	static const TCHAR* ExpectedTriggers[] = {
		TEXT("fts_assets_ai"), TEXT("fts_assets_ad"), TEXT("fts_assets_au"),
		TEXT("fts_nodes_ai"), TEXT("fts_nodes_ad"), TEXT("fts_nodes_au") };
	for (const TCHAR* Tr : ExpectedTriggers)
	{
		const bool bHas = ObjectExists(Db, TEXT("trigger"), Tr);
		Check(FString::Printf(TEXT("trigger:%s"), Tr), bHas,
			bHas ? FString::Printf(TEXT("trigger %s present"), Tr)
				: FString::Printf(TEXT("missing trigger %s (FTS may drift)"), Tr));
	}

	// Schema v2: schema_version meta key + assets.saved_hash column.
	const FString SchemaVer = Db.ReadMeta(TEXT("schema_version"));
	Check(TEXT("meta:schema_version"), !SchemaVer.IsEmpty(),
		SchemaVer.IsEmpty() ? TEXT("meta.schema_version missing (pre-v2 / corrupt)")
			: FString::Printf(TEXT("schema_version=%s"), *SchemaVer));

	bool bSavedHash = false;
	{
		FSQLitePreparedStatement Stmt;
		if (Stmt.Create(*Db.GetRawDatabase(), TEXT("PRAGMA table_info(assets);")))
		{
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString ColName;
				Stmt.GetColumnValueByIndex(1, ColName);
				if (ColName == TEXT("saved_hash")) { bSavedHash = true; break; }
			}
		}
	}
	Check(TEXT("schema:assets.saved_hash"), bSavedHash,
		bSavedHash ? TEXT("assets.saved_hash present (v2)")
			: TEXT("assets.saved_hash missing (incremental index disabled)"));

	// Orphan dependency rows.
	const int64 OrphanDeps = CountRows(Db, TEXT(
		"SELECT COUNT(*) FROM dependencies d "
		"WHERE d.source_asset_id NOT IN (SELECT id FROM assets) "
		"   OR d.target_asset_id NOT IN (SELECT id FROM assets);"));
	Check(TEXT("integrity:orphan_dependencies"), OrphanDeps == 0,
		OrphanDeps == 0 ? TEXT("no orphan dependency rows")
			: FString::Printf(TEXT("%lld orphan dependency row(s)"), OrphanDeps));

	// FTS row parity (external-content tables should match base counts).
	const int64 ACnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM assets;"));
	const int64 FACnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM fts_assets;"));
	Check(TEXT("fts:assets_row_parity"), ACnt == FACnt,
		FString::Printf(TEXT("assets=%lld fts_assets=%lld%s"), ACnt, FACnt,
			ACnt == FACnt ? TEXT("") : TEXT(" (mismatch -> project.repair_fts)")));
	const int64 NCnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM nodes;"));
	const int64 FNCnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM fts_nodes;"));
	Check(TEXT("fts:nodes_row_parity"), NCnt == FNCnt,
		FString::Printf(TEXT("nodes=%lld fts_nodes=%lld%s"), NCnt, FNCnt,
			NCnt == FNCnt ? TEXT("") : TEXT(" (mismatch -> project.repair_fts)")));

	bool bHasAllCrg = true;
	static const TCHAR* ExpectedCrgTables[] = {
		TEXT("crg_nodes"), TEXT("crg_edges"), TEXT("crg_node_metrics"), TEXT("crg_meta") };
	for (const TCHAR* T : ExpectedCrgTables)
	{
		const bool bHas = ObjectExists(Db, TEXT("table"), T);
		bHasAllCrg = bHasAllCrg && bHas;
		Check(FString::Printf(TEXT("crg:table:%s"), T), bHas,
			bHas ? FString::Printf(TEXT("CRG projection table %s present"), T)
				: FString::Printf(TEXT("missing CRG projection table %s (run project.repair_crg_cache)"), T));
	}
	static const TCHAR* ExpectedCrgIndexes[] = {
		TEXT("idx_crg_nodes_domain_native"), TEXT("idx_crg_nodes_stable"),
		TEXT("idx_crg_edges_domain_source"), TEXT("idx_crg_edges_domain_target"),
		TEXT("idx_crg_edges_kind_subkind"), TEXT("idx_crg_metrics_score") };
	for (const TCHAR* I : ExpectedCrgIndexes)
	{
		const bool bHas = ObjectExists(Db, TEXT("index"), I);
		Check(FString::Printf(TEXT("crg:index:%s"), I), bHas,
			bHas ? FString::Printf(TEXT("CRG projection index %s present"), I)
				: FString::Printf(TEXT("missing CRG projection index %s (run project.repair_crg_cache)"), I));
	}
	int64 CrgNodeCnt = -1;
	int64 CrgEdgeCnt = -1;
	int64 CrgMetricCnt = -1;
	if (bHasAllCrg)
	{
		const int64 DepCnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM dependencies;"));
		CrgNodeCnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'project';"));
		CrgEdgeCnt = CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'project';"));
		CrgMetricCnt = CountRows(Db, TEXT(
			"SELECT COUNT(*) FROM crg_node_metrics m "
			"JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'project';"));
		Check(TEXT("crg:nodes_row_parity"), CrgNodeCnt == ACnt,
			FString::Printf(TEXT("assets=%lld crg_nodes(project)=%lld%s"), ACnt, CrgNodeCnt,
				CrgNodeCnt == ACnt ? TEXT("") : TEXT(" (mismatch -> project.repair_crg_cache)")));
		Check(TEXT("crg:edges_row_parity"), CrgEdgeCnt == DepCnt,
			FString::Printf(TEXT("dependencies=%lld crg_edges(project)=%lld%s"), DepCnt, CrgEdgeCnt,
				CrgEdgeCnt == DepCnt ? TEXT("") : TEXT(" (mismatch -> project.repair_crg_cache)")));
		Check(TEXT("crg:metrics_row_parity"), CrgMetricCnt == CrgNodeCnt,
			FString::Printf(TEXT("crg_nodes(project)=%lld crg_node_metrics=%lld%s"), CrgNodeCnt, CrgMetricCnt,
				CrgMetricCnt == CrgNodeCnt ? TEXT("") : TEXT(" (mismatch -> project.repair_crg_cache)")));
		const int64 OrphanCrgEdges = CountRows(Db, TEXT(
			"SELECT COUNT(*) FROM crg_edges e "
			"WHERE e.domain = 'project' AND ("
			" e.source_node_id NOT IN (SELECT id FROM crg_nodes) "
			" OR e.target_node_id NOT IN (SELECT id FROM crg_nodes));"));
		Check(TEXT("crg:orphan_edges"), OrphanCrgEdges == 0,
			OrphanCrgEdges == 0 ? TEXT("no orphan CRG projection edge rows")
				: FString::Printf(TEXT("%lld orphan CRG projection edge row(s)"), OrphanCrgEdges));
		const FString CacheVersion = ScalarStr(Db, TEXT("SELECT value FROM crg_meta WHERE key = 'cache_version';"));
		Check(TEXT("crg:cache_version"), !CacheVersion.IsEmpty(),
			CacheVersion.IsEmpty() ? TEXT("crg_meta.cache_version missing (run project.repair_crg_cache)")
				: FString::Printf(TEXT("crg cache_version=%s"), *CacheVersion));
		const FString CrgScoringVersion = ScalarStr(Db, TEXT("SELECT value FROM crg_meta WHERE key = 'scoring_version';"));
		Check(TEXT("crg:scoring_version"), CrgScoringVersion == FString(ExpectedScoringVersion),
			CrgScoringVersion.IsEmpty() ? TEXT("crg_meta.scoring_version missing (run project.repair_crg_cache)")
				: FString::Printf(TEXT("crg scoring_version=%s (expected %s)"), *CrgScoringVersion, ExpectedScoringVersion));
	}

	const FString Journal = ScalarStr(Db, TEXT("PRAGMA journal_mode;"));
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("schema_version"), SchemaVer);
	Schema->SetStringField(TEXT("journal_mode"), Journal);
	Root->SetObjectField(TEXT("schema"), Schema);

	if (bIncludeCounts)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("assets"), static_cast<double>(ACnt));
		Counts->SetNumberField(TEXT("nodes"), static_cast<double>(NCnt));
		Counts->SetNumberField(TEXT("dependencies"),
			static_cast<double>(CountRows(Db, TEXT("SELECT COUNT(*) FROM dependencies;"))));
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
		? TEXT("ProjectIndex schema, triggers, FTS parity and integrity OK")
		: FString::Printf(TEXT("%d health warning(s)"), Warnings.Num()));
	Root->SetArrayField(TEXT("checks"), Checks);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("project.repair_crg_cache"), TEXT("project.repair_fts"), TEXT("project.get_stats") });
	return Root;
}

// ============================================================================
// repair_fts (write only when bExecute)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::RepairFts(FMonolithIndexDatabase& Db, const FString& Target, bool bExecute)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target"), Target.IsEmpty() ? TEXT("all") : Target);
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetStringField(TEXT("target"), Target.IsEmpty() ? TEXT("all") : Target);
	Limits->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("limits"), Limits);

	TArray<FString> FtsTables;
	const FString T = Target.IsEmpty() ? TEXT("all") : Target;
	if (T == TEXT("all") || T == TEXT("assets")) FtsTables.Add(TEXT("fts_assets"));
	if (T == TEXT("all") || T == TEXT("nodes")) FtsTables.Add(TEXT("fts_nodes"));

	if (FtsTables.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Unknown target '%s' (expected all|assets|nodes)"), *T));
		Root->SetArrayField(TEXT("warnings"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}

	TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
	for (const FString& F : FtsTables)
	{
		Before->SetNumberField(F, static_cast<double>(
			CountRows(Db, FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), *F))));
	}
	Root->SetObjectField(TEXT("before"), Before);

	FJsonArr Plan;
	for (const FString& F : FtsTables)
	{
		Plan.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("INSERT INTO %s(%s) VALUES('rebuild');"), *F, *F)));
	}
	Root->SetArrayField(TEXT("plan"), Plan);

	FJsonArr Warnings;
	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Dry-run: %d FTS table(s) would be rebuilt. Pass execute=true to apply."),
				FtsTables.Num()));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.repair_fts (execute=true)"), TEXT("project.health") });
		return Root;
	}

	bool bOk = Db.BeginTransaction();
	if (bOk)
	{
		for (const FString& F : FtsTables)
		{
			FSQLitePreparedStatement Stmt;
			const FString Sql = FString::Printf(TEXT("INSERT INTO %s(%s) VALUES('rebuild');"), *F, *F);
			if (!Stmt.Create(*Db.GetRawDatabase(), *Sql) || !Stmt.Execute())
			{
				bOk = false;
				Warnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("rebuild failed for %s"), *F)));
				break;
			}
		}
	}
	if (bOk) { Db.CommitTransaction(); }
	else { Db.RollbackTransaction(); }

	TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
	for (const FString& F : FtsTables)
	{
		After->SetNumberField(F, static_cast<double>(
			CountRows(Db, FString::Printf(TEXT("SELECT COUNT(*) FROM %s;"), *F))));
	}
	Root->SetObjectField(TEXT("after"), After);

	Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
	Root->SetStringField(TEXT("summary"), bOk
		? FString::Printf(TEXT("Rebuilt %d FTS table(s)"), FtsTables.Num())
		: TEXT("FTS rebuild failed; rolled back. Consider a full reindex (monolith_reindex)."));
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("project.health"), TEXT("project.search") });
	return Root;
}

// ============================================================================
// repair_crg_cache (derived projection; write only when bExecute)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::RepairCrgCache(FMonolithIndexDatabase& Db, bool bExecute)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("limits"), Limits);

	FJsonArr Warnings;
	FJsonArr Plan;
	Plan.Add(MakeShared<FJsonValueString>(TEXT("CREATE IF MISSING crg_nodes/crg_edges/crg_node_metrics/crg_meta")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("DELETE existing project CRG projection rows")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("PROJECT assets -> crg_nodes; dependencies -> crg_edges")));
	Plan.Add(MakeShared<FJsonValueString>(TEXT("Recompute fan-in/fan-out/hard-in/risk_score into crg_node_metrics")));
	Root->SetArrayField(TEXT("plan"), Plan);

	if (!Db.IsOpen() || !Db.GetRawDatabase())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.get_stats"), TEXT("project.health") });
		return Root;
	}

	TSharedPtr<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetNumberField(TEXT("assets"), static_cast<double>(CountRows(Db, TEXT("SELECT COUNT(*) FROM assets;"))));
	Before->SetNumberField(TEXT("dependencies"), static_cast<double>(CountRows(Db, TEXT("SELECT COUNT(*) FROM dependencies;"))));
	if (HasCrgProjectionTables(Db))
	{
		Before->SetNumberField(TEXT("crg_nodes"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'project';"))));
		Before->SetNumberField(TEXT("crg_edges"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'project';"))));
		Before->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'project';"))));
	}
	Root->SetObjectField(TEXT("before"), Before);

	if (!bExecute)
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"),
			TEXT("Dry-run: project CRG projection/cache would be rebuilt. Pass execute=true to apply."));
		Root->SetObjectField(TEXT("after"), MakeShared<FJsonObject>());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.repair_crg_cache (execute=true)"), TEXT("project.health"), TEXT("project.risk_score") });
		return Root;
	}

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	bool bOk = EnsureCrgProjectionTables(Db);
	auto Exec = [&](const TCHAR* Sql, const TCHAR* Label)
	{
		if (!bOk) return;
		if (!Raw->Execute(Sql))
		{
			bOk = false;
			Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("CRG cache rebuild failed at %s"), Label)));
		}
	};

	if (bOk)
	{
		bOk = Db.BeginTransaction();
	}
	Exec(TEXT("DELETE FROM crg_node_metrics WHERE node_id IN (SELECT id FROM crg_nodes WHERE domain = 'project');"), TEXT("clear metrics"));
	Exec(TEXT("DELETE FROM crg_edges WHERE domain = 'project';"), TEXT("clear edges"));
	Exec(TEXT("DELETE FROM crg_nodes WHERE domain = 'project';"), TEXT("clear nodes"));
	Exec(TEXT(
		"INSERT INTO crg_nodes(domain,native_table,native_id,stable_key,kind,name,path,module,source_revision,extra,updated_at) "
		"SELECT 'project','assets',a.id,a.package_path,a.asset_class,a.asset_name,a.package_path,a.module_name,COALESCE(a.saved_hash,''),'{}',"
		"CAST(strftime('%s','now') AS INTEGER) "
		"FROM assets a;"), TEXT("project nodes"));
	Exec(TEXT(
		"INSERT INTO crg_edges(domain,source_node_id,target_node_id,edge_kind,edge_subkind,weight,native_table,native_id,updated_at) "
		"SELECT 'project',sn.id,tn.id,'dependency',COALESCE(d.dependency_type,''),"
		"CASE WHEN d.dependency_type = 'Hard' THEN 1.0 ELSE 0.5 END,"
		"'dependencies',d.id,CAST(strftime('%s','now') AS INTEGER) "
		"FROM dependencies d "
		"JOIN crg_nodes sn ON sn.domain='project' AND sn.native_table='assets' AND sn.native_id=d.source_asset_id "
		"JOIN crg_nodes tn ON tn.domain='project' AND tn.native_table='assets' AND tn.native_id=d.target_asset_id;"), TEXT("project edges"));
	Exec(TEXT(
		"WITH counts AS ("
		" SELECT a.id AS native_id,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id = a.id) AS fan_in,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.source_asset_id = a.id) AS fan_out,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id = a.id AND d.dependency_type = 'Hard') AS hard_in,"
		"        (SELECT COUNT(*) FROM nodes x WHERE x.asset_id = a.id) AS node_count,"
		"        (SELECT COUNT(*) FROM variables x WHERE x.asset_id = a.id) AS var_count,"
		"        (SELECT COUNT(*) FROM parameters x WHERE x.asset_id = a.id) AS param_count,"
		"        (SELECT COUNT(*) FROM tag_references x WHERE x.asset_id = a.id) AS tag_refs,"
		"        CASE"
		"          WHEN a.asset_class LIKE '%World%' OR a.asset_class LIKE '%Level%' THEN 5"
		"          WHEN a.asset_class LIKE '%GameplayAbility%' OR a.asset_class LIKE '%AttributeSet%' OR a.asset_class LIKE '%GameplayEffect%' THEN 4"
		"          WHEN a.asset_class LIKE '%Blueprint%' THEN 3"
		"          WHEN a.asset_class LIKE '%NiagaraSystem%' OR a.asset_class LIKE '%Material%' THEN 2"
		"          WHEN a.asset_class LIKE '%DataTable%' OR a.asset_class LIKE '%DataAsset%' THEN 2"
		"          ELSE 1"
		"        END AS class_weight,"
		"        CASE"
		"          WHEN lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%network%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%replication%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%rpc%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%netmulticast%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%onrep%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%save%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%serialize%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%archive%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%auth%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%login%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%account%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%session%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%purchase%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%store%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%entitlement%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%anticheat%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%crypt%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%encrypt%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%decrypt%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%sign%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%hash%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%exec%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%eval%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%command%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%file%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%registry%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%process%'"
		"          THEN 1 ELSE 0 END AS sensitivity"
		" FROM assets a"
		"), scored AS ("
		" SELECT c.*, MIN(1.0,"
		"        MIN(c.fan_in,30) / 30.0 * 0.30 +"
		"        MIN(c.hard_in,20) / 20.0 * 0.20 +"
		"        MIN(c.fan_out,30) / 30.0 * 0.10 +"
		"        (c.class_weight - 1) / 4.0 * 0.20 +"
		"        CASE WHEN c.sensitivity != 0 THEN 0.15 ELSE 0.0 END +"
		"        MIN(c.node_count,400) / 400.0 * 0.15 +"
		"        MIN(c.tag_refs,20) / 20.0 * 0.05) AS score"
		" FROM counts c"
		") "
		"INSERT INTO crg_node_metrics(node_id,fan_in,fan_out,hard_in,descendants,risk_score,risk_tier,reasons_json,raw_counts_json,scoring_version,computed_at) "
		"SELECT n.id,s.fan_in,s.fan_out,s.hard_in,0,ROUND(s.score,3),"
		"       CASE WHEN s.score >= 0.66 THEN 'high' WHEN s.score >= 0.33 THEN 'medium' ELSE 'low' END,"
		"       CASE WHEN s.sensitivity != 0 THEN"
		"         printf('[\"inbound dependency fan-in: %d referencer(s)\",\"hard inbound dependencies: %d\",\"outbound dependencies: %d\",\"sensitivity: UE-domain sensitive surface\",\"graph density: %d node(s)\"]',"
		"                s.fan_in,s.hard_in,s.fan_out,s.node_count)"
		"       ELSE"
		"         printf('[\"inbound dependency fan-in: %d referencer(s)\",\"hard inbound dependencies: %d\",\"outbound dependencies: %d\",\"graph density: %d node(s)\"]',"
		"                s.fan_in,s.hard_in,s.fan_out,s.node_count)"
		"       END,"
		"       printf('{\"inbound\":%d,\"inbound_hard\":%d,\"outbound\":%d,\"nodes\":%d,\"variables\":%d,\"parameters\":%d,\"tag_references\":%d,\"class_weight\":%d,\"sensitivity\":%d}',"
		"              s.fan_in,s.hard_in,s.fan_out,s.node_count,s.var_count,s.param_count,s.tag_refs,s.class_weight,s.sensitivity),"
		"       '3',CAST(strftime('%s','now') AS INTEGER) "
		"FROM scored s "
		"JOIN crg_nodes n ON n.domain='project' AND n.native_table='assets' AND n.native_id=s.native_id;"), TEXT("project metrics"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('cache_version','1');"), TEXT("cache_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('scoring_version','3');"), TEXT("scoring_version"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('built_at',datetime('now'));"), TEXT("built_at"));
	Exec(TEXT("INSERT OR REPLACE INTO crg_meta(key,value) VALUES('project_built_at',datetime('now'));"), TEXT("project_built_at"));

	if (bOk) { Db.CommitTransaction(); }
	else { Db.RollbackTransaction(); }

	TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
	if (HasCrgProjectionTables(Db))
	{
		After->SetNumberField(TEXT("crg_nodes"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_nodes WHERE domain = 'project';"))));
		After->SetNumberField(TEXT("crg_edges"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_edges WHERE domain = 'project';"))));
		After->SetNumberField(TEXT("crg_node_metrics"), static_cast<double>(
			CountRows(Db, TEXT("SELECT COUNT(*) FROM crg_node_metrics m JOIN crg_nodes n ON n.id = m.node_id WHERE n.domain = 'project';"))));
	}
	Root->SetObjectField(TEXT("after"), After);
	Root->SetStringField(TEXT("status"), bOk ? TEXT("ok") : TEXT("error"));
	Root->SetStringField(TEXT("summary"), bOk
		? TEXT("Rebuilt project CRG projection/cache from ProjectIndex assets and dependencies")
		: TEXT("Project CRG projection/cache rebuild failed; rolled back"));
	Root->SetArrayField(TEXT("warnings"), Warnings);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("project.health"), TEXT("project.risk_score"), TEXT("project.review_context") });
	return Root;
}

// ============================================================================
// risk_score (cached when available; query-time fallback)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::RiskScore(
	FMonolithIndexDatabase& Db,
	const FString& SeedPath,
	int32 Limit,
	const FString& MinTier)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("seed"), SeedPath);
	Root->SetObjectField(TEXT("input"), Input);
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 20 : Limit, 1, 200);
	const int32 MinRank = TierRank(MinTier.IsEmpty() ? TEXT("low") : MinTier);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Limits->SetStringField(TEXT("min_tier"), MinTier.IsEmpty() ? TEXT("low") : MinTier);
	Root->SetObjectField(TEXT("limits"), Limits);

	FJsonArr Items;

	if (!SeedPath.IsEmpty())
	{
		const TOptional<FIndexedAsset> Seed = Db.GetAssetByPath(SeedPath);
		if (!Seed.IsSet())
		{
			Root->SetStringField(TEXT("status"), TEXT("error"));
			Root->SetStringField(TEXT("summary"),
				FString::Printf(TEXT("Asset not found: %s"), *SeedPath));
			Root->SetArrayField(TEXT("items"), Items);
			Root->SetBoolField(TEXT("truncated"), false);
			AddNext(Root, { TEXT("project.search") });
			return Root;
		}
		Items.Add(MakeShared<FJsonValueObject>(ScoreAsset(Db, Seed.GetValue())));
	}
	else
	{
		// No seed: score the highest fan-in assets (bounded scan, not full table).
		FSQLiteDatabase* Raw = Db.GetRawDatabase();
		TArray<int64> Candidates;
		if (Raw)
		{
			FSQLitePreparedStatement Stmt;
			if (Stmt.Create(*Raw, TEXT(
				"SELECT target_asset_id, COUNT(*) c FROM dependencies "
				"GROUP BY target_asset_id ORDER BY c DESC LIMIT ?;")))
			{
				Stmt.SetBindingValueByIndex(1, static_cast<int64>(Cap));
				while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					int64 Aid = 0;
					Stmt.GetColumnValueByIndex(0, Aid);
					Candidates.Add(Aid);
				}
			}
		}
		for (int64 Aid : Candidates)
		{
			FSQLitePreparedStatement S2;
			if (S2.Create(*Raw, TEXT(
				"SELECT id, package_path, asset_name, asset_class, module_name FROM assets WHERE id = ?;")))
			{
				S2.SetBindingValueByIndex(1, Aid);
				if (S2.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FIndexedAsset A;
					S2.GetColumnValueByIndex(0, A.Id);
					S2.GetColumnValueByIndex(1, A.PackagePath);
					S2.GetColumnValueByIndex(2, A.AssetName);
					S2.GetColumnValueByIndex(3, A.AssetClass);
					S2.GetColumnValueByIndex(4, A.ModuleName);
					Items.Add(MakeShared<FJsonValueObject>(ScoreAsset(Db, A)));
				}
			}
		}
	}

	// min_tier filter + sort by score desc.
	Items.RemoveAll([&](const TSharedPtr<FJsonValue>& V)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		return O.IsValid() && TierRank(O->GetStringField(TEXT("tier"))) < MinRank;
	});
	Items.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const double SA = A->AsObject().IsValid() ? A->AsObject()->GetNumberField(TEXT("score")) : 0.0;
		const double SB = B->AsObject().IsValid() ? B->AsObject()->GetNumberField(TEXT("score")) : 0.0;
		return SA > SB;
	});

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"),
		FString::Printf(TEXT("%d asset(s) scored (scoring_version=3 cached when available, v3 query fallback)"), Items.Num()));
	Root->SetStringField(TEXT("scoring_version"), ExpectedScoringVersion);
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("project.repair_crg_cache"), TEXT("project.review_context"), TEXT("project.impact_radius") });
	return Root;
}

// ============================================================================
// detect_changes (changed asset paths -> review queue)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::DetectChanges(
	FMonolithIndexDatabase& Db,
	const TArray<FString>& ChangedPaths,
	int32 MaxResults,
	const FString& DetailLevel)
{
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
	Root->SetStringField(TEXT("scoring_version"), ExpectedScoringVersion);
	Root->SetNumberField(TEXT("risk_score"), 0.0);

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	if (!Db.IsOpen() || !Raw)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		Root->SetArrayField(TEXT("changed_entities"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.get_stats"), TEXT("project.health") });
		return Root;
	}

	if (NormalizedPaths.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("changed_paths or paths must include at least one path"));
		Root->SetArrayField(TEXT("changed_entities"), FJsonArr());
		TSharedPtr<FJsonObject> Impact = MakeShared<FJsonObject>();
		Impact->SetNumberField(TEXT("depth"), 1);
		Impact->SetNumberField(TEXT("impacted_count"), 0);
		Root->SetObjectField(TEXT("impact"), Impact);
		Root->SetArrayField(TEXT("test_gaps"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.search"), TEXT("project.find_by_type") });
		return Root;
	}

	TSet<int64> ChangedIds;
	FJsonArr ChangedEntities;
	bool bTruncated = false;

	for (const FString& Path : NormalizedPaths)
	{
		if (ChangedEntities.Num() >= Cap)
		{
			bTruncated = true;
			break;
		}
		const FString Stem = FPaths::GetBaseFilename(Path);
		if (Stem.IsEmpty())
		{
			continue;
		}
		const FString EscapedStem = Stem
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("%"), TEXT("\\%"))
			.Replace(TEXT("_"), TEXT("\\_"));

		FSQLitePreparedStatement Stmt;
		if (!Stmt.Create(*Raw, TEXT(
			"SELECT id,package_path,asset_name,asset_class,COALESCE(module_name,'') "
			"FROM assets "
			"WHERE asset_name = ? OR replace(package_path,'\\','/') LIKE ? ESCAPE '\\' "
			"ORDER BY id LIMIT ?;")))
		{
			continue;
		}
		Stmt.SetBindingValueByIndex(1, Stem);
		Stmt.SetBindingValueByIndex(2, FString::Printf(TEXT("%%/%s"), *EscapedStem));
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Cap + 1));

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			if (ChangedEntities.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}

			FIndexedAsset Asset;
			Stmt.GetColumnValueByIndex(0, Asset.Id);
			Stmt.GetColumnValueByIndex(1, Asset.PackagePath);
			Stmt.GetColumnValueByIndex(2, Asset.AssetName);
			Stmt.GetColumnValueByIndex(3, Asset.AssetClass);
			Stmt.GetColumnValueByIndex(4, Asset.ModuleName);
			if (ChangedIds.Contains(Asset.Id))
			{
				continue;
			}
			ChangedIds.Add(Asset.Id);

			TSharedPtr<FJsonObject> Scored = ScoreAsset(Db, Asset);
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
		FSQLitePreparedStatement ImpactStmt;
		if (!ImpactStmt.Create(*Raw, TEXT(
			"SELECT DISTINCT source_asset_id FROM dependencies WHERE target_asset_id = ? LIMIT 201;")))
		{
			continue;
		}
		ImpactStmt.SetBindingValueByIndex(1, ChangedId);
		while (ImpactStmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 SourceId = 0;
			ImpactStmt.GetColumnValueByIndex(0, SourceId);
			if (!ChangedIds.Contains(SourceId))
			{
				ImpactedIds.Add(SourceId);
			}
		}
	}

	FJsonArr ImpactedEntities;
	if (bStandard)
	{
		int32 Emitted = 0;
		for (int64 Id : ImpactedIds)
		{
			if (Emitted >= 200)
			{
				break;
			}
			if (TSharedPtr<FJsonObject> Asset = AssetJsonById(Db, Id))
			{
				ImpactedEntities.Add(MakeShared<FJsonValueObject>(Asset));
				++Emitted;
			}
		}
	}

	FJsonArr Priorities;
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
			if (!O->TryGetStringField(TEXT("asset_name"), Name) || Name.IsEmpty())
			{
				O->TryGetStringField(TEXT("asset_path"), Name);
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
		TEXT("%d changed asset(s), %d direct impacted referencer(s), %d review priorit%s"),
		ChangedEntities.Num(), ImpactedIds.Num(), Priorities.Num(), Priorities.Num() == 1 ? TEXT("y") : TEXT("ies")));
	Root->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(MaxRisk * 1000.0) / 1000.0);
	Root->SetNumberField(TEXT("changed_entity_count"), ChangedEntities.Num());
	Root->SetNumberField(TEXT("impacted_count"), ImpactedIds.Num());
	Root->SetNumberField(TEXT("test_gap_count"), 0);
	Root->SetObjectField(TEXT("impact"), Impact);
	Root->SetArrayField(TEXT("review_priorities"), Priorities);
	Root->SetArrayField(TEXT("test_gaps"), FJsonArr());
	if (bStandard)
	{
		Root->SetArrayField(TEXT("changed_entities"), ChangedEntities);
	}
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	if (ChangedEntities.Num() == 0)
	{
		AddNext(Root, { TEXT("project.search"), TEXT("project.find_by_type") });
	}
	else
	{
		AddNext(Root, { TEXT("project.review_context"), TEXT("project.find_references"), TEXT("project.risk_score") });
	}
	return Root;
}

// ============================================================================
// find_unused (advisory orphan-asset candidates)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::FindUnused(
	FMonolithIndexDatabase& Db,
	const FString& Kind,
	int32 Limit,
	const FString& MinConfidence)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString KindFilter = Kind.TrimStartAndEnd();
	const bool bFilterKind = !KindFilter.IsEmpty() && !KindFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase);
	FString MinConf = MinConfidence.IsEmpty() ? TEXT("low") : MinConfidence.ToLower();
	if (MinConf != TEXT("low") && MinConf != TEXT("medium") && MinConf != TEXT("high"))
	{
		MinConf = TEXT("low");
	}
	const int32 MinRank = ConfidenceRank(MinConf);
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 100 : Limit, 1, 1000);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("kind"), bFilterKind ? KindFilter : TEXT("all"));
	Input->SetStringField(TEXT("min_confidence"), MinConf);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Root->SetObjectField(TEXT("limits"), Limits);

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	if (!Db.IsOpen() || !Raw)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		Root->SetArrayField(TEXT("items"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.get_stats"), TEXT("project.health") });
		return Root;
	}

	if (MinRank >= ConfidenceRank(TEXT("high")))
	{
		Root->SetStringField(TEXT("status"), TEXT("ok"));
		Root->SetStringField(TEXT("summary"), TEXT("0 advisory project unused candidate(s) found (find_unused never reports high confidence)"));
		Root->SetArrayField(TEXT("items"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.find_references"), TEXT("project.review_context"), TEXT("project.impact_radius") });
		return Root;
	}

	const FString KindClause = bFilterKind ? TEXT("AND a.asset_class = ? ") : TEXT("");
	const FString ConfidenceClause = MinRank >= ConfidenceRank(TEXT("medium"))
		? TEXT("AND a.asset_class NOT LIKE '%%Texture%%' "
			"AND a.asset_class NOT LIKE '%%Material%%' "
			"AND a.asset_class NOT LIKE '%%Sound%%' "
			"AND a.asset_class NOT LIKE '%%DataTable%%' "
			"AND a.asset_class NOT LIKE '%%DataAsset%%' "
			"AND a.asset_class NOT LIKE '%%Paper%%' ")
		: TEXT("");
	const FString Sql = FString::Printf(TEXT(
		"SELECT a.id,a.package_path,a.asset_name,a.asset_class,COALESCE(a.module_name,'') "
		"FROM assets a "
		"WHERE NOT EXISTS (SELECT 1 FROM dependencies d WHERE d.target_asset_id = a.id) "
		"AND a.asset_class NOT LIKE '%%World%%' "
		"AND a.asset_class NOT LIKE '%%Level%%' "
		"AND a.asset_class NOT LIKE '%%PrimaryAssetLabel%%' "
		"AND a.asset_class NOT LIKE '%%DirectoryPlaceholder%%' "
		"%s"
		"%s"
		"ORDER BY a.id LIMIT ?;"), *KindClause, *ConfidenceClause);

	FJsonArr Items;
	bool bTruncated = false;
	FSQLitePreparedStatement Stmt;
	if (Stmt.Create(*Raw, *Sql))
	{
		int32 BindIndex = 1;
		if (bFilterKind)
		{
			Stmt.SetBindingValueByIndex(BindIndex++, KindFilter);
		}
		Stmt.SetBindingValueByIndex(BindIndex, static_cast<int64>(Cap + 1));

		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			int64 Id = 0;
			FString Path, Name, Class, Module;
			Stmt.GetColumnValueByIndex(0, Id);
			Stmt.GetColumnValueByIndex(1, Path);
			Stmt.GetColumnValueByIndex(2, Name);
			Stmt.GetColumnValueByIndex(3, Class);
			Stmt.GetColumnValueByIndex(4, Module);

			const FString Confidence = ConfidenceForUnusedAssetClass(Class);
			if (ConfidenceRank(Confidence) < MinRank)
			{
				continue;
			}
			if (Items.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}

			FJsonArr Reasons;
			Reasons.Add(MakeShared<FJsonValueString>(TEXT("no indexed inbound dependencies")));
			Reasons.Add(MakeShared<FJsonValueString>(TEXT("World/Level/PrimaryAssetLabel root-like classes excluded")));
			if (Confidence == TEXT("low"))
			{
				Reasons.Add(MakeShared<FJsonValueString>(TEXT("asset class is commonly referenced by convention, editor settings, soft paths, or runtime loads")));
			}
			else
			{
				Reasons.Add(MakeShared<FJsonValueString>(TEXT("asset class is not in the known low-confidence convention-loaded set")));
			}

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetNumberField(TEXT("id"), static_cast<double>(Id));
			O->SetStringField(TEXT("asset_path"), Path);
			O->SetStringField(TEXT("asset_name"), Name);
			O->SetStringField(TEXT("asset_class"), Class);
			O->SetStringField(TEXT("module_name"), Module);
			O->SetStringField(TEXT("confidence"), Confidence);
			O->SetArrayField(TEXT("reasons"), Reasons);
			Items.Add(MakeShared<FJsonValueObject>(O));
		}
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d advisory project unused candidate(s) found (never high confidence; no mutation)"), Items.Num()));
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNext(Root, { TEXT("project.find_references"), TEXT("project.review_context"), TEXT("project.impact_radius") });
	return Root;
}

// ============================================================================
// pre_merge_check (advisory aggregate gate)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::PreMergeCheck(
	FMonolithIndexDatabase& Db,
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
	Root->SetStringField(TEXT("scoring_version"), ExpectedScoringVersion);

	TSharedPtr<FJsonObject> HealthResult = Health(Db, false);
	TSharedPtr<FJsonObject> ChangeResult = DetectChanges(Db, NormalizedPaths, ChangeCap, bStandard ? TEXT("standard") : TEXT("minimal"));
	TSharedPtr<FJsonObject> UnusedResult = bIncludeUnused
		? FindUnused(Db, TEXT("all"), UnusedCap, TEXT("low"))
		: nullptr;

	FJsonArr Checks;
	FJsonArr Findings;
	int32 Severity = 0; // 0 pass, 1 warn, 2 fail
	auto Promote = [&](int32 Value)
	{
		Severity = FMath::Max(Severity, Value);
	};
	auto StatusOf = [](const TSharedPtr<FJsonObject>& Object) -> FString
	{
		return Object.IsValid() ? Object->GetStringField(TEXT("status")) : FString(TEXT("error"));
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
	AddCheck(TEXT("health"), HealthStatus, HealthResult.IsValid() ? HealthResult->GetStringField(TEXT("summary")) : TEXT("Project health could not run"), HealthSeverity);
	if (HealthSeverity > 0)
	{
		AddFinding(HealthSeverity >= 2 ? TEXT("error") : TEXT("warning"), TEXT("health"),
			HealthResult.IsValid() ? HealthResult->GetStringField(TEXT("summary")) : TEXT("Project health failed"));
	}

	const FString ChangeStatus = StatusOf(ChangeResult);
	const int32 ChangedCount = IntField(ChangeResult, TEXT("changed_entity_count"));
	const int32 ImpactCount = IntField(ChangeResult, TEXT("impacted_count"));
	const double RiskScore = NumField(ChangeResult, TEXT("risk_score"));
	int32 ChangeSeverity = ChangeStatus == TEXT("error") ? 2 : 0;
	if (ChangeSeverity == 0 && ChangedCount == 0)
	{
		ChangeSeverity = 1;
		AddFinding(TEXT("warning"), TEXT("detect_changes"), TEXT("No indexed asset matched the changed path set"));
	}
	if (RiskScore >= 0.66)
	{
		ChangeSeverity = FMath::Max(ChangeSeverity, 1);
		AddFinding(TEXT("warning"), TEXT("detect_changes"),
			FString::Printf(TEXT("Changed asset risk score is high: %.3f"), RiskScore));
	}
	if (ImpactCount > 50)
	{
		ChangeSeverity = FMath::Max(ChangeSeverity, 1);
		AddFinding(TEXT("warning"), TEXT("detect_changes"),
			FString::Printf(TEXT("Changed asset set has broad direct impact: %d referencer(s)"), ImpactCount));
	}
	if (ChangeStatus == TEXT("error"))
	{
		AddFinding(TEXT("error"), TEXT("detect_changes"),
			ChangeResult.IsValid() ? ChangeResult->GetStringField(TEXT("summary")) : TEXT("detect_changes failed"));
	}
	AddCheck(TEXT("detect_changes"), ChangeStatus, FString::Printf(
		TEXT("%d changed asset(s), %d impacted referencer(s), risk=%.3f"),
		ChangedCount, ImpactCount, RiskScore), ChangeSeverity);

	int32 UnusedCount = 0;
	if (bIncludeUnused && UnusedResult.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		UnusedCount = UnusedResult->TryGetArrayField(TEXT("items"), Items) && Items ? Items->Num() : 0;
		const FString UnusedStatus = StatusOf(UnusedResult);
		const int32 UnusedSeverity = UnusedStatus == TEXT("error") ? 2 : UnusedCount > 0 ? 1 : 0;
		AddCheck(TEXT("find_unused"), UnusedStatus, FString::Printf(
			TEXT("%d advisory unused asset candidate(s) sampled"), UnusedCount), UnusedSeverity);
		if (UnusedCount > 0)
		{
			AddFinding(TEXT("warning"), TEXT("find_unused"),
				FString::Printf(TEXT("%d advisory unused asset candidate(s) present in sampled index"), UnusedCount));
		}
	}

	const FString Decision = Severity >= 2 ? TEXT("fail") : Severity == 1 ? TEXT("warn") : TEXT("pass");
	Root->SetStringField(TEXT("status"), Severity >= 2 ? TEXT("error") : Severity == 1 ? TEXT("warning") : TEXT("ok"));
	Root->SetStringField(TEXT("decision"), Decision);
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("Project pre-merge check %s: %d changed asset(s), %d impacted referencer(s), %d finding(s)"),
		*Decision, ChangedCount, ImpactCount, Findings.Num()));
	Root->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(RiskScore * 1000.0) / 1000.0);
	Root->SetArrayField(TEXT("checks"), Checks);
	Root->SetArrayField(TEXT("findings"), Findings);
	Root->SetNumberField(TEXT("changed_entity_count"), ChangedCount);
	Root->SetNumberField(TEXT("impacted_count"), ImpactCount);
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
		AddNext(Root, { TEXT("project.health"), TEXT("project.search") });
	}
	else
	{
		AddNext(Root, { TEXT("project.detect_changes"), TEXT("project.review_context"), TEXT("project.find_unused") });
	}
	return Root;
}

// ============================================================================
// snapshot / diff_snapshots (derived CRG projection review manifests)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::Snapshot(
	FMonolithIndexDatabase& Db,
	const FString& Label,
	bool bExecute)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString RequestedLabel = Label.TrimStartAndEnd();
	const FString CleanLabel = RequestedLabel.IsEmpty()
		? FString::Printf(TEXT("project-%lld"), FDateTime::UtcNow().ToUnixTimestamp())
		: RequestedLabel;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("label"), CleanLabel);
	Input->SetBoolField(TEXT("execute"), bExecute);
	Root->SetObjectField(TEXT("input"), Input);

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	if (!Db.IsOpen() || !Raw)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}
	if (!ObjectExists(Db, TEXT("table"), TEXT("crg_nodes")) || !ObjectExists(Db, TEXT("table"), TEXT("crg_edges")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("CRG projection tables are missing; run project.repair_crg_cache execute=true first"));
		AddNext(Root, { TEXT("project.repair_crg_cache"), TEXT("project.health") });
		return Root;
	}

	FSnapshotManifest Manifest;
	if (!LoadCurrentManifest(*Raw, TEXT("project"), Manifest))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to read current project CRG projection"));
		AddNext(Root, { TEXT("project.health"), TEXT("project.repair_crg_cache") });
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
			TEXT("Would capture project CRG snapshot '%s' with %d node(s), %d edge(s)"),
			*CleanLabel, Manifest.Nodes.Num(), Manifest.Edges.Num()));
		AddNext(Root, { TEXT("project.snapshot execute=true"), TEXT("project.diff_snapshots") });
		return Root;
	}

	if (!EnsureSnapshotTable(*Raw))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to create crg_snapshots table"));
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}

	FSQLitePreparedStatement Stmt;
	if (!Stmt.Create(*Raw, TEXT(
		"INSERT OR REPLACE INTO crg_snapshots(label,domain,captured_at,node_count,edge_count,manifest_json) "
		"VALUES(?,?,?,?,?,?);")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to prepare project snapshot insert"));
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}
	Stmt.SetBindingValueByIndex(1, CleanLabel);
	Stmt.SetBindingValueByIndex(2, FString(TEXT("project")));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()));
	Stmt.SetBindingValueByIndex(4, static_cast<int64>(Manifest.Nodes.Num()));
	Stmt.SetBindingValueByIndex(5, static_cast<int64>(Manifest.Edges.Num()));
	Stmt.SetBindingValueByIndex(6, SerializeManifest(Manifest));
	if (!Stmt.Execute())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Failed to store project CRG snapshot"));
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}

	Root->SetNumberField(TEXT("id"), static_cast<double>(Raw->GetLastInsertRowId()));
	Root->SetStringField(TEXT("label"), CleanLabel);
	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("Captured project CRG snapshot '%s' with %d node(s), %d edge(s)"),
		*CleanLabel, Manifest.Nodes.Num(), Manifest.Edges.Num()));
	AddNext(Root, { TEXT("project.diff_snapshots"), TEXT("project.repair_crg_cache") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithIndexReview::DiffSnapshots(
	FMonolithIndexDatabase& Db,
	const FString& Before,
	const FString& After,
	int32 Limit)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 100 : Limit, 1, 1000);
	const FString AfterRef = After.TrimStartAndEnd().IsEmpty() ? TEXT("current") : After.TrimStartAndEnd();

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("before"), Before);
	Input->SetStringField(TEXT("after"), AfterRef);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Root->SetObjectField(TEXT("limits"), Limits);

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	if (!Db.IsOpen() || !Raw)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		AddNext(Root, { TEXT("project.health") });
		return Root;
	}
	if (Before.TrimStartAndEnd().IsEmpty())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("before snapshot label/id is required"));
		AddNext(Root, { TEXT("project.snapshot execute=true") });
		return Root;
	}
	if (!ObjectExists(Db, TEXT("table"), TEXT("crg_snapshots")))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("crg_snapshots table is missing; capture a project.snapshot first"));
		AddNext(Root, { TEXT("project.snapshot execute=true") });
		return Root;
	}

	FSnapshotRecord BeforeRecord;
	FSnapshotRecord AfterRecord;
	if (!LoadSnapshotRecord(*Raw, TEXT("project"), Before.TrimStartAndEnd(), BeforeRecord))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("Before snapshot not found or invalid: %s"), *Before));
		AddNext(Root, { TEXT("project.snapshot execute=true") });
		return Root;
	}
	if (!LoadSnapshotRecord(*Raw, TEXT("project"), AfterRef, AfterRecord))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), FString::Printf(TEXT("After snapshot not found or invalid: %s"), *AfterRef));
		AddNext(Root, { TEXT("project.snapshot execute=true") });
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
		TEXT("Project CRG diff %s -> %s: +%d/-%d node(s), +%d/-%d edge(s)"),
		*BeforeRecord.Label, *AfterRecord.Label, NewNodes.Num(), RemovedNodes.Num(), NewEdges.Num(), RemovedEdges.Num()));
	AddNext(Root, { TEXT("project.snapshot"), TEXT("project.review_hotspots"), TEXT("project.health") });
	return Root;
}

// ============================================================================
// review_hotspots (global top fan/risk/size review queue)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::ReviewHotspots(
	FMonolithIndexDatabase& Db,
	const FString& Kind,
	int32 Limit,
	int32 MinLines,
	bool bIncludeQuestions)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const FString NormalizedKind = Kind.IsEmpty() ? TEXT("all") : Kind.ToLower();
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 50 : Limit, 1, 200);
	const int32 SizeFloor = FMath::Max(MinLines <= 0 ? 100 : MinLines, 0);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("kind"), NormalizedKind);
	Input->SetBoolField(TEXT("include_questions"), bIncludeQuestions);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Limits->SetNumberField(TEXT("min_lines"), SizeFloor);
	Root->SetObjectField(TEXT("limits"), Limits);

	if (NormalizedKind != TEXT("fan_in") && NormalizedKind != TEXT("fan_out")
		&& NormalizedKind != TEXT("risk") && NormalizedKind != TEXT("large")
		&& NormalizedKind != TEXT("all"))
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("Unsupported kind for project.review_hotspots (expected fan_in|fan_out|risk|large|all)"));
		Root->SetArrayField(TEXT("hotspots"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.review_hotspots kind=all"), TEXT("project.risk_score") });
		return Root;
	}

	FSQLiteDatabase* Raw = Db.GetRawDatabase();
	if (!Db.IsOpen() || !Raw)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"), TEXT("ProjectIndex DB is not open"));
		Root->SetArrayField(TEXT("hotspots"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.get_stats"), TEXT("project.health") });
		return Root;
	}

	const bool bHasCrg = HasCrgProjectionTables(Db);
	const FString CacheJoin = bHasCrg
		? TEXT("LEFT JOIN crg_nodes n ON n.domain='project' AND n.native_table='assets' AND n.native_id=c.id "
			"LEFT JOIN crg_node_metrics m ON m.node_id=n.id ")
		: TEXT("");
	const FString RiskScoreExpr = bHasCrg ? TEXT("COALESCE(m.risk_score, c.estimated_risk)") : TEXT("c.estimated_risk");
	const FString RiskTierExpr = bHasCrg
		? TEXT("COALESCE(m.risk_tier, CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END)")
		: TEXT("CASE WHEN c.estimated_risk >= 0.66 THEN 'high' WHEN c.estimated_risk >= 0.33 THEN 'medium' ELSE 'low' END");
	const FString WhereClause = NormalizedKind == TEXT("large")
		? FString::Printf(TEXT("WHERE size_signal >= %d "), SizeFloor)
		: TEXT("WHERE fan_in > 0 OR fan_out > 0 OR hard_in > 0 OR risk_score > 0 OR size_signal >= ") + FString::FromInt(SizeFloor) + TEXT(" ");
	FString OrderBy = TEXT("ORDER BY hotspot_score DESC, risk_score DESC, fan_in DESC, size_signal DESC ");
	if (NormalizedKind == TEXT("fan_in")) OrderBy = TEXT("ORDER BY fan_in DESC, risk_score DESC, hard_in DESC ");
	else if (NormalizedKind == TEXT("fan_out")) OrderBy = TEXT("ORDER BY fan_out DESC, risk_score DESC, size_signal DESC ");
	else if (NormalizedKind == TEXT("risk")) OrderBy = TEXT("ORDER BY risk_score DESC, fan_in DESC, size_signal DESC ");
	else if (NormalizedKind == TEXT("large")) OrderBy = TEXT("ORDER BY size_signal DESC, risk_score DESC, fan_in DESC ");

	const FString Sql = FString::Printf(TEXT(
		"WITH base AS ("
		" SELECT a.id,a.package_path,a.asset_name,a.asset_class,COALESCE(a.module_name,'') AS module_name,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id=a.id) AS fan_in,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.source_asset_id=a.id) AS fan_out,"
		"        (SELECT COUNT(*) FROM dependencies d WHERE d.target_asset_id=a.id AND d.dependency_type='Hard') AS hard_in,"
		"        (SELECT COUNT(*) FROM nodes x WHERE x.asset_id=a.id) AS node_count,"
		"        (SELECT COUNT(*) FROM variables x WHERE x.asset_id=a.id) AS variable_count,"
		"        (SELECT COUNT(*) FROM parameters x WHERE x.asset_id=a.id) AS parameter_count,"
		"        (SELECT COUNT(*) FROM tag_references x WHERE x.asset_id=a.id) AS tag_refs,"
		"        CASE"
		"          WHEN a.asset_class LIKE '%%World%%' OR a.asset_class LIKE '%%Level%%' THEN 5"
		"          WHEN a.asset_class LIKE '%%GameplayAbility%%' OR a.asset_class LIKE '%%AttributeSet%%' OR a.asset_class LIKE '%%GameplayEffect%%' THEN 4"
		"          WHEN a.asset_class LIKE '%%Blueprint%%' THEN 3"
		"          WHEN a.asset_class LIKE '%%NiagaraSystem%%' OR a.asset_class LIKE '%%Material%%' THEN 2"
		"          WHEN a.asset_class LIKE '%%DataTable%%' OR a.asset_class LIKE '%%DataAsset%%' THEN 2"
		"          ELSE 1 END AS class_weight,"
		"        CASE WHEN lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%network%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%replication%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%save%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%serialize%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%auth%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%purchase%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%anticheat%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%crypt%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%exec%%'"
		"            OR lower(a.asset_class || ' ' || a.package_path || ' ' || a.asset_name) LIKE '%%file%%'"
		"          THEN 1 ELSE 0 END AS sensitivity"
		" FROM assets a"
		"), counts AS ("
		" SELECT *, (node_count + variable_count + parameter_count + tag_refs) AS size_signal,"
		"        MIN(1.0, MIN(fan_in,30)/30.0*0.30 + MIN(hard_in,20)/20.0*0.20 + "
		"        MIN(fan_out,30)/30.0*0.10 + (class_weight-1)/4.0*0.20 + "
		"        CASE WHEN sensitivity != 0 THEN 0.15 ELSE 0 END + "
		"        MIN(node_count,400)/400.0*0.15 + MIN(tag_refs,20)/20.0*0.05) AS estimated_risk "
		" FROM base"
		"), scored AS ("
		" SELECT c.id,c.package_path,c.asset_name,c.asset_class,c.module_name,c.fan_in,c.fan_out,c.hard_in,"
		"        c.node_count,c.variable_count,c.parameter_count,c.tag_refs,c.size_signal,"
		"        %s AS risk_score,%s AS risk_tier FROM counts c %s"
		") "
		"SELECT *, MAX(risk_score, MIN(fan_in,30)/30.0, MIN(fan_out,30)/30.0, MIN(size_signal,500)/500.0) AS hotspot_score "
		"FROM scored %s%sLIMIT %d;"),
		*RiskScoreExpr, *RiskTierExpr, *CacheJoin, *WhereClause, *OrderBy, Cap + 1);

	FSQLitePreparedStatement Stmt;
	FJsonArr Hotspots;
	FJsonArr Questions;
	bool bTruncated = false;
	if (Stmt.Create(*Raw, *Sql))
	{
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			if (Hotspots.Num() >= Cap)
			{
				bTruncated = true;
				break;
			}
			int64 Id = 0;
			FString Path, Name, Class, Module, Tier;
			int32 FanIn = 0, FanOut = 0, HardIn = 0, NodeCount = 0, VarCount = 0, ParamCount = 0, TagRefs = 0, SizeSignal = 0;
			double Risk = 0.0;
			Stmt.GetColumnValueByIndex(0, Id);
			Stmt.GetColumnValueByIndex(1, Path);
			Stmt.GetColumnValueByIndex(2, Name);
			Stmt.GetColumnValueByIndex(3, Class);
			Stmt.GetColumnValueByIndex(4, Module);
			Stmt.GetColumnValueByIndex(5, FanIn);
			Stmt.GetColumnValueByIndex(6, FanOut);
			Stmt.GetColumnValueByIndex(7, HardIn);
			Stmt.GetColumnValueByIndex(8, NodeCount);
			Stmt.GetColumnValueByIndex(9, VarCount);
			Stmt.GetColumnValueByIndex(10, ParamCount);
			Stmt.GetColumnValueByIndex(11, TagRefs);
			Stmt.GetColumnValueByIndex(12, SizeSignal);
			Stmt.GetColumnValueByIndex(13, Risk);
			Stmt.GetColumnValueByIndex(14, Tier);

			FString Primary = NormalizedKind;
			if (Primary == TEXT("all"))
			{
				const double InSignal = FMath::Min<double>(FanIn, 30) / 30.0;
				const double OutSignal = FMath::Min<double>(FanOut, 30) / 30.0;
				const double LargeSignal = FMath::Min<double>(SizeSignal, 500) / 500.0;
				Primary = TEXT("risk");
				double Best = Risk;
				if (InSignal > Best) { Best = InSignal; Primary = TEXT("fan_in"); }
				if (OutSignal > Best) { Best = OutSignal; Primary = TEXT("fan_out"); }
				if (LargeSignal > Best) { Primary = TEXT("large"); }
			}

			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("primary_kind"), Primary);
			O->SetNumberField(TEXT("id"), static_cast<double>(Id));
			O->SetStringField(TEXT("asset_path"), Path);
			O->SetStringField(TEXT("asset_name"), Name);
			O->SetStringField(TEXT("asset_class"), Class);
			O->SetStringField(TEXT("module_name"), Module);
			TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
			Metrics->SetNumberField(TEXT("fan_in"), FanIn);
			Metrics->SetNumberField(TEXT("fan_out"), FanOut);
			Metrics->SetNumberField(TEXT("hard_in"), HardIn);
			Metrics->SetNumberField(TEXT("risk_score"), FMath::RoundToDouble(Risk * 1000.0) / 1000.0);
			Metrics->SetStringField(TEXT("risk_tier"), Tier);
			Metrics->SetNumberField(TEXT("nodes"), NodeCount);
			Metrics->SetNumberField(TEXT("variables"), VarCount);
			Metrics->SetNumberField(TEXT("parameters"), ParamCount);
			Metrics->SetNumberField(TEXT("tag_references"), TagRefs);
			Metrics->SetNumberField(TEXT("size_signal"), SizeSignal);
			O->SetObjectField(TEXT("signals"), Metrics);
			O->SetObjectField(TEXT("metrics"), Metrics);
			Hotspots.Add(MakeShared<FJsonValueObject>(O));

			if (bIncludeQuestions && Questions.Num() < 5)
			{
				TSharedPtr<FJsonObject> Q = MakeShared<FJsonObject>();
				Q->SetStringField(TEXT("target"), Path);
				Q->SetStringField(TEXT("reason"), Primary);
				Q->SetStringField(TEXT("question"), Primary == TEXT("large")
					? TEXT("Can this asset's graph/detail size be reduced or validated with focused coverage before editing?")
					: TEXT("Which dependent assets and gameplay paths should be checked before changing this hotspot?"));
				Questions.Add(MakeShared<FJsonValueObject>(Q));
			}
		}
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d project review hotspot(s) ranked by %s%s"),
		Hotspots.Num(), *NormalizedKind, bHasCrg ? TEXT(" using CRG cache when available") : TEXT(" using native fallback")));
	Root->SetArrayField(TEXT("hotspots"), Hotspots);
	if (bIncludeQuestions)
	{
		Root->SetArrayField(TEXT("questions"), Questions);
	}
	Root->SetBoolField(TEXT("truncated"), bTruncated);
	AddNext(Root, { TEXT("project.review_context"), TEXT("project.risk_score"), TEXT("project.impact_radius") });
	return Root;
}

// ============================================================================
// review_context (impact + risk + details + next actions)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::ReviewContext(
	FMonolithIndexDatabase& Db,
	const FString& AssetPath,
	const FString& Direction,
	int32 MaxDepth,
	int32 MaxResults,
	const FString& DetailLevel)
{
	const bool bMinimal = DetailLevel != TEXT("standard");
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("asset_path"), AssetPath);
	Input->SetStringField(TEXT("direction"), Direction);
	Input->SetStringField(TEXT("detail_level"), bMinimal ? TEXT("minimal") : TEXT("standard"));
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_depth"), ClampDepth(MaxDepth));
	Limits->SetNumberField(TEXT("max_results"), bMinimal ? FMath::Min(ClampResults(MaxResults), 25) : ClampResults(MaxResults));
	Root->SetObjectField(TEXT("limits"), Limits);

	const TOptional<FIndexedAsset> Seed = Db.GetAssetByPath(AssetPath);
	if (!Seed.IsSet())
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("project.search") });
		return Root;
	}

	TSharedPtr<FJsonObject> Risk = ScoreAsset(Db, Seed.GetValue());
	Root->SetObjectField(TEXT("risk"), Risk);
	Root->SetArrayField(TEXT("top_risks"), TopRiskReasons(Risk, 5));

	TSharedPtr<FJsonObject> Impact = ImpactRadius(Db, AssetPath,
		Direction.IsEmpty() ? TEXT("both") : Direction, MaxDepth,
		bMinimal ? FMath::Min(MaxResults, 25) : MaxResults, FString());

	// Minimal: keep only impacted count + top entries, drop the full edge list.
	TSharedPtr<FJsonObject> ImpactSummary = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* ImpArr = nullptr;
	if (Impact->TryGetArrayField(TEXT("impacted_assets"), ImpArr) && ImpArr)
	{
		ImpactSummary->SetNumberField(TEXT("impacted_count"), ImpArr->Num());
		FJsonArr Top;
		for (int32 i = 0; i < ImpArr->Num() && i < (bMinimal ? 5 : 25); ++i)
		{
			Top.Add((*ImpArr)[i]);
		}
		ImpactSummary->SetArrayField(TEXT("top_impacted"), Top);
	}
	ImpactSummary->SetBoolField(TEXT("truncated"), Impact->GetBoolField(TEXT("truncated")));
	Root->SetObjectField(TEXT("impact"), ImpactSummary);

	if (!bMinimal)
	{
		if (TSharedPtr<FJsonObject> Details = Db.GetAssetDetails(AssetPath))
		{
			Root->SetObjectField(TEXT("details"), Details);
		}
	}

	TSharedPtr<FJsonObject> SeedObj = MakeShared<FJsonObject>();
	SeedObj->SetStringField(TEXT("asset_path"), Seed->PackagePath);
	SeedObj->SetStringField(TEXT("asset_name"), Seed->AssetName);
	SeedObj->SetStringField(TEXT("asset_class"), Seed->AssetClass);
	Root->SetObjectField(TEXT("seed"), SeedObj);
	FJsonArr Context;
	TSharedPtr<FJsonObject> SeedContext = MakeShared<FJsonObject>();
	SeedContext->SetStringField(TEXT("type"), TEXT("seed_asset"));
	SeedContext->SetStringField(TEXT("asset_path"), Seed->PackagePath);
	SeedContext->SetStringField(TEXT("asset_name"), Seed->AssetName);
	SeedContext->SetStringField(TEXT("asset_class"), Seed->AssetClass);
	SeedContext->SetStringField(TEXT("module_name"), Seed->ModuleName);
	Context.Add(MakeShared<FJsonValueObject>(SeedContext));
	Root->SetArrayField(TEXT("context"), Context);

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%s risk=%s (%s); review the top impacted assets and high-risk reasons"),
		*Seed->AssetName, *Risk->GetStringField(TEXT("tier")),
		bMinimal ? TEXT("minimal") : TEXT("standard")));
	Root->SetBoolField(TEXT("truncated"), Impact->GetBoolField(TEXT("truncated")));
	AddNext(Root, { TEXT("project.get_asset_details"), TEXT("project.impact_radius"), TEXT("project.find_references") });
	return Root;
}
