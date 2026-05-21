// Copyright tumourlove. All Rights Reserved.
#include "MonolithAssetFindActions.h"

#include "MonolithFuzzyMatch.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// Asset-oriented alias expansion (single-word tokens only). Distinct from the
	// MCP-routing alias table used by monolith.find.
	const TMap<FString, TArray<FString>>& GetAssetAliasTable()
	{
		static const TMap<FString, TArray<FString>> AliasTable = {
			{ TEXT("tex"), { TEXT("texture") } },
			{ TEXT("mat"), { TEXT("material") } },
			{ TEXT("bp"), { TEXT("blueprint") } },
			{ TEXT("sk"), { TEXT("skeletal") } },
			{ TEXT("sm"), { TEXT("static"), TEXT("mesh") } },
			{ TEXT("mi"), { TEXT("material"), TEXT("instance") } },
			{ TEXT("abp"), { TEXT("animation"), TEXT("blueprint") } },
		};
		return AliasTable;
	}

	// Short, useful AssetRegistry tag values eligible for scoring when include_tags=true.
	// Large source/import payloads and filesystem paths are deliberately excluded.
	const TArray<FName>& GetTagWhitelist()
	{
		static const TArray<FName> Tags = {
			FName(TEXT("DisplayName")), FName(TEXT("Description")), FName(TEXT("Tooltip")),
			FName(TEXT("ParentClass")), FName(TEXT("NativeParentClass")), FName(TEXT("GeneratedClass")), FName(TEXT("BlueprintParentClass")),
			FName(TEXT("Skeleton")), FName(TEXT("PreviewMesh"))
		};
		return Tags;
	}

	void AppendField(TArray<FMonolithFuzzyField>& Fields, const FString& RawText, const FMonolithFuzzyWeights& Weights,
		int32 ExactPhrase, int32 PrefixPhrase, int32 ContainsPhrase, const TCHAR* ReasonTag)
	{
		FMonolithFuzzyField Field;
		Field.Text = FMonolithFuzzyMatch::NormalizeText(RawText);
		Field.Tokens = FMonolithFuzzyMatch::Tokenize(RawText);
		Field.Weights = Weights;
		Field.ExactPhraseBonus = ExactPhrase;
		Field.PrefixPhraseBonus = PrefixPhrase;
		Field.ContainsPhraseBonus = ContainsPhrase;
		Field.ReasonTag = ReasonTag;
		Fields.Add(MoveTemp(Field));
	}

	// Build the scored fields for one asset. Never loads the asset (FAssetData only).
	TArray<FMonolithFuzzyField> BuildFieldsForAsset(const FAssetData& Data, bool bIncludeTags)
	{
		TArray<FMonolithFuzzyField> Fields;

		AppendField(Fields, Data.AssetName.ToString(), FMonolithFuzzyWeights{ 45, 30, 16, 8 }, 200, 130, 90, TEXT("asset_name"));
		AppendField(Fields, Data.PackageName.ToString(), FMonolithFuzzyWeights{ 22, 14, 8, 4 }, 0, 0, 0, TEXT("path"));
		AppendField(Fields, Data.AssetClassPath.GetAssetName().ToString(), FMonolithFuzzyWeights{ 25, 16, 8, 0 }, 60, 0, 0, TEXT("class"));

		if (bIncludeTags)
		{
			TArray<FString> Values;
			for (const FName& Tag : GetTagWhitelist())
			{
				FString Value;
				if (Data.GetTagValue(Tag, Value) && !Value.IsEmpty())
				{
					Values.Add(Value);
				}
			}
			if (Values.Num() > 0)
			{
				AppendField(Fields, FString::Join(Values, TEXT(" ")), FMonolithFuzzyWeights{ 10, 6, 4, 0 }, 0, 0, 0, TEXT("tags"));
			}
		}

		return Fields;
	}
}

bool FMonolithAssetFindActions::ResolveClassNames(const TArray<FString>& InClassNames, TArray<FTopLevelAssetPath>& OutPaths, TArray<FString>& OutUnknown)
{
	for (const FString& Raw : InClassNames)
	{
		const FString Entry = Raw.TrimStartAndEnd();
		if (Entry.IsEmpty())
		{
			OutUnknown.Add(Raw);
			continue;
		}

		UClass* Resolved = nullptr;
		if (Entry.Contains(TEXT(".")))
		{
			// Full class path form: /Script/Module.ClassName.
			Resolved = FindObject<UClass>(nullptr, *Entry);
		}
		else
		{
			// Class name form (project convention; native asset classes are always loaded).
			Resolved = FindFirstObject<UClass>(*Entry, EFindFirstObjectOptions::NativeFirst);
		}

		if (Resolved)
		{
			OutPaths.AddUnique(Resolved->GetClassPathName());
		}
		else
		{
			OutUnknown.Add(Entry);
		}
	}
	return OutUnknown.Num() == 0;
}

