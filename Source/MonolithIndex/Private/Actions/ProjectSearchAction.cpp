#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexDatabase.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

// Named (never anonymous) so the forced-full-unity release pass cannot collide
// this constant with a same-named file-local in a sibling translation unit.
namespace MonolithProjectSearchActionDetail
{
	/**
	 * Upper bound on the query string. SQLite's own MATCH grammar is bounded and
	 * the query is bound as a parameter, so this is defence in depth: it caps the
	 * parse work one MCP call can hand the game thread, and it is the standing
	 * mitigation if any query-analysing layer is ever added above the bound MATCH.
	 */
	static constexpr int32 MaxQueryLength = 4096;
}

FMonolithActionResult FProjectSearchAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("query"), Query))
	{
		return FMonolithActionResult::Error(TEXT("'query' must be a string"), -32602);
	}

	Query.TrimStartAndEndInline();
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' must not be empty"), -32602);
	}
	if (Query.Len() > MonolithProjectSearchActionDetail::MaxQueryLength)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("'query' must be %d characters or fewer (got %d)"),
				MonolithProjectSearchActionDetail::MaxQueryLength,
				Query.Len()),
			-32602);
	}

	int32 Limit = 50;
	if (Params->HasField(TEXT("limit")))
	{
		double RawLimit = 0.0;
		if (!Params->TryGetNumberField(TEXT("limit"), RawLimit)
			|| !FMath::IsFinite(RawLimit)
			|| FMath::FloorToDouble(RawLimit) != RawLimit)
		{
			return FMonolithActionResult::Error(TEXT("'limit' must be an integer"), -32602);
		}
		Limit = static_cast<int32>(FMath::Clamp(RawLimit, 1.0, 1000.0));
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
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), TEXT("Indexing is currently in progress"));
		Result->SetNumberField(TEXT("progress"), Subsystem->GetProgress());
		return FMonolithActionResult::Success(Result);
	}

	// Go straight to the database so a caller's bad query stays distinguishable
	// from an index failure; the subsystem's own Search() wrapper cannot report
	// which of the two happened.
	FMonolithIndexDatabase* Database = Subsystem->GetDatabase();
	if (!Database || !Database->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Project index database is not open"));
	}

	TArray<FSearchResult> SearchResults;
	FString SearchError;
	const EMonolithProjectSearchStatus SearchStatus =
		Database->FullTextSearch(Query, Limit, SearchResults, SearchError);
	if (SearchStatus != EMonolithProjectSearchStatus::Succeeded)
	{
		const bool bInvalidQuery = SearchStatus == EMonolithProjectSearchStatus::InvalidQuery;
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("%s: %s"),
				bInvalidQuery ? TEXT("Invalid FTS5 query") : TEXT("Project search failed"),
				*SearchError),
			bInvalidQuery ? -32602 : -32603);
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	for (const FSearchResult& SR : SearchResults)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("asset_path"), SR.AssetPath);
		Entry->SetStringField(TEXT("asset_name"), SR.AssetName);
		Entry->SetStringField(TEXT("asset_class"), SR.AssetClass);
		Entry->SetStringField(TEXT("module_name"), SR.ModuleName);
		Entry->SetStringField(TEXT("match_context"), SR.MatchContext);
		Entry->SetNumberField(TEXT("rank"), SR.Rank);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("count"), SearchResults.Num());
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS5 search query (supports AND, OR, NOT, quoted phrases, prefix*, NEAR(a b, N)); max 4096 characters"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return (clamped to 1-1000)"), TEXT("50"))
		.Build();
}
