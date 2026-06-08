#include "Actions/ProjectSearchAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "Editor.h"

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
		if (!Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);

	bool bIncludeContent = true;
	if (Params->HasField(TEXT("include_content")))
	{
		if (!Params->TryGetBoolField(TEXT("include_content"), bIncludeContent))
		{
			return FMonolithActionResult::Error(TEXT("'include_content' parameter must be a bool"), -32602);
		}
	}

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
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

	const FProjectSearchOptions Options = bIncludeContent
		? FProjectSearchOptions::ContentInclusive()
		: FProjectSearchOptions::AssetNodeOnly();
	TArray<FSearchResult> SearchResults = Subsystem->Search(Query, Limit, Options);

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
		Entry->SetStringField(TEXT("match_value"), SR.MatchValue);
		Entry->SetNumberField(TEXT("rank"), SR.Rank);
		ResultsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("contract"), TEXT("search_only_not_write_schema"));
	Result->SetStringField(TEXT("mutation_validation"),
		TEXT("Use describe.schema, describe.action_schema, or the target action schema before mutating any search result."));
	Result->SetArrayField(TEXT("results"), ResultsArr);
	Result->SetNumberField(TEXT("count"), SearchResults.Num());
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS search query (automatically escaped and tokenized for prefix matching). Search results are not writable schema."))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return"), TEXT("50"))
		.Optional(TEXT("include_content"), TEXT("bool"), TEXT("Include variable/parameter/DataTable/actor/supplemental matches for discovery only"), TEXT("true"))
		.Build();
}
