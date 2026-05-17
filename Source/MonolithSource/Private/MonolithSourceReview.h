#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FMonolithSourceDatabase;

/**
 * CRG-inspired review/navigation over the EXISTING EngineSource.db symbol graph.
 *
 * Adapted from code-review-graph (0919071a) — recursive impact traversal,
 * minimal token-efficient output, factor-decomposed risk — but NOT its generic
 * schema, Python runtime, parser, embeddings, or daemon. Source-symbol native:
 * only the existing `symbols`/`"references"`/`inheritance` graph via the public
 * FMonolithSourceDatabase query surface (each call self-locks). `include` edges
 * are intentionally excluded (file/path-level, not a symbol edge).
 *
 * health/repair_fts live on FMonolithSourceDatabase itself (private DbLock).
 */
class FMonolithSourceReview
{
public:
	/** Bounded BFS over "references" + inheritance. direction: in|out|both. */
	static TSharedPtr<FJsonObject> ImpactRadius(
		FMonolithSourceDatabase& Db,
		const FString& Symbol,
		const FString& EdgeKinds,
		const FString& Direction,
		int32 MaxDepth,
		int32 MaxResults);

	/** Query-time risk: caller/callee degree, descendants, UE macro, fan. */
	static TSharedPtr<FJsonObject> RiskScore(
		FMonolithSourceDatabase& Db,
		const FString& Symbol,
		int32 Limit,
		const FString& MinTier);

	/** Rank global source review hotspots by fan-in, fan-out, risk, LOC, or all. */
	static TSharedPtr<FJsonObject> ReviewHotspots(
		FMonolithSourceDatabase& Db,
		const FString& Kind,
		int32 Limit,
		int32 MinLines,
		bool bIncludeQuestions);

	/** Compose impact + risk + callers/callees/hierarchy + next actions. */
	static TSharedPtr<FJsonObject> ReviewContext(
		FMonolithSourceDatabase& Db,
		const FString& Symbol,
		const FString& Direction,
		int32 MaxDepth,
		int32 MaxResults,
		const FString& DetailLevel);

	static int32 ClampDepth(int32 In) { return FMath::Clamp(In <= 0 ? 2 : In, 1, 6); }
	static int32 ClampResults(int32 In) { return FMath::Clamp(In <= 0 ? 200 : In, 1, 2000); }

	static FString PStr(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, const FString& Def = TEXT(""))
	{
		FString S;
		return (P.IsValid() && P->TryGetStringField(Key, S)) ? S : Def;
	}
	static int32 PInt(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, int32 Def)
	{
		if (!P.IsValid()) return Def;
		double D;
		if (P->TryGetNumberField(Key, D)) return static_cast<int32>(D);
		FString S;
		if (P->TryGetStringField(Key, S) && !S.IsEmpty()) return FCString::Atoi(*S);
		return Def;
	}
	static bool PBool(const TSharedPtr<FJsonObject>& P, const TCHAR* Key, bool Def)
	{
		if (!P.IsValid()) return Def;
		bool B;
		if (P->TryGetBoolField(Key, B)) return B;
		FString S;
		if (P->TryGetStringField(Key, S)) return S == TEXT("true") || S == TEXT("1");
		return Def;
	}
};
