#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "ProjectSearchTextProjection.h"
#include "Editor.h"

namespace
{
constexpr int32 GProjectSearchMaxLimit = 1000;
constexpr int32 GProjectSearchMaxWindow = 100000;

bool TryParseProjectSearchCursor(const FString& Cursor, int32& OutOffset)
{
	if (Cursor.IsEmpty())
	{
		return false;
	}
	for (int32 Index = 0; Index < Cursor.Len(); ++Index)
	{
		if (!FChar::IsDigit(Cursor[Index]))
		{
			return false;
		}
	}
	const int64 ParsedOffset = FCString::Atoi64(*Cursor);
	if (ParsedOffset < 0 || ParsedOffset > GProjectSearchMaxWindow)
	{
		return false;
	}
	OutOffset = static_cast<int32>(ParsedOffset);
	return true;
}

FString CompactProjectSearchMatchValue(
	const FString& Value,
	bool bDetail,
	int32& OutLength,
	bool& bOutTruncated)
{
	OutLength = MonolithProjectSearchText::CountUnicodeCodePoints(Value);
	bOutTruncated = !bDetail && OutLength > MonolithProjectSearchText::PreviewCodePoints;
	return bOutTruncated
		? MonolithProjectSearchText::LeftUnicodeCodePoints(
			Value,
			MonolithProjectSearchText::PreviewCodePoints)
		: Value;
}
}

