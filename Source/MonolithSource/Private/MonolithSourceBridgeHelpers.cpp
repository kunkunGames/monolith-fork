#include "MonolithSourceBridgeHelpers.h"

#include "Misc/Paths.h"

namespace
{
void AddUniqueCandidate(TArray<FString>& Out, TSet<FString>& Seen, const FString& Candidate)
{
	FString Cleaned = MonolithSourceBridge::CleanBridgeToken(Candidate);
	Cleaned.TrimStartAndEndInline();
	if (Cleaned.IsEmpty())
	{
		return;
	}

	const FString Key = Cleaned.ToLower();
	if (!Seen.Contains(Key))
	{
		Seen.Add(Key);
		Out.Add(Cleaned);
	}
}

bool StartsWithAnyPrefix(const FString& Value, const TCHAR* const* Prefixes, int32 PrefixCount, FString& OutTrimmed)
{
	for (int32 Index = 0; Index < PrefixCount; ++Index)
	{
		const FString Prefix(Prefixes[Index]);
		if (Value.StartsWith(Prefix, ESearchCase::IgnoreCase) && Value.Len() > Prefix.Len())
		{
			OutTrimmed = Value.Mid(Prefix.Len());
			return true;
		}
	}
	return false;
}

bool IsGenericAssetClass(const FString& Value)
{
	static const TSet<FString> GenericClasses = {
		TEXT("Blueprint"),
		TEXT("WidgetBlueprint"),
		TEXT("AnimBlueprint"),
		TEXT("DataAsset"),
		TEXT("PrimaryDataAsset"),
		TEXT("Material"),
		TEXT("MaterialInstanceConstant"),
		TEXT("Texture2D"),
		TEXT("StaticMesh"),
		TEXT("SkeletalMesh")
	};
	return GenericClasses.Contains(Value);
}
}

namespace MonolithSourceBridge
{
FString CleanBridgeToken(const FString& Value)
{
	FString Cleaned = Value;
	Cleaned.TrimStartAndEndInline();
	Cleaned.ReplaceInline(TEXT("\\"), TEXT("/"));

	int32 SeparatorIndex = INDEX_NONE;
	if (Cleaned.FindLastChar(TEXT('/'), SeparatorIndex) && SeparatorIndex + 1 < Cleaned.Len())
	{
		Cleaned = Cleaned.Mid(SeparatorIndex + 1);
	}
	if (Cleaned.FindLastChar(TEXT('.'), SeparatorIndex) && SeparatorIndex + 1 < Cleaned.Len())
	{
		Cleaned = Cleaned.Mid(SeparatorIndex + 1);
	}
	if (Cleaned.FindLastChar(TEXT(':'), SeparatorIndex) && SeparatorIndex + 1 < Cleaned.Len())
	{
		Cleaned = Cleaned.Mid(SeparatorIndex + 1);
	}

	if (Cleaned.EndsWith(TEXT("_C"), ESearchCase::IgnoreCase) && Cleaned.Len() > 2)
	{
		Cleaned.LeftChopInline(2);
	}
	return Cleaned;
}

FString NormalizeBridgeName(const FString& Value)
{
	FString Normalized = CleanBridgeToken(Value);

	static const TCHAR* AssetPrefixes[] = {
		TEXT("BP_"),
		TEXT("B_"),
		TEXT("WBP_"),
		TEXT("ABP_"),
		TEXT("DA_"),
		TEXT("PDA_"),
		TEXT("GA_"),
		TEXT("GE_"),
		TEXT("GC_"),
		TEXT("GCN_")
	};

	bool bTrimmed = true;
	while (bTrimmed)
	{
		FString Trimmed;
		bTrimmed = StartsWithAnyPrefix(Normalized, AssetPrefixes, UE_ARRAY_COUNT(AssetPrefixes), Trimmed);
		if (bTrimmed)
		{
			Normalized = Trimmed;
		}
	}

	if (Normalized.Len() > 2)
	{
		const TCHAR First = Normalized[0];
		const TCHAR Second = Normalized[1];
		if ((First == TEXT('U') || First == TEXT('A') || First == TEXT('F') || First == TEXT('I')) && FChar::IsUpper(Second))
		{
			Normalized = Normalized.Mid(1);
		}
	}
	return Normalized;
}

TArray<FString> BuildAssetSymbolCandidates(const FString& AssetPath, const FString& AssetName, const FString& AssetClass)
{
	TArray<FString> Candidates;
	TSet<FString> Seen;

	const FString CleanAssetName = CleanBridgeToken(AssetName.IsEmpty() ? AssetPath : AssetName);
	const FString CleanPathName = CleanBridgeToken(AssetPath);
	AddUniqueCandidate(Candidates, Seen, CleanAssetName);
	AddUniqueCandidate(Candidates, Seen, CleanPathName);

	const FString NormalizedAssetName = NormalizeBridgeName(CleanAssetName);
	AddUniqueCandidate(Candidates, Seen, NormalizedAssetName);
	if (!NormalizedAssetName.IsEmpty())
	{
		AddUniqueCandidate(Candidates, Seen, TEXT("U") + NormalizedAssetName);
		AddUniqueCandidate(Candidates, Seen, TEXT("A") + NormalizedAssetName);
		AddUniqueCandidate(Candidates, Seen, TEXT("F") + NormalizedAssetName);
	}

	const FString CleanAssetClass = CleanBridgeToken(AssetClass);
	if (!CleanAssetClass.IsEmpty() && !IsGenericAssetClass(CleanAssetClass))
	{
		AddUniqueCandidate(Candidates, Seen, CleanAssetClass);
		AddUniqueCandidate(Candidates, Seen, NormalizeBridgeName(CleanAssetClass));
	}

	return Candidates;
}

TArray<FString> BuildSymbolAssetCandidates(const FString& SymbolName, const FString& QualifiedName)
{
	TArray<FString> Candidates;
	TSet<FString> Seen;

	const FString CleanSymbolName = CleanBridgeToken(SymbolName.IsEmpty() ? QualifiedName : SymbolName);
	const FString CleanQualifiedName = CleanBridgeToken(QualifiedName);
	AddUniqueCandidate(Candidates, Seen, CleanSymbolName);
	AddUniqueCandidate(Candidates, Seen, CleanQualifiedName);

	const FString Normalized = NormalizeBridgeName(CleanSymbolName);
	AddUniqueCandidate(Candidates, Seen, Normalized);
	if (!Normalized.IsEmpty())
	{
		AddUniqueCandidate(Candidates, Seen, TEXT("BP_") + Normalized);
		AddUniqueCandidate(Candidates, Seen, TEXT("WBP_") + Normalized);
		AddUniqueCandidate(Candidates, Seen, TEXT("DA_") + Normalized);
		AddUniqueCandidate(Candidates, Seen, TEXT("GA_") + Normalized);
		AddUniqueCandidate(Candidates, Seen, TEXT("GE_") + Normalized);
	}

	return Candidates;
}

bool NamesMatchNormalized(const FString& Left, const FString& Right)
{
	return NormalizeBridgeName(Left).Equals(NormalizeBridgeName(Right), ESearchCase::IgnoreCase);
}
}
