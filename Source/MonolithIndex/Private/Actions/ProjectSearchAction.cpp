#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "ProjectSearchQueryProjection.h"
#include "ProjectSearchTextProjection.h"
#include "Editor.h"

FMonolithActionResult FProjectSearchAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("query"), Query))
	{
		return FMonolithActionResult::Error(TEXT("'query' must be a string"), -32602);
	}
	MonolithProjectSearchQuery::TrimSyntaxWhitespaceInline(Query);
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' must not be empty"), -32602);
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

	TArray<FSearchResult> SearchResults;
	FString SearchError;
	const EMonolithProjectSearchStatus SearchStatus =
		Subsystem->Search(Query, Limit, SearchResults, SearchError);
	if (SearchStatus != EMonolithProjectSearchStatus::Succeeded)
	{
		const int32 ErrorCode =
			SearchStatus == EMonolithProjectSearchStatus::InvalidQuery
				? -32602
				: -32603;
		const TCHAR* ErrorKind =
			SearchStatus == EMonolithProjectSearchStatus::InvalidQuery
				? TEXT("Invalid FTS5 query")
				: TEXT("Project search failed");
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("%s: %s"), ErrorKind, *SearchError),
			ErrorCode);
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
		Entry->SetNumberField(TEXT("match_context_length"), SR.MatchContextLength);
		Entry->SetBoolField(TEXT("match_context_truncated"), SR.bMatchContextTruncated);
		Entry->SetStringField(TEXT("match_source"), SR.MatchSource);
		Entry->SetStringField(TEXT("match_table"), SR.MatchTable);
		Entry->SetStringField(TEXT("match_field"), SR.MatchField);
		Entry->SetStringField(TEXT("match_object_path"), SR.MatchObjectPath);
		Entry->SetStringField(TEXT("match_value"), SR.MatchValue);
		Entry->SetNumberField(TEXT("match_value_length"), SR.MatchValueLength);
		Entry->SetBoolField(TEXT("match_value_truncated"), SR.bMatchValueTruncated);
		Entry->SetNumberField(TEXT("rank"), SR.Rank);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("count"), SearchResults.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(
		TEXT("match_value_preview_chars"),
		MonolithProjectSearchText::PreviewCodePoints);
	Result->SetNumberField(
		TEXT("match_context_preview_chars"),
		MonolithProjectSearchText::PreviewCodePoints);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(
			TEXT("query"),
			TEXT("string"),
			TEXT("FTS5 query; supports AND, OR, NOT, quoted phrases, token-prefix *, and asset/node column filters"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results (clamped to 1-1000)"), TEXT("50"))
		.Build();
}
