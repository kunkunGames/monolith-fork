#include "Actions/ProjectListGameplayTagsAction.h"
#include "MonolithIndexSubsystem.h"
#include "MonolithParamSchema.h"
#include "SQLiteDatabase.h"
#include "Editor.h"

FMonolithActionResult FProjectListGameplayTagsAction::Execute(const TSharedPtr<FJsonObject>& Params)
{
	FString Prefix;
	if (Params->HasField(TEXT("prefix")) && !Params->TryGetStringField(TEXT("prefix"), Prefix))
	{
		return FMonolithActionResult::Error(TEXT("'prefix' parameter must be a string"), -32602);
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
	int32 Offset = 0;
	if (Params->HasField(TEXT("offset")))
	{
		double OffsetValue = 0.0;
		if (!Params->TryGetNumberField(TEXT("offset"), OffsetValue))
		{
			return FMonolithActionResult::Error(TEXT("'offset' parameter must be a number"), -32602);
		}
		Offset = static_cast<int32>(OffsetValue);
	}
	Limit = FMath::Clamp(Limit, 1, 1000);
	Offset = FMath::Max(0, Offset);

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

	FSQLitePreparedStatement Stmt;
	if (Prefix.IsEmpty())
	{
		Stmt.Create(*RawDB, TEXT("SELECT tag_name, parent_tag, reference_count FROM tags ORDER BY tag_name LIMIT ? OFFSET ?;"));
		Stmt.SetBindingValueByIndex(1, static_cast<int64>(Limit));
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Offset));
	}
	else
	{
		// Escape SQL wildcards in user input to prevent broad table scans or malformed matching
		FString EscapedPrefix = Prefix.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("%"), TEXT("\\%")).Replace(TEXT("_"), TEXT("\\_"));
		FString LikePattern = EscapedPrefix + TEXT("%");

		Stmt.Create(*RawDB, TEXT("SELECT tag_name, parent_tag, reference_count FROM tags WHERE tag_name LIKE ? ESCAPE '\\' ORDER BY tag_name LIMIT ? OFFSET ?;"));
		Stmt.SetBindingValueByIndex(1, LikePattern);
		Stmt.SetBindingValueByIndex(2, static_cast<int64>(Limit));
		Stmt.SetBindingValueByIndex(3, static_cast<int64>(Offset));
	}

	TArray<TSharedPtr<FJsonValue>> TagsArr;
	while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		FString TagName, ParentTag;
		int64 RefCount = 0;

		Stmt.GetColumnValueByIndex(0, TagName);
		Stmt.GetColumnValueByIndex(1, ParentTag);
		Stmt.GetColumnValueByIndex(2, RefCount);

		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag_name"), TagName);
		Entry->SetStringField(TEXT("parent_tag"), ParentTag);
		Entry->SetNumberField(TEXT("reference_count"), static_cast<double>(RefCount));
		TagsArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetArrayField(TEXT("tags"), TagsArr);
	Result->SetNumberField(TEXT("count"), TagsArr.Num());
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("offset"), Offset);
	if (!Prefix.IsEmpty())
	{
		Result->SetStringField(TEXT("prefix_filter"), Prefix);
	}
	return FMonolithActionResult::Success(Result);
}

TSharedPtr<FJsonObject> FProjectListGameplayTagsAction::GetSchema()
{
	return FParamSchemaBuilder()
		.Optional(TEXT("prefix"), TEXT("string"), TEXT("Tag prefix filter (e.g. 'Weapon.Melee') -- returns tags starting with this prefix"))
		.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tags to return"), TEXT("100"))
		.Optional(TEXT("offset"), TEXT("integer"), TEXT("Pagination offset"), TEXT("0"))
		.Build();
}
