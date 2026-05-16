#include "MonolithIndexReview.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexLog.h"
#include "SQLiteDatabase.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

// ============================================================================
// Local helpers
// ============================================================================
namespace
{
	using FJsonArr = TArray<TSharedPtr<FJsonValue>>;

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
		// UE-domain risk weighting (replaces CRG's security/flow factors).
		if (AssetClass.Contains(TEXT("World")) || AssetClass.Contains(TEXT("Level"))) return 5;
		if (AssetClass.Contains(TEXT("GameplayAbility")) || AssetClass.Contains(TEXT("AttributeSet"))
			|| AssetClass.Contains(TEXT("GameplayEffect"))) return 4;
		if (AssetClass.Contains(TEXT("Blueprint"))) return 3;
		if (AssetClass.Contains(TEXT("NiagaraSystem")) || AssetClass.Contains(TEXT("Material"))) return 2;
		if (AssetClass.Contains(TEXT("DataTable")) || AssetClass.Contains(TEXT("DataAsset"))) return 2;
		return 1;
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

	/** Single-asset risk computation shared by risk_index and review_context. */
	TSharedPtr<FJsonObject> ScoreAsset(FMonolithIndexDatabase& Db, const FIndexedAsset& Asset)
	{
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
		O->SetObjectField(TEXT("raw_counts"), RawCounts);
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
	AddNext(Root, { TEXT("project.review_context"), TEXT("project.risk_index"), TEXT("project.get_asset_details") });
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
	AddNext(Root, { TEXT("project.repair_fts"), TEXT("project.get_stats") });
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
// risk_index (query-time)
// ============================================================================
TSharedPtr<FJsonObject> FMonolithIndexReview::RiskIndex(
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
		FString::Printf(TEXT("%d asset(s) scored (scoring_version=1)"), Items.Num()));
	Root->SetStringField(TEXT("scoring_version"), TEXT("1"));
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("project.review_context"), TEXT("project.impact_radius") });
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
