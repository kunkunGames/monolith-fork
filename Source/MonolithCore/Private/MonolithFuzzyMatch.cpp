// SPDX-License-Identifier: MIT

#include "MonolithFuzzyMatch.h"

#include "Algo/LevenshteinDistance.h"
#include "Algo/StableSort.h"

namespace
{
	bool IsTokenChar(TCHAR Ch)
	{
		return FChar::IsAlnum(Ch);
	}

	bool IsIgnoredToken(const FString& Token)
	{
		static const TSet<FString> IgnoredTokens = {
			TEXT("a"), TEXT("an"), TEXT("and"), TEXT("for"), TEXT("from"), TEXT("in"), TEXT("of"),
			TEXT("on"), TEXT("or"), TEXT("the"), TEXT("to"), TEXT("with"), TEXT("using"), TEXT("use")
		};
		return IgnoredTokens.Contains(Token);
	}

	void AddUniqueToken(TArray<FString>& Tokens, TSet<FString>& Seen, const FString& Token)
	{
		if (Token.Len() < 2 || IsIgnoredToken(Token) || Seen.Contains(Token))
		{
			return;
		}
		Seen.Add(Token);
		Tokens.Add(Token);
	}

	int32 GetTypoMaxDistance(const FString& QueryToken, const FString& FieldToken)
	{
		if (QueryToken.Len() < 4 || FieldToken.Len() < 4 || QueryToken[0] != FieldToken[0])
		{
			return -1;
		}
		return FMath::Max(QueryToken.Len(), FieldToken.Len()) >= 7 ? 2 : 1;
	}
}

FString FMonolithFuzzyMatch::NormalizeText(FString Text)
{
	Text.ToLowerInline();
	Text.ReplaceInline(TEXT("c++"), TEXT(" cpp "));
	Text.ReplaceInline(TEXT("c#"), TEXT(" csharp "));
	Text.ReplaceInline(TEXT("blueprintassist"), TEXT(" blueprint assist "));

	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		if (!IsTokenChar(Text[Index]))
		{
			Text[Index] = TEXT(' ');
		}
	}

	TArray<FString> Tokens;
	Text.ParseIntoArrayWS(Tokens);
	return FString::Join(Tokens, TEXT(" "));
}

TArray<FString> FMonolithFuzzyMatch::Tokenize(
	const FString& Text,
	const TMap<FString, TArray<FString>>* AliasTable)
{
	TArray<FString> RawTokens;
	NormalizeText(Text).ParseIntoArrayWS(RawTokens);

	TArray<FString> Tokens;
	TSet<FString> Seen;
	for (const FString& Token : RawTokens)
	{
		AddUniqueToken(Tokens, Seen, Token);
	}

	if (AliasTable)
	{
		const TArray<FString> Snapshot = Tokens;
		for (const FString& Token : Snapshot)
		{
			if (const TArray<FString>* Expansions = AliasTable->Find(Token))
			{
				for (const FString& Alias : *Expansions)
				{
					AddUniqueToken(Tokens, Seen, Alias);
				}
			}
		}
	}

	return Tokens;
}

