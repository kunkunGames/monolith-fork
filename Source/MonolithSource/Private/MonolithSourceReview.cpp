#include "MonolithSourceReview.h"
#include "MonolithSourceDatabase.h"
#include "MonolithFuzzyMatch.h"
#include "Dom/JsonValue.h"
#include <initializer_list>

namespace
{
	using FJsonArr = TArray<TSharedPtr<FJsonValue>>;
	constexpr const TCHAR* ExpectedScoringVersion = TEXT("3");

	bool WantsKind(const FString& EdgeKinds, const TCHAR* Kind)
	{
		if (EdgeKinds.IsEmpty()) return true;
		return EdgeKinds.Contains(Kind);
	}

	FString PathStatus(const FString& Path)
	{
		return Path.IsEmpty() || Path == TEXT("<unknown>") ? TEXT("missing") : TEXT("known");
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
		const FString FilePath = Db.GetFilePath(S->FileId);
		O->SetStringField(TEXT("file"), FilePath);
		O->SetStringField(TEXT("path_status"), PathStatus(FilePath));
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

	bool ReviewAllowsSensitivityPrefix(const FString& Token)
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

	bool ReviewMatchesSensitivityToken(const FString& CandidateToken, const FString& SensitiveToken)
	{
		if (CandidateToken == SensitiveToken)
		{
			return true;
		}
		return ReviewAllowsSensitivityPrefix(SensitiveToken) && CandidateToken.StartsWith(SensitiveToken);
	}

	void ReviewAddSensitivityCandidateToken(TArray<FString>& Tokens, TSet<FString>& Seen, FString Token)
	{
		Token.ToLowerInline();
		if (Token.IsEmpty() || Seen.Contains(Token))
		{
			return;
		}
		Seen.Add(Token);
		Tokens.Add(MoveTemp(Token));
	}

	TArray<FString> ReviewBuildSensitivityCandidateTokens(const FString& Text)
	{
		TArray<FString> Tokens;
		TSet<FString> Seen;
		for (const FString& Token : FMonolithFuzzyMatch::Tokenize(Text))
		{
			ReviewAddSensitivityCandidateToken(Tokens, Seen, Token);
		}

		FString Current;
		TCHAR Previous = TEXT('\0');
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Ch = Text[Index];
			if (!FChar::IsAlnum(Ch))
			{
				ReviewAddSensitivityCandidateToken(Tokens, Seen, Current);
				Current.Empty();
				Previous = TEXT('\0');
				continue;
			}
			if (!Current.IsEmpty()
				&& FChar::IsUpper(Ch)
				&& (FChar::IsLower(Previous) || FChar::IsDigit(Previous)))
			{
				ReviewAddSensitivityCandidateToken(Tokens, Seen, Current);
				Current.Empty();
			}
			Current.AppendChar(Ch);
			Previous = Ch;
		}
		ReviewAddSensitivityCandidateToken(Tokens, Seen, Current);
		return Tokens;
	}

