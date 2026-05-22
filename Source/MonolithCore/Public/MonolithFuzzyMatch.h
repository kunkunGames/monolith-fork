// Copyright tumourlove. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/** Per-field token-scoring weights. Fuzzy=0 disables typo tolerance for the field. */
struct FMonolithFuzzyWeights
{
	int32 Exact = 0;
	int32 Prefix = 0;
	int32 Contains = 0;
	int32 Fuzzy = 0;
};

/** One scored field of a candidate (whole-field phrase text + tokens + weights). */
struct FMonolithFuzzyField
{
	/** Normalized field text, used for whole-query phrase matching. */
	FString Text;
	/** Tokenized field text, used for per-token matching. */
	TArray<FString> Tokens;
	/** Per-token scoring weights. */
	FMonolithFuzzyWeights Weights;
	/** Whole-query phrase bonuses against this field's Text (exact > prefix > contains). */
	int32 ExactPhraseBonus = 0;
	int32 PrefixPhraseBonus = 0;
	int32 ContainsPhraseBonus = 0;
	/** Reason-tag stem, e.g. TEXT("asset_name"); emits "<stem>", "<stem>_exact", etc. */
	const TCHAR* ReasonTag = nullptr;
};

/** Result of scoring one candidate. */
struct FMonolithFuzzyScore
{
	int32 Score = 0;
	TArray<FString> Reasons;
	TArray<FString> MatchedTokens;
	/** Smallest typo edit distance that contributed, or MAX_int32 when no fuzzy match contributed. */
	int32 BestDistance = MAX_int32;
};

/**
 * Shared fuzzy text-matching primitives used by monolith.find, FindSimilarActions,
 * and asset.find_assets. Pure (Core-only); no editor/asset dependencies.
 *
 * Design rule: this owns only the genuinely identical primitives — normalization,
 * tokenization, bounded edit distance, typo gating, weighted token scoring, and a
 * per-field composition convenience. Callers keep their own corpus, field set,
 * weights, thresholds, cross-field bonuses, and output shape.
 */
class MONOLITHCORE_API FMonolithFuzzyMatch
{
public:
	/** Lowercase, fold a few code tokens (c++/c#), strip non-alnum to spaces, collapse whitespace. */
	static FString NormalizeText(FString Text);

	/**
	 * Normalize + split + drop stopwords / <2-char tokens + dedupe. When AliasTable is
	 * non-null, each base token present as a key expands to its values (deduped, snapshot
	 * based so expansions are not themselves re-expanded).
	 */
	static TArray<FString> Tokenize(const FString& Text, const TMap<FString, TArray<FString>>* AliasTable = nullptr);

	/**
	 * Banded Levenshtein with early-out once the running row minimum exceeds MaxDistance
	 * (returns MaxDistance+1). For unbounded-style use pass a MaxDistance at least as large
	 * as the threshold you compare against (callers do this; keep MaxDistance small to stay
	 * well clear of INT32 overflow). bCaseInsensitive lowercases per character before compare.
	 *
	 * bAllowTransposition switches to the optimal-string-alignment (restricted Damerau) variant:
	 * an adjacent transposition counts as one edit instead of two (e.g. "form" <-> "from").
	 * Default false preserves plain Levenshtein for existing callers. With transposition on the
	 * row-minimum early-out is disabled (a transposition can skip a row, making that bound unsafe);
	 * the length-difference early-out still applies.
	 */
	static int32 EditDistanceBounded(const FString& A, const FString& B, int32 MaxDistance, bool bCaseInsensitive = false, bool bAllowTransposition = false);

	/** Typo gate: both >=4 chars, same first char, edit distance <= (2 if either >=7 else 1). bAllowTransposition counts an adjacent swap as one edit. */
	static bool IsTypoMatch(const FString& QueryToken, const FString& FieldToken, bool bAllowTransposition = false);

	/**
	 * Score one field's tokens against the query tokens: exact/prefix/contains/fuzzy weighted,
	 * capturing reasons and matched tokens. When OutBestDistance is non-null, records the
	 * smallest contributing typo distance. Returns the field's score contribution.
	 */
	static int32 ScoreTokens(
		const TArray<FString>& QueryTokens,
		const TArray<FString>& FieldTokens,
		const FString& FieldText,
		const FMonolithFuzzyWeights& Weights,
		const TCHAR* Reason,
		TArray<FString>& OutReasons,
		TSet<FString>& OutMatchedTokens,
		int32* OutBestDistance = nullptr,
		bool bAllowTransposition = false);

	/**
	 * Per-field composition convenience for Search callers: for each field applies the
	 * whole-query phrase bonus (exact > prefix > contains) then ScoreTokens. Cross-field
	 * bonuses remain caller policy and are NOT applied here.
	 */
	static FMonolithFuzzyScore ScoreCandidate(
		const FString& QueryNormalized,
		const TArray<FString>& QueryTokens,
		TArrayView<const FMonolithFuzzyField> Fields,
		bool bAllowTransposition = false);
};
