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

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
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

	TArray<FSearchResult> SearchResults = Subsystem->Search(Query, Limit);

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
		.Required(TEXT("query"), TEXT("string"), TEXT("FTS5 search query (supports AND, OR, NOT, prefix*)"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum results to return"), TEXT("50"))
		.Build();
}
