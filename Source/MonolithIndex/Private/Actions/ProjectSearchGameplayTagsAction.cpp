#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "SQLiteDatabase.h"
#include "Editor.h"

FMonolithActionResult FProjectSearchGameplayTagsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (Params->HasField(TEXT("query")))
	{
		if (!Params->HasTypedField(TEXT("query"), EJson::String) || !Params->TryGetStringField(TEXT("query"), Query))
		{
			return FMonolithActionResult::Error(TEXT("'query' parameter must be a string"), -32602);
		}
	}
	if (Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required and cannot be empty"), -32602);
	}

	int32 Limit = 100;
	if (Params->HasField(TEXT("limit")))
	{
		double LimitValue = 0.0;
		if (!Params->HasTypedField(TEXT("limit"), EJson::Number) || !Params->TryGetNumberField(TEXT("limit"), LimitValue))
		{
			return FMonolithActionResult::Error(TEXT("'limit' parameter must be a number"), -32602);
		}
		Limit = static_cast<int32>(LimitValue);
	}
	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		double OffsetValue = 0.0;
		if (!Params->HasTypedField(TEXT("offset"), EJson::Number) || !Params->TryGetNumberField(TEXT("offset"), OffsetValue))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be a number"), -32602);
		}
		if (OffsetValue < 0.0 || OffsetValue > 100000.0)
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be within 0..100000"), -32602);
		}
		Offset = static_cast<int32>(OffsetValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);
	if (static_cast<int64>(Offset) + static_cast<int64>(Limit) > 100000)
	{
		return FMonolithActionResult::Error(TEXT("'offset' + 'limit' exceeds the max window of 100000"), -32602);
	}

	UMonolithIndexSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMonolithIndexSubsystem>();
	if (!Subsystem)
	{
		return FMonolithActionResult::Error(TEXT("Index subsystem not available"));
	}

	FMonolithIndexDatabase* DB = Subsystem->GetDatabase();
	if (!DB || !DB->IsOpen())
	{
		return FMonolithActionResult::Error(TEXT("Project index database not available"));
	}

	FSQLiteDatabase* RawDB = DB->GetRawDatabase();
	if (!RawDB)
	{
		return FMonolithActionResult::Error(TEXT("Raw database handle not available"));
	}

	// Query tags with aggregated referencing asset paths
	FSQLitePreparedStatement Stmt;
	Stmt.Create(*RawDB, TEXT(
		"SELECT t.id, t.tag_name, t.parent_tag, t.reference_count, "
		"GROUP_CONCAT(a.package_path) as referencing_assets "
		"FROM tags t "
		"LEFT JOIN tag_references tr ON t.id = tr.tag_id "
		"LEFT JOIN assets a ON tr.asset_id = a.id "
		"WHERE t.tag_name LIKE ? ESCAPE '\\' "
		"GROUP BY t.id "
		"ORDER BY t.reference_count DESC, t.tag_name "
		"LIMIT ? OFFSET ?;"
	));

	FString EscapedQuery = Query.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
	FString LikePattern = TEXT("%") + EscapedQuery + TEXT("%");
	Stmt.SetBindingValueByIndex(1, LikePattern);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
	Stmt.SetBindingValueByIndex(3, static_cast<int64>(Offset));

	TArray<TSharedPtr<FJsonValue>> TagsArr;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		int64 TagId = 0;
		FString TagName, ParentTag, ReferencingAssetsRaw;
		int64 RefCount = 0;

		Stmt.GetColumnValueByIndex(0, TagId);
		Stmt.GetColumnValueByIndex(1, TagName);
		Stmt.GetColumnValueByIndex(2, ParentTag);
		Stmt.GetColumnValueByIndex(3, RefCount);
		Stmt.GetColumnValueByIndex(4, ReferencingAssetsRaw);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag_name"), TagName);
		Entry->SetStringField(TEXT("parent_tag"), ParentTag);
		Entry->SetNumberField(TEXT("reference_count"), static_cast<double>(RefCount));

		// Parse comma-separated asset paths into array
		TArray<TSharedPtr<FJsonValue>> AssetsArr;
		if (!ReferencingAssetsRaw.IsEmpty())
		{
			TArray<FString> AssetPaths;
			ReferencingAssetsRaw.ParseIntoArray(AssetPaths, TEXT(","));
			for (const FString& Path : AssetPaths)
			{
				FString Trimmed = Path.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					AssetsArr.Add(MakeShared<FJsonValueString>(Trimmed));
				}
			}
		}
		Entry->SetArrayField(TEXT("referencing_assets"), AssetsArr);

		TagsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("tags"), TagsArr);
	Result->SetNumberField(TEXT("count"), TagsArr.Num());
	Result->SetStringField(TEXT("query"), Query);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("offset"), Offset);
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchGameplayTagsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("Substring to search for in tag names (e.g. 'Damage', 'Weapon'). Alias: q."), { TEXT("q") })
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tags to return"), TEXT("100"))
		.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset"), TEXT("0"))
		.Build();
}
