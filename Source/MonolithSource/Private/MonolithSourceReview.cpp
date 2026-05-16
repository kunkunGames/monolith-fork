#include "MonolithSourceReview.h"
#include "MonolithSourceDatabase.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

namespace
{
	using FJsonArr = TArray<TSharedPtr<FJsonValue>>;

	bool WantsKind(const FString& EdgeKinds, const TCHAR* Kind)
	{
		if (EdgeKinds.IsEmpty()) return true; // default = all
		return EdgeKinds.Contains(Kind);
	}

	TSharedPtr<FJsonObject> SymbolJson(FMonolithSourceDatabase& Db, int64 SymId, int32 Depth)
	{
		const TOptional<FMonolithSourceSymbol> S = Db.GetSymbolById(SymId);
		if (!S.IsSet()) return nullptr;
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("id"), static_cast<double>(S->Id));
		O->SetStringField(TEXT("name"), S->Name);
		O->SetStringField(TEXT("qualified_name"), S->QualifiedName);
		O->SetStringField(TEXT("kind"), S->Kind);
		O->SetStringField(TEXT("file"), Db.GetFilePath(S->FileId));
		O->SetNumberField(TEXT("line"), S->LineStart);
		if (Depth >= 0) O->SetNumberField(TEXT("depth"), Depth);
		return O;
	}

	void AddNext(const TSharedPtr<FJsonObject>& Root, std::initializer_list<const TCHAR*> Acts)
	{
		FJsonArr Arr;
		for (const TCHAR* A : Acts) Arr.Add(MakeShared<FJsonValueString>(FString(A)));
		Root->SetArrayField(TEXT("next_actions"), Arr);
	}

	FString TierFor(double S)
	{
		if (S >= 0.66) return TEXT("high");
		if (S >= 0.33) return TEXT("medium");
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

	/** Shared single-symbol risk used by risk_score and review_context. */
	TSharedPtr<FJsonObject> ScoreSymbol(FMonolithSourceDatabase& Db, const FMonolithSourceSymbol& Sym)
	{
		const TArray<FMonolithSourceReference> Callers = Db.GetReferencesTo(Sym.Id, TEXT(""), 500);
		const TArray<FMonolithSourceReference> Callees = Db.GetReferencesFrom(Sym.Id, TEXT(""), 500);
		const TArray<FMonolithSourceInheritance> Children = Db.GetChildren(Sym.Id);
		const TArray<FMonolithSourceInheritance> Parents = Db.GetParents(Sym.Id);

		TSet<int64> CallerFiles;
		for (const FMonolithSourceReference& R : Callers) CallerFiles.Add(R.FileId);
		const bool bCrossesFiles = CallerFiles.Num() > 1;

		FJsonArr Reasons;
		double Raw = 0.0;
		auto Factor = [&](double C, const FString& Why)
		{
			if (C > 0.0) { Raw += C; Reasons.Add(MakeShared<FJsonValueString>(Why)); }
		};

		Factor(FMath::Min<double>(Callers.Num(), 50) / 50.0 * 0.35,
			FString::Printf(TEXT("caller fan-in: %d"), Callers.Num()));
		Factor(FMath::Min<double>(Children.Num(), 30) / 30.0 * 0.25,
			FString::Printf(TEXT("inheritance descendants (1-hop): %d"), Children.Num()));
		Factor(FMath::Min<double>(Callees.Num(), 50) / 50.0 * 0.10,
			FString::Printf(TEXT("callee fan-out: %d"), Callees.Num()));
		Factor(Sym.bIsUEMacro ? 0.15 : 0.0,
			TEXT("UE reflection macro symbol (UCLASS/UFUNCTION/UPROPERTY family)"));
		Factor(bCrossesFiles ? FMath::Min<double>(CallerFiles.Num(), 20) / 20.0 * 0.15 : 0.0,
			FString::Printf(TEXT("module/file boundary crossing: %d distinct caller file(s)"),
				CallerFiles.Num()));

		if (Callers.Num() == 0 && Sym.Kind.Contains(TEXT("function")))
		{
			Reasons.Add(MakeShared<FJsonValueString>(TEXT(
				"missing direct callers: function has 0 indexed callers — may be "
				"reflection/delegate/Blueprint-invoked (static graph cannot see those)")));
		}

		const double Score = FMath::Clamp(Raw, 0.0, 1.0);
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("id"), static_cast<double>(Sym.Id));
		O->SetStringField(TEXT("name"), Sym.Name);
		O->SetStringField(TEXT("qualified_name"), Sym.QualifiedName);
		O->SetStringField(TEXT("kind"), Sym.Kind);
		O->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
		O->SetStringField(TEXT("tier"), TierFor(Score));
		O->SetArrayField(TEXT("reasons"), Reasons);
		TSharedPtr<FJsonObject> RC = MakeShared<FJsonObject>();
		RC->SetNumberField(TEXT("callers"), Callers.Num());
		RC->SetNumberField(TEXT("callees"), Callees.Num());
		RC->SetNumberField(TEXT("descendants"), Children.Num());
		RC->SetNumberField(TEXT("ancestors"), Parents.Num());
		RC->SetNumberField(TEXT("caller_files"), CallerFiles.Num());
		RC->SetBoolField(TEXT("is_ue_macro"), Sym.bIsUEMacro);
		O->SetObjectField(TEXT("raw_counts"), RC);
		return O;
	}
} // namespace

