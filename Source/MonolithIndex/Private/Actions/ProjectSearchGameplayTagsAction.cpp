#include "Actions/ProjectSearchGameplayTagsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "SQLiteDatabase.h"
#include "Editor.h"

FMonolithActionResult FProjectSearchGameplayTagsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	if (!Params->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("'query' parameter is required"), -32602);
	}

	int32 Limit = 100;
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
		"LIMIT ?;"
	));

	FString EscapedQuery = Query.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
	FString LikePattern = TEXT("%") + EscapedQuery + TEXT("%");
	Stmt.SetBindingValueByIndex(1, LikePattern);
	Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));

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
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectSearchGameplayTagsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Required(TEXT("query"), TEXT("string"), TEXT("Substring to search for in tag names (e.g. 'Damage', 'Weapon')"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tags to return"), TEXT("100"))
		.Build();
}