FMonolithActionResult FProjectSearchAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (Params->HasField(TEXT("query")))
	{
		if (!Params->TryGetStringField(TEXT("query"), Query))
		{
			return FMonolithActionResult::Error(TEXT("'query' parameter must be a string"), -32602);
		}
	}
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required and cannot be empty"), -32602);
	}

	int32 Limit = 50;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue)
			|| !FMath::IsFinite(LimitValue)
			|| FMath::FloorToDouble(LimitValue) != LimitValue)
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be an integer"), -32602);
		}
		Limit = static_cast<int32>(FMath::Clamp(
			LimitValue,
			1.0,
			static_cast<double>(GProjectSearchMaxLimit)));
	}

	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		double OffsetValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("offset"), OffsetValue)
			|| !FMath::IsFinite(OffsetValue)
			|| FMath::FloorToDouble(OffsetValue) != OffsetValue)
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be an integer"), -32602);
		}
		if (OffsetValue < 0.0 || OffsetValue > static_cast<double>(GProjectSearchMaxWindow))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be within 0..100000"), -32602);
		}
		Offset = static_cast<int32>(OffsetValue);
	}
	if (Params->HasField(TEXT("cursor")))
	{
		FString Cursor;
		if (!Params->TryGetStringField(TEXT("cursor"), Cursor))
		{
			return FMonolithActionResult::Error(TEXT("'cursor' parameter must be a string"), -32602);
		}
		if (!TryParseProjectSearchCursor(Cursor, Offset))
		{
			return FMonolithActionResult::Error(TEXT("'cursor' parameter must be a non-negative numeric offset cursor"), -32602);
		}
	}

	bool bIncludeContent = true;
	if (Params->HasField(TEXT("include_content")))
	{
		if (!Params->TryGetBoolField(TEXT("include_content"), bIncludeContent))
		{
			return FMonolithActionResult::Error(TEXT("'include_content' parameter must be a bool"), -32602);
		}
	}
	bool bDetail = false;
	if (Params->HasField(TEXT("detail")))
	{
		if (!Params->HasTypedField<EJson::Boolean>(TEXT("detail")) || !Params->TryGetBoolField(TEXT("detail"), bDetail))
		{
			return FMonolithActionResult::Error(TEXT("'detail' parameter must be a bool"), -32602);
		}
	}
	FString Projection = bDetail ? TEXT("full") : TEXT("compact");
	if (Params->HasField(TEXT("projection")))
	{
		if (!Params->HasTypedField<EJson::String>(TEXT("projection")) || !Params->TryGetStringField(TEXT("projection"), Projection))
		{
			return FMonolithActionResult::Error(TEXT("'projection' parameter must be a string"), -32602);
		}
		Projection = Projection.ToLower();
		if (Projection == TEXT("full"))
		{
			bDetail = true;
		}
		else if (Projection == TEXT("compact"))
		{
			bDetail = false;
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("'projection' parameter must be 'compact' or 'full'"), -32602);
		}
	}
	Projection = bDetail ? TEXT("full") : TEXT("compact");

	FString AssetClassFilter;
	if (Params->HasField(TEXT("asset_class")) &&
		(!Params->HasTypedField<EJson::String>(TEXT("asset_class")) ||
			!Params->TryGetStringField(TEXT("asset_class"), AssetClassFilter)))
	{
		return FMonolithActionResult::Error(TEXT("'asset_class' parameter must be a string"), -32602);
	}
	FString PathFilter;
	if (Params->HasField(TEXT("path_filter")) &&
		(!Params->HasTypedField<EJson::String>(TEXT("path_filter")) ||
			!Params->TryGetStringField(TEXT("path_filter"), PathFilter)))
	{
		return FMonolithActionResult::Error(TEXT("'path_filter' parameter must be a string"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>()
		: nullptr;
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	if (Subsystem->IsIndexing())
	{
		return FMonolithActionResult::Error(TEXT("Indexing is currently in progress"), -32000)
			.WithHint(TEXT("Wait for indexing to complete or use project.get_stats to check progress"));
	}

	FMonolithIndexDatabase* DB = Subsystem->GetDatabase();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Project index database not available"));
	}

	FProjectSearchOptions Options = bIncludeContent
		? FProjectSearchOptions::ContentInclusive()
		: FProjectSearchOptions::AssetNodeOnly();
	Options.Offset = Offset;
	// Q6 (PRD AssetSearchSemanticSearch): optional pushed-down scope filters.
	Options.AssetClassFilter = AssetClassFilter;
	Options.PathFilter = PathFilter;
	// PRD AssetSearchSemanticSearch residual: opt-in per-result RRF score breakdown.
	if (Params->HasField(TEXT("explain")))
	{
		if (!Params->TryGetBoolField(TEXT("explain"), Options.bExplain))
		{
			return FMonolithActionResult::Error(TEXT("'explain' parameter must be a bool"), -32602);
		}
	}
	// PRD AssetSearchSemanticSearch residual: opt-in min-should-match precision gate.
	if (Params->HasField(TEXT("min_should_match_pct")))
	{
		double MsmValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("min_should_match_pct"), MsmValue))
		{
			return FMonolithActionResult::Error(TEXT("'min_should_match_pct' parameter must be a number"), -32602);
		}
		Options.MinShouldMatchPct = FMath::Clamp(static_cast<int32>(MsmValue), 0, 100);
	}
	const int32 QueryLimit = Limit + 1;
	if (static_cast<int64>(Offset) + static_cast<int64>(QueryLimit) > GProjectSearchMaxWindow)
	{
		return FMonolithActionResult::Error(TEXT("'offset' + 'limit' exceeds the project.search max window"), -32602);
	}
	FString SearchError;
	TArray<FSearchResult> SearchResults = Subsystem->Search(Query, QueryLimit, Options, &SearchError);
	if (!SearchError.IsEmpty())
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Project search failed: %s"), *SearchError),
			-32603)
			.WithHint(TEXT("Run project.health to check the index, then use project.repair_fts only when health reports an FTS problem."));
	}
	const bool bTruncated = SearchResults.Num() > Limit;
	if (bTruncated)
	{
		SearchResults.SetNum(Limit);
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	ResultsArr.Reserve(SearchResults.Num());
	for (const FSearchResult& SR : SearchResults)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), SR.AssetPath);
		Entry->SetStringField(TEXT("asset_name"), SR.AssetName);
		Entry->SetStringField(TEXT("asset_class"), SR.AssetClass);
		Entry->SetStringField(TEXT("module_name"), SR.ModuleName);
		Entry->SetStringField(TEXT("match_context"), SR.MatchContext);
		Entry->SetStringField(TEXT("match_source"), SR.MatchSource);
		Entry->SetStringField(TEXT("match_table"), SR.MatchTable);
		Entry->SetStringField(TEXT("match_field"), SR.MatchField);
		Entry->SetStringField(TEXT("match_object_path"), SR.MatchObjectPath);
		int32 MatchValueLength = 0;
		bool bMatchValueTruncated = false;
		Entry->SetStringField(
			TEXT("match_value"),
			CompactProjectSearchMatchValue(
				SR.MatchValue,
				bDetail,
				MatchValueLength,
				bMatchValueTruncated));
		Entry->SetNumberField(TEXT("match_value_length"), MatchValueLength);
		Entry->SetBoolField(TEXT("match_value_truncated"), bMatchValueTruncated);
		Entry->SetNumberField(TEXT("rank"), SR.Rank);
		// score-explain (opt-in): why this result ranked where it did (RRF provenance).
		if (Options.bExplain)
		{
			auto Breakdown = MakeShared<FJsonObject>();
			Breakdown->SetNumberField(TEXT("contributing_hits"), SR.ContributingHits);
			Breakdown->SetNumberField(TEXT("source_kind_count"), SR.ScoreBySource.Num());
			Breakdown->SetNumberField(TEXT("best_rank"), SR.BestRank);
			auto PerSource = MakeShared<FJsonObject>();
			for (const TPair<FString, float>& Pair : SR.ScoreBySource)
			{
				PerSource->SetNumberField(Pair.Key, Pair.Value);
			}
			Breakdown->SetObjectField(TEXT("rrf_contributions"), PerSource);
			Entry->SetObjectField(TEXT("score_breakdown"), Breakdown);
		}
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("contract"), TEXT("search_only_not_write_schema"));
	Result->SetStringField(TEXT("mutation_validation"),
		TEXT("Use describe.schema, describe.action_schema, or the target action schema before mutating any search result."));
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("count"), SearchResults.Num());
	Result->SetNumberField(TEXT("returned_count"), SearchResults.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetBoolField(TEXT("truncated"), bTruncated);
	if (bTruncated)
	{
		Result->SetStringField(TEXT("next_cursor"), FString::FromInt(Offset + Limit));
	}
	Result->SetBoolField(TEXT("include_content"), bIncludeContent);
	Result->SetBoolField(TEXT("detail"), bDetail);
	Result->SetStringField(TEXT("projection"), Projection);
	auto Limits = MakeShared<FJsonObject>();
	Limits->SetNumberField(TEXT("limit"), Limit);
	Limits->SetNumberField(TEXT("offset"), Offset);
	Limits->SetNumberField(TEXT("returned"), SearchResults.Num());
	Limits->SetNumberField(TEXT("max_limit"), GProjectSearchMaxLimit);
	Limits->SetNumberField(TEXT("max_window"), GProjectSearchMaxWindow);
	Limits->SetNumberField(
		TEXT("match_value_preview_chars"),
		MonolithProjectSearchText::PreviewCodePoints);
	Limits->SetBoolField(TEXT("truncated"), bTruncated);
	Limits->SetBoolField(TEXT("include_content"), bIncludeContent);
	Limits->SetBoolField(TEXT("detail"), bDetail);
	Limits->SetStringField(TEXT("projection"), Projection);
	Result->SetObjectField(TEXT("limits"), Limits);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS search query (automatically escaped and tokenized for prefix matching). Alias: q. Search results are not writable schema."), { TEXT("q") })
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return, clamped to 1..1000"), TEXT("50"))
		.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset applied after final fused ranking"), TEXT("0"))
		.Optional(TEXT("cursor"), TEXT("string"), TEXT("Numeric offset cursor returned as next_cursor"), TEXT(""))
		.Optional(TEXT("include_content"), TEXT("bool"), TEXT("Include variable/parameter/DataTable/actor/supplemental matches for discovery only"), TEXT("true"))
		.Optional(TEXT("detail"), TEXT("bool"), TEXT("Return full match_value rows. Default false keeps payloads compact with match_value_truncated metadata."), TEXT("false"))
		.Optional(TEXT("projection"), TEXT("string"), TEXT("Result projection: compact caps match_value previews; full returns complete match_value rows."), TEXT("compact"))
		.Optional(TEXT("asset_class"), TEXT("string"), TEXT("Scope results to this exact asset class (e.g. 'Blueprint', 'WidgetBlueprint'). Empty = any"), TEXT(""))
		.Optional(TEXT("path_filter"), TEXT("string"), TEXT("Scope results to package paths containing this substring (e.g. '/Game/Combat'). Empty = any"), TEXT(""))
		.Optional(TEXT("explain"), TEXT("bool"), TEXT("Attach a per-result score_breakdown (contributing_hits, source_kind_count, best_rank, per-source rrf_contributions) explaining the RRF rank"), TEXT("false"))
		.Optional(TEXT("min_should_match_pct"), TEXT("integer"), TEXT("Precision gate: require at least ceil(token_count*pct/100) of the query tokens to match (exact, via FTS5 K-of-N subset expansion). 0 = off"), TEXT("0"))
		.Build();
}