int32 FMonolithFuzzyMatch::EditDistanceBounded(
	const FString& A,
	const FString& B,
	int32 MaxDistance,
	bool bCaseInsensitive,
	bool bAllowTransposition)
{
	if (MaxDistance < 0)
	{
		return MAX_int32;
	}

	const int32 La = A.Len();
	const int32 Lb = B.Len();
	if (FMath::Abs(La - Lb) > MaxDistance)
	{
		return MaxDistance + 1;
	}
	if (La == 0)
	{
		return Lb;
	}
	if (Lb == 0)
	{
		return La;
	}

	auto Fold = [bCaseInsensitive](TCHAR Ch)
	{
		return bCaseInsensitive ? FChar::ToLower(Ch) : Ch;
	};

	TArray<int32> PrevPrev;
	TArray<int32> Prev;
	TArray<int32> Curr;
	PrevPrev.SetNumUninitialized(Lb + 1);
	Prev.SetNumUninitialized(Lb + 1);
	Curr.SetNumUninitialized(Lb + 1);
	for (int32 J = 0; J <= Lb; ++J)
	{
		Prev[J] = J;
	}

	for (int32 I = 1; I <= La; ++I)
	{
		Curr[0] = I;
		int32 RowMin = Curr[0];
		const TCHAR Ca = Fold(A[I - 1]);
		const TCHAR CaPrev = I >= 2 ? Fold(A[I - 2]) : TEXT('\0');
		for (int32 J = 1; J <= Lb; ++J)
		{
			const TCHAR Cb = Fold(B[J - 1]);
			const int32 Cost = Ca == Cb ? 0 : 1;
			int32 Best = FMath::Min3(
				Prev[J] + 1,
				Curr[J - 1] + 1,
				Prev[J - 1] + Cost);
			if (bAllowTransposition && I >= 2 && J >= 2
				&& Ca == Fold(B[J - 2]) && CaPrev == Cb)
			{
				Best = FMath::Min(Best, PrevPrev[J - 2] + 1);
			}
			Curr[J] = Best;
			RowMin = FMath::Min(RowMin, Best);
		}
		if (!bAllowTransposition && RowMin > MaxDistance)
		{
			return MaxDistance + 1;
		}
		Swap(PrevPrev, Prev);
		Swap(Prev, Curr);
	}

	return Prev[Lb];
}

bool FMonolithFuzzyMatch::IsTypoMatch(
	const FString& QueryToken,
	const FString& FieldToken,
	bool bAllowTransposition)
{
	const int32 MaxDistance = GetTypoMaxDistance(QueryToken, FieldToken);
	return MaxDistance >= 0
		&& EditDistanceBounded(
			QueryToken,
			FieldToken,
			MaxDistance,
			/*bCaseInsensitive=*/false,
			bAllowTransposition) <= MaxDistance;
}

int32 FMonolithFuzzyMatch::ScoreTokens(
	const TArray<FString>& QueryTokens,
	const TArray<FString>& FieldTokens,
	const FString& FieldText,
	const FMonolithFuzzyWeights& Weights,
	const TCHAR* Reason,
	TArray<FString>& OutReasons,
	TSet<FString>& OutMatchedTokens,
	int32* OutBestDistance,
	bool bAllowTransposition)
{
	int32 Score = 0;
	bool bAnyMatch = false;
	bool bAnyFuzzyMatch = false;

	for (const FString& Token : QueryTokens)
	{
		bool bMatched = false;
		if (FieldTokens.Contains(Token))
		{
			Score += Weights.Exact;
			bMatched = true;
		}
		else
		{
			for (const FString& FieldToken : FieldTokens)
			{
				if (FieldToken.StartsWith(Token))
				{
					Score += Weights.Prefix;
					bMatched = true;
					break;
				}
			}
		}

		if (!bMatched && FieldText.Contains(Token))
		{
			Score += Weights.Contains;
			bMatched = true;
		}

		if (!bMatched && Weights.Fuzzy > 0)
		{
			for (const FString& FieldToken : FieldTokens)
			{
				const int32 MaxDistance = GetTypoMaxDistance(Token, FieldToken);
				if (MaxDistance < 0)
				{
					continue;
				}

				const int32 Distance = EditDistanceBounded(
					Token,
					FieldToken,
					MaxDistance,
					/*bCaseInsensitive=*/false,
					bAllowTransposition);
				if (Distance <= MaxDistance)
				{
					Score += Weights.Fuzzy;
					bMatched = true;
					bAnyFuzzyMatch = true;
					if (OutBestDistance && Distance < *OutBestDistance)
					{
						*OutBestDistance = Distance;
					}
					break;
				}
			}
		}

		if (bMatched)
		{
			bAnyMatch = true;
			OutMatchedTokens.Add(Token);
		}
	}

	if (bAnyMatch)
	{
		OutReasons.Add(Reason);
	}
	if (bAnyFuzzyMatch)
	{
		OutReasons.Add(FString::Printf(TEXT("%s_fuzzy"), Reason));
	}
	return Score;
}