TSharedPtr<FJsonObject> FMonolithSourceReview::ImpactRadius(
	FMonolithSourceDatabase& Db,
	const FString& Symbol,
	const FString& EdgeKinds,
	const FString& Direction,
	int32 MaxDepth,
	int32 MaxResults)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	const int32 Depth = ClampDepth(MaxDepth);
	const int32 Limit = ClampResults(MaxResults);
	const bool bIn = Direction != TEXT("out");
	const bool bOut = Direction != TEXT("in");
	const bool bCall = WantsKind(EdgeKinds, TEXT("call"));
	const bool bType = WantsKind(EdgeKinds, TEXT("type"));
	const bool bRefs = bCall || bType;
	const bool bInh = WantsKind(EdgeKinds, TEXT("inheritance"));
	const bool bIncludeRequested = !EdgeKinds.IsEmpty() && WantsKind(EdgeKinds, TEXT("include"));

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("symbol"), Symbol);
	Input->SetStringField(TEXT("edge_kinds"), EdgeKinds.IsEmpty() ? TEXT("call|type|inheritance") : EdgeKinds);
	Input->SetStringField(TEXT("direction"), Direction);
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_depth"), Depth);
	Limits->SetNumberField(TEXT("max_results"), Limit);
	Root->SetObjectField(TEXT("limits"), Limits);
	FJsonArr Warnings;
	if (bIncludeRequested)
	{
		Warnings.Add(MakeShared<FJsonValueString>(TEXT(
			"include edges are excluded in P0 until includes.included_path can be resolved to files.path with file-level fixtures")));
	}

	const TArray<FMonolithSourceSymbol> Seeds = Db.GetSymbolsByName(Symbol);
	if (Seeds.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Symbol not found in EngineSource: %s"), *Symbol));
		Root->SetArrayField(TEXT("impacted_symbols"), FJsonArr());
		Root->SetArrayField(TEXT("edges"), FJsonArr());
		Root->SetArrayField(TEXT("warnings"), Warnings);
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("source.search_source"), TEXT("source.read_source") });
		return Root;
	}

	TSet<int64> Visited;
	TArray<int64> Frontier;
	FJsonArr SeedArr;
	for (int32 i = 0; i < Seeds.Num() && i < 5; ++i)
	{
		Visited.Add(Seeds[i].Id);
		Frontier.Add(Seeds[i].Id);
		if (TSharedPtr<FJsonObject> SJ = SymbolJson(Db, Seeds[i].Id, 0))
		{
			SeedArr.Add(MakeShared<FJsonValueObject>(SJ));
		}
	}
	Root->SetArrayField(TEXT("seed_symbols"), SeedArr);

	FJsonArr Impacted;
	FJsonArr Edges;
	bool bTrunc = false;
	int32 Emitted = 0;
	const int32 PerNode = FMath::Clamp(Limit, 50, 500);

	for (int32 D = 1; D <= Depth && Frontier.Num() > 0 && !bTrunc; ++D)
	{
		TArray<int64> Next;
		for (int64 Cur : Frontier)
		{
			TArray<TPair<int64, FString>> Neighbors; // (otherId, edgeKind)

			if (bRefs && bOut)
			{
				TArray<FMonolithSourceReference> Refs;
				if (bCall) Refs.Append(Db.GetReferencesFrom(Cur, TEXT("call"), PerNode));
				if (bType) Refs.Append(Db.GetReferencesFrom(Cur, TEXT("type"), PerNode));
				for (const FMonolithSourceReference& R : Refs)
				{
					Neighbors.Emplace(R.ToSymbolId, R.RefKind.IsEmpty() ? TEXT("ref") : R.RefKind);
				}
			}
			if (bRefs && bIn)
			{
				TArray<FMonolithSourceReference> Refs;
				if (bCall) Refs.Append(Db.GetReferencesTo(Cur, TEXT("call"), PerNode));
				if (bType) Refs.Append(Db.GetReferencesTo(Cur, TEXT("type"), PerNode));
				for (const FMonolithSourceReference& R : Refs)
				{
					Neighbors.Emplace(R.FromSymbolId, R.RefKind.IsEmpty() ? TEXT("ref") : R.RefKind);
				}
			}
			if (bInh && bOut)
			{
				for (const FMonolithSourceInheritance& P : Db.GetParents(Cur))
				{
					Neighbors.Emplace(P.Id, TEXT("inheritance"));
				}
			}
			if (bInh && bIn)
			{
				for (const FMonolithSourceInheritance& C : Db.GetChildren(Cur))
				{
					Neighbors.Emplace(C.Id, TEXT("inheritance"));
				}
			}

			for (const TPair<int64, FString>& N : Neighbors)
			{
				const int64 Other = N.Key;
				if (Other <= 0 || Other == Cur) continue;

				TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetNumberField(TEXT("from"), static_cast<double>(Cur));
				E->SetNumberField(TEXT("to"), static_cast<double>(Other));
				E->SetStringField(TEXT("kind"), N.Value);
				E->SetNumberField(TEXT("depth"), D);
				Edges.Add(MakeShared<FJsonValueObject>(E));

				if (Visited.Contains(Other)) continue;
				Visited.Add(Other);
				if (Emitted >= Limit) { bTrunc = true; break; }
				if (TSharedPtr<FJsonObject> SJ = SymbolJson(Db, Other, D))
				{
					Impacted.Add(MakeShared<FJsonValueObject>(SJ));
					++Emitted;
				}
				Next.Add(Other);
			}
			if (bTrunc) break;
		}
		Frontier = MoveTemp(Next);
	}

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%d impacted symbol(s) within depth %d (%s) of %s%s"),
		Emitted, Depth, *Direction, *Symbol, bTrunc ? TEXT(" [truncated]") : TEXT("")));
	Root->SetArrayField(TEXT("impacted_symbols"), Impacted);
	Root->SetArrayField(TEXT("edges"), Edges);
	Root->SetBoolField(TEXT("truncated"), bTrunc);
	Root->SetArrayField(TEXT("warnings"), Warnings);
	AddNext(Root, { TEXT("source.review_context"), TEXT("source.risk_score"), TEXT("source.find_callers") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceReview::RiskScore(
	FMonolithSourceDatabase& Db,
	const FString& Symbol,
	int32 Limit,
	const FString& MinTier)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("symbol"), Symbol);
	Root->SetObjectField(TEXT("input"), Input);
	const int32 Cap = FMath::Clamp(Limit <= 0 ? 10 : Limit, 1, 100);
	const int32 MinRank = TierRank(MinTier.IsEmpty() ? TEXT("low") : MinTier);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Cap);
	Limits->SetStringField(TEXT("min_tier"), MinTier.IsEmpty() ? TEXT("low") : MinTier);
	Root->SetObjectField(TEXT("limits"), Limits);

	const TArray<FMonolithSourceSymbol> Syms = Db.GetSymbolsByName(Symbol);
	if (Syms.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Symbol not found: %s"), *Symbol));
		Root->SetArrayField(TEXT("items"), FJsonArr());
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("source.search_source") });
		return Root;
	}

	FJsonArr Items;
	for (int32 i = 0; i < Syms.Num() && i < Cap; ++i)
	{
		Items.Add(MakeShared<FJsonValueObject>(ScoreSymbol(Db, Syms[i])));
	}
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
		FString::Printf(TEXT("%d symbol overload(s) scored (scoring_version=1)"), Items.Num()));
	Root->SetStringField(TEXT("scoring_version"), TEXT("1"));
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("source.review_context"), TEXT("source.impact_radius") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceReview::ReviewContext(
	FMonolithSourceDatabase& Db,
	const FString& Symbol,
	const FString& Direction,
	int32 MaxDepth,
	int32 MaxResults,
	const FString& DetailLevel)
{
	const bool bMinimal = DetailLevel != TEXT("standard");
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("symbol"), Symbol);
	Input->SetStringField(TEXT("direction"), Direction);
	Input->SetStringField(TEXT("detail_level"), bMinimal ? TEXT("minimal") : TEXT("standard"));
	Root->SetObjectField(TEXT("input"), Input);
	TSharedPtr<FJsonObject> Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("max_depth"), FMonolithSourceReview::ClampDepth(MaxDepth));
	Limits->SetNumberField(TEXT("max_results"), bMinimal
		? FMath::Min(FMonolithSourceReview::ClampResults(MaxResults), 25)
		: FMonolithSourceReview::ClampResults(MaxResults));
	Root->SetObjectField(TEXT("limits"), Limits);

	const TArray<FMonolithSourceSymbol> Syms = Db.GetSymbolsByName(Symbol);
	if (Syms.Num() == 0)
	{
		Root->SetStringField(TEXT("status"), TEXT("error"));
		Root->SetStringField(TEXT("summary"),
			FString::Printf(TEXT("Symbol not found: %s"), *Symbol));
		Root->SetBoolField(TEXT("truncated"), false);
		AddNext(Root, { TEXT("source.search_source") });
		return Root;
	}

	const FMonolithSourceSymbol& Seed = Syms[0];
	TSharedPtr<FJsonObject> Risk = ScoreSymbol(Db, Seed);
	Root->SetObjectField(TEXT("risk"), Risk);
	Root->SetArrayField(TEXT("top_risks"), TopRiskReasons(Risk, 5));

	TSharedPtr<FJsonObject> Impact = ImpactRadius(Db, Symbol,
		TEXT("call|type|inheritance"),
		Direction.IsEmpty() ? TEXT("both") : Direction,
		MaxDepth, bMinimal ? FMath::Min(MaxResults, 25) : MaxResults);

	TSharedPtr<FJsonObject> ImpactSummary = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* ImpArr = nullptr;
	if (Impact->TryGetArrayField(TEXT("impacted_symbols"), ImpArr) && ImpArr)
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

	TSharedPtr<FJsonObject> SeedObj = MakeShared<FJsonObject>();
	SeedObj->SetStringField(TEXT("name"), Seed.Name);
	SeedObj->SetStringField(TEXT("qualified_name"), Seed.QualifiedName);
	SeedObj->SetStringField(TEXT("kind"), Seed.Kind);
	SeedObj->SetStringField(TEXT("file"), Db.GetFilePath(Seed.FileId));
	SeedObj->SetNumberField(TEXT("line"), Seed.LineStart);
	if (!bMinimal)
	{
		SeedObj->SetStringField(TEXT("signature"), Seed.Signature);
	}
	Root->SetObjectField(TEXT("seed"), SeedObj);
	FJsonArr Context;
	TSharedPtr<FJsonObject> SeedContext = MakeShared<FJsonObject>();
	SeedContext->SetStringField(TEXT("type"), TEXT("seed_symbol"));
	SeedContext->SetStringField(TEXT("name"), Seed.Name);
	SeedContext->SetStringField(TEXT("qualified_name"), Seed.QualifiedName);
	SeedContext->SetStringField(TEXT("file"), Db.GetFilePath(Seed.FileId));
	SeedContext->SetNumberField(TEXT("line"), Seed.LineStart);
	Context.Add(MakeShared<FJsonValueObject>(SeedContext));
	Root->SetArrayField(TEXT("context"), Context);

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%s risk=%s (%s); review impacted symbols and risk reasons"),
		*Seed.Name, *Risk->GetStringField(TEXT("tier")),
		bMinimal ? TEXT("minimal") : TEXT("standard")));
	Root->SetBoolField(TEXT("truncated"), Impact->GetBoolField(TEXT("truncated")));
	AddNext(Root, { TEXT("source.read_source"), TEXT("source.impact_radius"), TEXT("source.get_class_hierarchy") });
	return Root;
}