	bool ReviewContainsAnyToken(const FString& Text, std::initializer_list<const TCHAR*> Tokens, FString* OutMatchedToken = nullptr)
	{
		const TArray<FString> CandidateTokens = ReviewBuildSensitivityCandidateTokens(Text);
		for (const FString& CandidateToken : CandidateTokens)
		{
			if (CandidateToken.IsEmpty())
			{
				continue;
			}
			for (const TCHAR* Token : Tokens)
			{
				const FString SensitiveToken(Token);
				if (ReviewMatchesSensitivityToken(CandidateToken, SensitiveToken))
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

	double SensitivityFactor(const FString& Text, FString& OutReason)
	{
		auto MatchedReason = [](const TCHAR* Reason, const FString& Token)
		{
			return Token.IsEmpty()
				? FString(Reason)
				: FString::Printf(TEXT("%s (token=%s)"), Reason, *Token);
		};

		FString MatchedToken;
		if (ReviewContainsAnyToken(Text, { TEXT("ufunction"), TEXT("server"), TEXT("client"), TEXT("netmulticast"), TEXT("onrep"), TEXT("replication"), TEXT("rpc"), TEXT("network") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: replication/RPC or network surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("save"), TEXT("serialize"), TEXT("archive") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: save/serialization surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("auth"), TEXT("login"), TEXT("account"), TEXT("session") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: auth/account/session surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("purchase"), TEXT("iap"), TEXT("store"), TEXT("entitlement") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: purchase/store entitlement surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("anticheat"), TEXT("anti_cheat"), TEXT("cheat") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: anticheat surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("crypt"), TEXT("crypto"), TEXT("encrypt"), TEXT("decrypt"), TEXT("sign"), TEXT("signature"), TEXT("signed"), TEXT("signing"), TEXT("hash") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: crypto/signing/hash surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("exec"), TEXT("eval"), TEXT("command") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: exec/eval/command surface"), MatchedToken);
			return 0.15;
		}
		if (ReviewContainsAnyToken(Text, { TEXT("file"), TEXT("registry"), TEXT("process") }, &MatchedToken))
		{
			OutReason = MatchedReason(TEXT("sensitivity: file/registry/process surface"), MatchedToken);
			return 0.15;
		}
		return 0.0;
	}

	int32 TierRank(const FString& Tier)
	{
		if (Tier == TEXT("high")) return 2;
		if (Tier == TEXT("medium")) return 1;
		return 0;
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

	FJsonArr TopRiskReasons(const TSharedPtr<FJsonObject>& Risk, int32 MaxItems)
	{
		FJsonArr Out;
		const TArray<TSharedPtr<FJsonValue>>* Reasons = nullptr;
		if (Risk.IsValid() && Risk->TryGetArrayField(TEXT("reasons"), Reasons) && Reasons)
		{
			Out.Reserve(FMath::Min(Reasons->Num(), MaxItems));
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
		const TArray<FMonolithSourceOverrideEdge> OverrideChildren = Db.GetOverridesTo(Sym.Id, 1000);
		const TArray<FMonolithSourceOverrideEdge> OverrideParents = Db.GetOverridesFrom(Sym.Id, 100);

		auto ApplyOverrideRisk = [&](TSharedPtr<FJsonObject> Risk) -> TSharedPtr<FJsonObject>
		{
			if (!Risk.IsValid() || (OverrideChildren.Num() == 0 && OverrideParents.Num() == 0))
			{
				return Risk;
			}

			double Score = 0.0;
			Risk->TryGetNumberField(TEXT("score"), Score);
			FJsonArr Reasons;
			const TArray<TSharedPtr<FJsonValue>>* ExistingReasons = nullptr;
			if (Risk->TryGetArrayField(TEXT("reasons"), ExistingReasons) && ExistingReasons)
			{
				Reasons = *ExistingReasons;
			}

			const double OverrideChildFactor = FMath::Min<double>(OverrideChildren.Num(), 30) / 30.0 * 0.20;
			const double OverrideParentFactor = FMath::Min<double>(OverrideParents.Num(), 10) / 10.0 * 0.05;
			if (OverrideChildFactor > 0.0)
			{
				Reasons.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("override fan-out: %d child override method(s)"),
					OverrideChildren.Num())));
			}
			if (OverrideParentFactor > 0.0)
			{
				Reasons.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("overrides parent method(s): %d"),
					OverrideParents.Num())));
			}
			Score = FMath::Clamp(Score + OverrideChildFactor + OverrideParentFactor, 0.0, 1.0);
			Risk->SetNumberField(TEXT("score"), FMath::RoundToDouble(Score * 1000.0) / 1000.0);
			Risk->SetStringField(TEXT("tier"), TierFor(Score));
			Risk->SetArrayField(TEXT("reasons"), Reasons);

			const TSharedPtr<FJsonObject>* RawCountsPtr;
			TSharedPtr<FJsonObject> RawCounts;
			if (Risk->TryGetObjectField(TEXT("raw_counts"), RawCountsPtr) && RawCountsPtr && RawCountsPtr->IsValid())
			{
				RawCounts = *RawCountsPtr;
			}
			else
			{
				RawCounts = MakeShared<FJsonObject>();
			}
			RawCounts->SetNumberField(TEXT("override_children"), OverrideChildren.Num());
			RawCounts->SetNumberField(TEXT("overridden_parents"), OverrideParents.Num());
			Risk->SetObjectField(TEXT("raw_counts"), RawCounts);

			const TSharedPtr<FJsonObject>* CachePtr;
			if (Risk->TryGetObjectField(TEXT("cache"), CachePtr) && CachePtr && CachePtr->IsValid())
			{
				TSharedPtr<FJsonObject> Cache = *CachePtr;
				Cache->SetBoolField(TEXT("override_augmented"), true);
				Risk->SetObjectField(TEXT("cache"), Cache);
			}
			return Risk;
		};

		if (TSharedPtr<FJsonObject> Cached = Db.GetCachedRiskForSymbol(Sym.Id))
		{
			return ApplyOverrideRisk(Cached);
		}

		const TArray<FMonolithSourceReference> Callers = Db.GetReferencesTo(Sym.Id, TEXT(""), 500);
		const TArray<FMonolithSourceReference> Callees = Db.GetReferencesFrom(Sym.Id, TEXT(""), 500);
		const TArray<FMonolithSourceInheritance> Children = Db.GetChildren(Sym.Id);
		const TArray<FMonolithSourceInheritance> Parents = Db.GetParents(Sym.Id);

		TSet<int64> CallerFiles;
		CallerFiles.Reserve(Callers.Num());
		for (const FMonolithSourceReference& R : Callers) CallerFiles.Add(R.FileId);
		const bool bCrossesFiles = CallerFiles.Num() > 1;
		FString SensitivityReason;
		const double Sensitivity = SensitivityFactor(
			FString::Printf(TEXT("%s %s %s %s"), *Sym.Name, *Sym.QualifiedName, *Sym.Kind, *Sym.Signature),
			SensitivityReason);

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
		Factor(FMath::Min<double>(OverrideChildren.Num(), 30) / 30.0 * 0.20,
			FString::Printf(TEXT("override fan-out: %d child override method(s)"), OverrideChildren.Num()));
		Factor(FMath::Min<double>(Callees.Num(), 50) / 50.0 * 0.10,
			FString::Printf(TEXT("callee fan-out: %d"), Callees.Num()));
		Factor(FMath::Min<double>(OverrideParents.Num(), 10) / 10.0 * 0.05,
			FString::Printf(TEXT("overrides parent method(s): %d"), OverrideParents.Num()));
		Factor(Sym.bIsUEMacro ? 0.15 : 0.0,
			TEXT("UE reflection macro symbol (UCLASS/UFUNCTION/UPROPERTY family)"));
		Factor(bCrossesFiles ? FMath::Min<double>(CallerFiles.Num(), 20) / 20.0 * 0.15 : 0.0,
			FString::Printf(TEXT("module/file boundary crossing: %d distinct caller file(s)"),
				CallerFiles.Num()));
		Factor(Sensitivity, SensitivityReason);

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
		RC->SetNumberField(TEXT("override_children"), OverrideChildren.Num());
		RC->SetNumberField(TEXT("overridden_parents"), OverrideParents.Num());
		RC->SetNumberField(TEXT("caller_files"), CallerFiles.Num());
		RC->SetBoolField(TEXT("is_ue_macro"), Sym.bIsUEMacro);
		RC->SetNumberField(TEXT("sensitivity"), Sensitivity);
		O->SetObjectField(TEXT("raw_counts"), RC);
		O->SetObjectField(TEXT("cache"), CacheMeta(TEXT("miss"), TEXT(""), ExpectedScoringVersion));
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
	const FString EffectiveEdgeKinds = EdgeKinds.IsEmpty() ? TEXT("call|type|inheritance") : EdgeKinds;
	const bool bIn = Direction != TEXT("out");
	const bool bOut = Direction != TEXT("in");
	const bool bCall = WantsKind(EffectiveEdgeKinds, TEXT("call"));
	const bool bType = WantsKind(EffectiveEdgeKinds, TEXT("type"));
	const bool bRefs = bCall || bType;
	const bool bInh = WantsKind(EffectiveEdgeKinds, TEXT("inheritance"));
	const bool bOverride = WantsKind(EffectiveEdgeKinds, TEXT("override"));
	const bool bIncludeRequested = !EdgeKinds.IsEmpty() && WantsKind(EffectiveEdgeKinds, TEXT("include"));

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("symbol"), Symbol);
	Input->SetStringField(TEXT("edge_kinds"), EffectiveEdgeKinds);
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
			if (bOverride && bOut)
			{
				for (const FMonolithSourceOverrideEdge& O : Db.GetOverridesFrom(Cur, PerNode))
				{
					Neighbors.Emplace(O.ToSymbolId, TEXT("override"));
				}
			}
			if (bOverride && bIn)
			{
				for (const FMonolithSourceOverrideEdge& O : Db.GetOverridesTo(Cur, PerNode))
				{
					Neighbors.Emplace(O.FromSymbolId, TEXT("override"));
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
	AddNext(Root, { TEXT("source.review_context"), TEXT("source.find_overrides"), TEXT("source.risk_score"), TEXT("source.find_callers") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceReview::FindOverrides(
	FMonolithSourceDatabase& Db,
	const FString& Symbol,
	const FString& Direction,
	int32 MaxDepth,
	int32 MaxResults,
	const FString& DetailLevel)
{
	const FString EffectiveDirection = Direction.IsEmpty() ? TEXT("both") : Direction;
	const bool bStandardDetail = DetailLevel.Equals(TEXT("standard"), ESearchCase::IgnoreCase)
		|| DetailLevel.Equals(TEXT("full"), ESearchCase::IgnoreCase);
	TSharedPtr<FJsonObject> Root = ImpactRadius(
		Db,
		Symbol,
		TEXT("override"),
		EffectiveDirection,
		MaxDepth,
		MaxResults);

	const TArray<TSharedPtr<FJsonValue>>* Impacted = nullptr;
	if (Root->TryGetArrayField(TEXT("impacted_symbols"), Impacted) && Impacted)
	{
		Root->SetArrayField(TEXT("overrides"), *Impacted);
		Root->SetStringField(TEXT("summary"), FString::Printf(
			TEXT("%d override-related symbol(s) within depth %d (%s) of %s"),
			Impacted->Num(),
			ClampDepth(MaxDepth),
			*EffectiveDirection,
			*Symbol));
	}
	Root->SetStringField(TEXT("detail_level"), bStandardDetail ? TEXT("standard") : TEXT("minimal"));

	const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
	if (Root->TryGetArrayField(TEXT("edges"), Edges) && Edges)
	{
		const int32 EdgeCount = Edges->Num();
		Root->SetNumberField(TEXT("edge_count"), EdgeCount);
		if (!bStandardDetail)
		{
			const int32 EdgeSampleLimit = 25;
			FJsonArr EdgeSample;
			EdgeSample.Reserve(FMath::Min(EdgeCount, EdgeSampleLimit));
			for (int32 Index = 0; Index < EdgeCount && Index < EdgeSampleLimit; ++Index)
			{
				EdgeSample.Add((*Edges)[Index]);
			}
			Root->SetArrayField(TEXT("edges"), EdgeSample);
			Root->SetBoolField(TEXT("edges_truncated"), EdgeCount > EdgeSample.Num());
			Root->RemoveField(TEXT("impacted_symbols"));
		}
		else
		{
			Root->SetBoolField(TEXT("edges_truncated"), false);
		}
	}
	AddNext(Root, { TEXT("source.impact_radius"), TEXT("source.review_context"), TEXT("source.risk_score") });
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
	Items.Reserve(FMath::Min(Syms.Num(), Cap));
	for (int32 i = 0; i < Syms.Num() && i < Cap; ++i)
	{
		Items.Add(MakeShared<FJsonValueObject>(ScoreSymbol(Db, Syms[i])));
	}
	Items.RemoveAll([&](const TSharedPtr<FJsonValue>& V)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FString Tier;
		return O.IsValid() && O->TryGetStringField(TEXT("tier"), Tier) && TierRank(Tier) < MinRank;
	});
	Items.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		double SA = 0.0;
		if (A->AsObject().IsValid())
		{
			A->AsObject()->TryGetNumberField(TEXT("score"), SA);
		}
		double SB = 0.0;
		if (B->AsObject().IsValid())
		{
			B->AsObject()->TryGetNumberField(TEXT("score"), SB);
		}
		return SA > SB;
	});

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"),
		FString::Printf(TEXT("%d symbol overload(s) scored (scoring_version=3 cached when available, v3 query fallback)"), Items.Num()));
	Root->SetStringField(TEXT("scoring_version"), ExpectedScoringVersion);
	Root->SetArrayField(TEXT("items"), Items);
	Root->SetBoolField(TEXT("truncated"), false);
	AddNext(Root, { TEXT("source.repair_crg_cache"), TEXT("source.review_context"), TEXT("source.find_overrides"), TEXT("source.impact_radius") });
	return Root;
}

TSharedPtr<FJsonObject> FMonolithSourceReview::ReviewHotspots(
	FMonolithSourceDatabase& Db,
	const FString& Kind,
	int32 Limit,
	int32 MinLines,
	bool bIncludeQuestions)
{
	return Db.ReviewHotspots(Kind, Limit, MinLines, bIncludeQuestions);
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
		TEXT("call|type|inheritance|override"),
		Direction.IsEmpty() ? TEXT("both") : Direction,
		MaxDepth, bMinimal ? FMath::Min(MaxResults, 25) : MaxResults);

	TSharedPtr<FJsonObject> ImpactSummary = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* ImpArr = nullptr;
	if (Impact->TryGetArrayField(TEXT("impacted_symbols"), ImpArr) && ImpArr)
	{
		ImpactSummary->SetNumberField(TEXT("impacted_count"), ImpArr->Num());
		FJsonArr Top;
		TSet<FString> EmittedExactKeys;
		TSet<FString> EmittedKnownSymbolKeys;
		const int32 TopLimit = bMinimal ? 5 : 25;
		for (int32 i = 0; i < ImpArr->Num() && Top.Num() < TopLimit; ++i)
		{
			TSharedPtr<FJsonObject> Obj = (*ImpArr)[i]->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FString QualifiedName;
			Obj->TryGetStringField(TEXT("qualified_name"), QualifiedName);
			FString Name;
			Obj->TryGetStringField(TEXT("name"), Name);
			FString Kind;
			Obj->TryGetStringField(TEXT("kind"), Kind);
			const FString SymbolKey = FString::Printf(TEXT("%s|%s"),
				*(QualifiedName.IsEmpty() ? Name : QualifiedName), *Kind);
			FString FilePath;
			Obj->TryGetStringField(TEXT("file"), FilePath);
			FString Status;
			Obj->TryGetStringField(TEXT("path_status"), Status);
			const bool bKnownPath = Status == TEXT("known") || (!FilePath.IsEmpty() && FilePath != TEXT("<unknown>"));
			const FString ExactKey = FString::Printf(TEXT("%s|%s"), *SymbolKey, *FilePath);
			if (EmittedExactKeys.Contains(ExactKey))
			{
				continue;
			}
			if (!bKnownPath && EmittedKnownSymbolKeys.Contains(SymbolKey))
			{
				continue;
			}
			EmittedExactKeys.Add(ExactKey);
			if (bKnownPath)
			{
				EmittedKnownSymbolKeys.Add(SymbolKey);
			}
			Top.Add((*ImpArr)[i]);
		}
		ImpactSummary->SetArrayField(TEXT("top_impacted"), Top);
	}
	bool bImpactTruncated = false;
	Impact->TryGetBoolField(TEXT("truncated"), bImpactTruncated);
	ImpactSummary->SetBoolField(TEXT("truncated"), bImpactTruncated);
	Root->SetObjectField(TEXT("impact"), ImpactSummary);

	TSharedPtr<FJsonObject> SeedObj = MakeShared<FJsonObject>();
	SeedObj->SetStringField(TEXT("name"), Seed.Name);
	SeedObj->SetStringField(TEXT("qualified_name"), Seed.QualifiedName);
	SeedObj->SetStringField(TEXT("kind"), Seed.Kind);
	const FString SeedFilePath = Db.GetFilePath(Seed.FileId);
	SeedObj->SetStringField(TEXT("file"), SeedFilePath);
	SeedObj->SetStringField(TEXT("path_status"), PathStatus(SeedFilePath));
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
	SeedContext->SetStringField(TEXT("file"), SeedFilePath);
	SeedContext->SetStringField(TEXT("path_status"), PathStatus(SeedFilePath));
	SeedContext->SetNumberField(TEXT("line"), Seed.LineStart);
	Context.Add(MakeShared<FJsonValueObject>(SeedContext));
	Root->SetArrayField(TEXT("context"), Context);

	FString RiskTier;
	Risk->TryGetStringField(TEXT("tier"), RiskTier);

	Root->SetStringField(TEXT("status"), TEXT("ok"));
	Root->SetStringField(TEXT("summary"), FString::Printf(
		TEXT("%s risk=%s (%s); review impacted symbols and risk reasons"),
		*Seed.Name, *RiskTier,
		bMinimal ? TEXT("minimal") : TEXT("standard")));
	Root->SetBoolField(TEXT("truncated"), bImpactTruncated);
	AddNext(Root, { TEXT("source.read_source"), TEXT("source.impact_radius"), TEXT("source.find_overrides"), TEXT("source.get_class_hierarchy") });
	return Root;
}