bool FMonolithAssetFindActions::RunAssetFind(const FAssetFindRequest& Request, FAssetFindResult& OutResult, FString& OutError, TSharedPtr<FJsonObject>& OutErrorData)
{
	// --- Validate path (semantic) ---
	FString Path = Request.Path;
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Path.IsEmpty() || !Path.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(TEXT("Invalid parameter 'path': '%s' is not a content path."), *Request.Path);
		OutErrorData = MakeShared<FJsonObject>();
		OutErrorData->SetStringField(TEXT("param"), TEXT("path"));
		OutErrorData->SetStringField(TEXT("reason"), TEXT("invalid_package_path"));
		OutErrorData->SetStringField(TEXT("path"), Request.Path);
		return false;
	}
	if (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}

	// --- Resolve class filters (semantic) ---
	TArray<FTopLevelAssetPath> ClassPaths;
	TArray<FString> Unknown;
	if (!ResolveClassNames(Request.ClassNames, ClassPaths, Unknown))
	{
		OutError = FString::Printf(TEXT("Invalid parameter 'class_names': unknown class name(s): %s."), *FString::Join(Unknown, TEXT(", ")));
		OutErrorData = MakeShared<FJsonObject>();
		OutErrorData->SetStringField(TEXT("param"), TEXT("class_names"));
		OutErrorData->SetStringField(TEXT("reason"), TEXT("unknown_class_names"));
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& U : Unknown) { Arr.Add(MakeShared<FJsonValueString>(U)); }
		OutErrorData->SetArrayField(TEXT("unknown_class_names"), Arr);
		OutErrorData->SetStringField(TEXT("hint"), TEXT("Pass a UClass name (e.g. Texture2D, Blueprint, StaticMesh) or a full /Script/Module.ClassName path."));
		return false;
	}

	IAssetRegistry* AR = IAssetRegistry::Get();
	if (!AR)
	{
		OutError = TEXT("AssetRegistry is unavailable.");
		return false;
	}

	// --- Pre-filter via FARFilter (path + class), then fuzzy-score the filtered set ---
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Path));
	Filter.bRecursivePaths = Request.bRecursive;
	for (const FTopLevelAssetPath& ClassPath : ClassPaths)
	{
		Filter.ClassPaths.Add(ClassPath);
	}
	Filter.bRecursiveClasses = ClassPaths.Num() > 0;

	TArray<FAssetData> Assets;
	AR->GetAssets(Filter, Assets);

	const int32 ScanBudget = FMath::Max(1, Request.ScanBudget);
	OutResult.FilteredCount = Assets.Num();
	OutResult.ScannedCount = FMath::Min(Assets.Num(), ScanBudget);
	OutResult.bTruncated = Assets.Num() > ScanBudget;

	const FString QueryNormalized = FMonolithFuzzyMatch::NormalizeText(Request.Query);
	const TArray<FString> QueryTokens = FMonolithFuzzyMatch::Tokenize(Request.Query, &GetAssetAliasTable());

	for (int32 Index = 0; Index < OutResult.ScannedCount; ++Index)
	{
		const FAssetData& Data = Assets[Index];
		const TArray<FMonolithFuzzyField> Fields = BuildFieldsForAsset(Data, Request.bIncludeTags);
		const FMonolithFuzzyScore Score = FMonolithFuzzyMatch::ScoreCandidate(QueryNormalized, QueryTokens, Fields);

		const bool bKeep = Request.Threshold.IsSet() ? (Score.Score >= Request.Threshold.GetValue()) : (Score.Score > 0);
		if (!bKeep)
		{
			continue;
		}

		FAssetFindRow Row;
		Row.ObjectPath = Data.GetSoftObjectPath().ToString();
		Row.PackageName = Data.PackageName.ToString();
		Row.AssetName = Data.AssetName.ToString();
		Row.ClassName = Data.AssetClassPath.GetAssetName().ToString();
		Row.ClassPath = Data.AssetClassPath.ToString();
		Row.Score = Score.Score;
		Row.BestDistance = Score.BestDistance;

		if (Request.bIncludeScoreBreakdown)
		{
			Row.Reason = FString::Join(Score.Reasons, TEXT(","));
			Row.MatchedTokens = Score.MatchedTokens;
			for (const FMonolithFuzzyField& Field : Fields)
			{
				TArray<FMonolithFuzzyField> Single;
				Single.Add(Field);
				const FMonolithFuzzyScore FieldScore = FMonolithFuzzyMatch::ScoreCandidate(QueryNormalized, QueryTokens, Single);
				if (Field.ReasonTag)
				{
					Row.FieldScores.Add(FString(Field.ReasonTag), FieldScore.Score);
				}
			}
		}

		OutResult.Matches.Add(MoveTemp(Row));
	}

	OutResult.MatchedCount = OutResult.Matches.Num();

	// Deterministic order: higher score, then lower edit distance, then shorter path, then lexicographic.
	OutResult.Matches.Sort([](const FAssetFindRow& A, const FAssetFindRow& B)
	{
		if (A.Score != B.Score) { return A.Score > B.Score; }
		if (A.BestDistance != B.BestDistance) { return A.BestDistance < B.BestDistance; }
		if (A.ObjectPath.Len() != B.ObjectPath.Len()) { return A.ObjectPath.Len() < B.ObjectPath.Len(); }
		return A.ObjectPath < B.ObjectPath;
	});

	if (OutResult.Matches.Num() > Request.Limit)
	{
		OutResult.bLimited = true;
		OutResult.Matches.SetNum(Request.Limit);
	}

	return true;
}