FMonolithFuzzyScore FMonolithFuzzyMatch::ScoreCandidate(
	const FString& QueryNormalized,
	const TArray<FString>& QueryTokens,
	TArrayView<const FMonolithFuzzyField> Fields,
	bool bAllowTransposition)
{
	FMonolithFuzzyScore Result;

	for (const FMonolithFuzzyField& Field : Fields)
	{
		const FString Stem = Field.ReasonTag
			? FString(Field.ReasonTag)
			: FString(TEXT("field"));
		if (!QueryNormalized.IsEmpty())
		{
			if (Field.ExactPhraseBonus != 0 && Field.Text == QueryNormalized)
			{
				Result.Score += Field.ExactPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_exact"));
			}
			else if (Field.PrefixPhraseBonus != 0 && Field.Text.StartsWith(QueryNormalized))
			{
				Result.Score += Field.PrefixPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_prefix"));
			}
			else if (Field.ContainsPhraseBonus != 0 && Field.Text.Contains(QueryNormalized))
			{
				Result.Score += Field.ContainsPhraseBonus;
				Result.Reasons.Add(Stem + TEXT("_phrase"));
			}
		}

		TSet<FString> Matched;
		Result.Score += ScoreTokens(
			QueryTokens,
			Field.Tokens,
			Field.Text,
			Field.Weights,
			Field.ReasonTag ? Field.ReasonTag : TEXT("field"),
			Result.Reasons,
			Matched,
			&Result.BestDistance,
			bAllowTransposition);

		for (const FString& Token : Matched)
		{
			Result.MatchedTokens.AddUnique(Token);
		}
	}

	return Result;
}

namespace MonolithFuzzyMatchDetail
{
	TArray<FFuzzyCandidate> ScoreFuzzyMatches(
		const FString& Needle,
		const TArray<FString>& KeysSnapshot,
		int32 TopN)
	{
		TArray<FFuzzyCandidate> Result;

		if (Needle.IsEmpty() || KeysSnapshot.Num() == 0 || TopN <= 0)
		{
			return Result;
		}

		// Score every candidate. Pattern matches the verified precedent at
		// Engine/Plugins/Animation/IKRig/Source/IKRigEditor/Private/RigEditor/
		// IKRigAutoCharacterizer.cpp:1761:
		//
		//   const float Score = 1.0f - (
		//     static_cast<float>(Algo::LevenshteinDistance(NameToMatchStr, CurrentNameStr))
		//     / WorstCase);
		//
		// WorstCase guards against div-by-zero for two empty strings (already
		// excluded by the Needle.IsEmpty check above, but cheap defence) and
		// normalises across needle/candidate length disparity.
		TArray<FFuzzyCandidate> All;
		All.Reserve(KeysSnapshot.Num());

		for (const FString& Key : KeysSnapshot)
		{
			const int32 Distance = Algo::LevenshteinDistance(Needle, Key);
			const int32 MaxLen = FMath::Max(Needle.Len(), Key.Len());
			const float WorstCase = MaxLen > 0 ? static_cast<float>(MaxLen) : 1.0f;
			const float Score = 1.0f - (static_cast<float>(Distance) / WorstCase);

			FFuzzyCandidate C;
			C.Key = Key;
			C.Score = Score;
			All.Add(MoveTemp(C));
		}

		// Sort descending by Score. Use StableSort so equal-score candidates
		// retain insertion order (KeysSnapshot order), which the dispatch path
		// already controls via the registry's TMap iteration order.
		Algo::StableSort(All, [](const FFuzzyCandidate& A, const FFuzzyCandidate& B)
		{
			return A.Score > B.Score;
		});

		const int32 Take = FMath::Min(TopN, All.Num());
		Result.Reserve(Take);
		for (int32 i = 0; i < Take; ++i)
		{
			Result.Add(All[i]);
		}
		return Result;
	}
}