FMonolithActionResult FMonolithAssetFindActions::FindAssets(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Missing parameters."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FAssetFindRequest Request;

	if (!Params->TryGetStringField(TEXT("query"), Request.Query) || Request.Query.TrimStartAndEnd().IsEmpty())
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("param"), TEXT("query"));
		Data->SetStringField(TEXT("reason"), TEXT("missing_or_empty"));
		return FMonolithActionResult::Error(TEXT("Invalid parameter 'query': required non-empty string."), FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data);
	}

	if (Params->HasField(TEXT("path")) && !Params->TryGetStringField(TEXT("path"), Request.Path))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter 'path': must be a string."), FMonolithJsonUtils::ErrInvalidParams);
	}

	if (Params->HasField(TEXT("recursive")) && !Params->TryGetBoolField(TEXT("recursive"), Request.bRecursive))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter 'recursive': must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
	}

	// class_names accepts an array of strings, or a single string (alias 'class').
	if (Params->HasField(TEXT("class_names")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Params->TryGetArrayField(TEXT("class_names"), Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Arr)
			{
				FString Entry;
				if (!Value.IsValid() || !Value->TryGetString(Entry) || Entry.TrimStartAndEnd().IsEmpty())
				{
					return FMonolithActionResult::Error(TEXT("Invalid parameter 'class_names': each entry must be a non-empty string."), FMonolithJsonUtils::ErrInvalidParams);
				}
				Request.ClassNames.Add(Entry);
			}
		}
		else
		{
			FString Single;
			if (!Params->TryGetStringField(TEXT("class_names"), Single) || Single.TrimStartAndEnd().IsEmpty())
			{
				return FMonolithActionResult::Error(TEXT("Invalid parameter 'class_names': must be a string or array of strings."), FMonolithJsonUtils::ErrInvalidParams);
			}
			Request.ClassNames.Add(Single);
		}
	}

	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 20.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter 'limit': must be an integer."), FMonolithJsonUtils::ErrInvalidParams);
		}
		Request.Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 100);
	}

	if (Params->HasField(TEXT("threshold")))
	{
		double ThresholdValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("threshold"), ThresholdValue) || ThresholdValue < 0.0)
		{
			return FMonolithActionResult::Error(TEXT("Invalid parameter 'threshold': must be a non-negative integer."), FMonolithJsonUtils::ErrInvalidParams);
		}
		Request.Threshold = static_cast<int32>(ThresholdValue);
	}

	if (Params->HasField(TEXT("include_tags")) && !Params->TryGetBoolField(TEXT("include_tags"), Request.bIncludeTags))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter 'include_tags': must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
	}

	if (Params->HasField(TEXT("include_score_breakdown")) && !Params->TryGetBoolField(TEXT("include_score_breakdown"), Request.bIncludeScoreBreakdown))
	{
		return FMonolithActionResult::Error(TEXT("Invalid parameter 'include_score_breakdown': must be a boolean."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FAssetFindResult Search;
	FString Error;
	TSharedPtr<FJsonObject> ErrorData;
	if (!RunAssetFind(Request, Search, Error, ErrorData))
	{
		FMonolithActionResult Failure = FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams);
		if (ErrorData.IsValid())
		{
			Failure.WithErrorData(ErrorData);
		}
		return Failure;
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Search.Matches.Num());
	for (const FAssetFindRow& Row : Search.Matches)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("object_path"), Row.ObjectPath);
		Obj->SetStringField(TEXT("package_name"), Row.PackageName);
		Obj->SetStringField(TEXT("asset_name"), Row.AssetName);
		Obj->SetStringField(TEXT("class"), Row.ClassName);
		Obj->SetStringField(TEXT("class_path"), Row.ClassPath);
		Obj->SetNumberField(TEXT("score"), Row.Score);
		if (Request.bIncludeScoreBreakdown)
		{
			Obj->SetStringField(TEXT("reason"), Row.Reason);
			TArray<TSharedPtr<FJsonValue>> Tokens;
			for (const FString& Token : Row.MatchedTokens)
			{
				Tokens.Add(MakeShared<FJsonValueString>(Token));
			}
			Obj->SetArrayField(TEXT("matched_tokens"), Tokens);
			if (Row.BestDistance != MAX_int32)
			{
				Obj->SetNumberField(TEXT("distance"), Row.BestDistance);
			}
			else
			{
				Obj->SetField(TEXT("distance"), MakeShared<FJsonValueNull>());
			}
			TSharedPtr<FJsonObject> Breakdown = MakeShared<FJsonObject>();
			for (const TPair<FString, int32>& Pair : Row.FieldScores)
			{
				Breakdown->SetNumberField(Pair.Key, Pair.Value);
			}
			Obj->SetObjectField(TEXT("score_breakdown"), Breakdown);
		}
		Rows.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ok"));
	Result->SetStringField(TEXT("query"), Request.Query);
	Result->SetStringField(TEXT("path"), Request.Path);
	Result->SetBoolField(TEXT("recursive"), Request.bRecursive);
	Result->SetStringField(TEXT("scoring_version"), TEXT("asset_fuzzy_v1"));
	Result->SetNumberField(TEXT("count"), Search.Matches.Num());
	Result->SetNumberField(TEXT("matched_count"), Search.MatchedCount);
	Result->SetNumberField(TEXT("filtered_count"), Search.FilteredCount);
	Result->SetNumberField(TEXT("scanned_count"), Search.ScannedCount);
	Result->SetBoolField(TEXT("truncated"), Search.bTruncated);
	Result->SetBoolField(TEXT("limited"), Search.bLimited);
	Result->SetNumberField(TEXT("scan_budget"), Request.ScanBudget);
	Result->SetArrayField(TEXT("matches"), Rows);
	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueString>(TEXT("asset.inspect_asset")));
	Result->SetArrayField(TEXT("next_actions"), NextActions);
	return FMonolithActionResult::Success(Result);
}

void FMonolithAssetFindActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("asset"), TEXT("find_assets"),
		TEXT("Fuzzy, scored, typo-tolerant search over the live AssetRegistry. Ranks assets by name/path/class with edit-distance typo tolerance; sees unsaved assets created this session. Results feed asset.inspect_asset. Distinct from offline project search."),
		FMonolithActionHandler::CreateStatic(&FMonolithAssetFindActions::FindAssets),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("query"), TEXT("string"), TEXT("Asset name or task text to search for."))
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path scope, e.g. /Game or /Game/Characters."), TEXT("/Game"))
			.Optional(TEXT("recursive"), TEXT("boolean"), TEXT("Recurse subfolders under path."), TEXT("true"))
			.Optional(TEXT("class_names"), TEXT("array"), TEXT("Filter by asset class names (e.g. Texture2D, Blueprint) or /Script/Module.ClassName paths."), { TEXT("class") })
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum ranked rows to return."), TEXT("20"))
			.Range(TEXT("limit"), 1, 100)
			.Optional(TEXT("threshold"), TEXT("integer"), TEXT("Minimum raw asset_fuzzy_v1 score to keep a match."), TEXT(""))
			.Optional(TEXT("include_tags"), TEXT("boolean"), TEXT("Also score selected AssetRegistry tag values."), TEXT("false"))
			.Optional(TEXT("include_score_breakdown"), TEXT("boolean"), TEXT("Include reason, matched_tokens, distance, and per-field score breakdown."), TEXT("false"))
			.Build());
}
