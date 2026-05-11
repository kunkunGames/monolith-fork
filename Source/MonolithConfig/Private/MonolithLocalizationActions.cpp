#include "MonolithLocalizationActions.h"

#include "MonolithAssetUtils.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/ObjectLibrary.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

namespace
{
	int32 GetClampedLimit(const TSharedPtr<FJsonObject>& Params, int32 DefaultValue)
	{
		double LimitNumber = static_cast<double>(DefaultValue);
		if (Params.IsValid())
		{
			Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		}
		return FMath::Clamp(static_cast<int32>(LimitNumber), 1, 1000);
	}

	FString GetPathParam(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path = TEXT("/Game");
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("path"), Path);
		}
		if (Path.IsEmpty())
		{
			Path = TEXT("/Game");
		}
		return Path;
	}

	bool IsProjectContentPath(const FString& Path)
	{
		return Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"));
	}

	bool ResolveProjectContentPath(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FString& OutPath, FString& OutError)
	{
		FString Path = GetPathParam(Params);
		if (FieldName && Params.IsValid())
		{
			Params->TryGetStringField(FieldName, Path);
		}

		OutPath = FMonolithAssetUtils::ResolveAssetPath(Path);
		if (!IsProjectContentPath(OutPath))
		{
			OutError = FString::Printf(TEXT("Path '%s' must resolve under /Game"), *Path);
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> CultureToJson(const FCultureRef& Culture)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("name"), Culture->GetName());
		Obj->SetStringField(TEXT("native_name"), Culture->GetNativeName());
		Obj->SetStringField(TEXT("english_name"), Culture->GetEnglishName());
		Obj->SetStringField(TEXT("display_name"), Culture->GetDisplayName());
		Obj->SetStringField(TEXT("two_letter_iso"), Culture->GetTwoLetterISOLanguageName());
		Obj->SetStringField(TEXT("three_letter_iso"), Culture->GetThreeLetterISOLanguageName());
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> StringTableEntriesToJson(const UStringTable* Table, int32 Limit, bool bIncludeMetadata, int32& OutTotalCount)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		OutTotalCount = 0;
		if (!Table)
		{
			return Rows;
		}

		const FStringTableConstRef StringTable = Table->GetStringTable();
		StringTable->EnumerateKeysAndSourceStrings(
			[&Rows, &OutTotalCount, Limit, bIncludeMetadata, StringTable](const FTextKey& Key, const FString& SourceString)
			{
				++OutTotalCount;
				if (Rows.Num() >= Limit)
				{
					return true;
				}

				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("key"), Key.ToString());
				Row->SetStringField(TEXT("source_string"), SourceString);
				Row->SetNumberField(TEXT("source_length"), SourceString.Len());

				if (bIncludeMetadata)
				{
					TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
					StringTable->EnumerateMetaData(Key,
						[&Metadata](FName MetadataId, const FString& MetadataValue)
						{
							Metadata->SetStringField(MetadataId.ToString(), MetadataValue);
							return true;
						});
					Row->SetObjectField(TEXT("metadata"), Metadata);
				}

				Rows.Add(MakeShared<FJsonValueObject>(Row));
				return true;
			});

		return Rows;
	}

	TSharedPtr<FJsonObject> StringTableSummaryToJson(UStringTable* Table, bool bIncludeEntries, int32 Limit, bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Table)
		{
			Obj->SetBoolField(TEXT("loaded"), false);
			return Obj;
		}

		Obj->SetBoolField(TEXT("loaded"), true);
		Obj->SetStringField(TEXT("asset_path"), Table->GetPathName());
		Obj->SetStringField(TEXT("package_path"), Table->GetOutermost() ? Table->GetOutermost()->GetName() : FString());
		Obj->SetStringField(TEXT("name"), Table->GetName());
		Obj->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
		Obj->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());

		int32 EntryCount = 0;
		TArray<TSharedPtr<FJsonValue>> Entries = StringTableEntriesToJson(Table, bIncludeEntries ? Limit : 1, bIncludeMetadata, EntryCount);
		Obj->SetNumberField(TEXT("entry_count"), EntryCount);
		if (bIncludeEntries)
		{
			Obj->SetArrayField(TEXT("entries"), Entries);
			Obj->SetNumberField(TEXT("returned_count"), Entries.Num());
			if (EntryCount > Entries.Num())
			{
				Obj->SetNumberField(TEXT("truncated_remaining"), EntryCount - Entries.Num());
			}
		}
		return Obj;
	}

	TArray<UStringTable*> LoadStringTablesUnderPath(const FString& Path)
	{
		TArray<UStringTable*> Tables;
		UObjectLibrary* Library = UObjectLibrary::CreateLibrary(UStringTable::StaticClass(), false, true);
		if (!Library)
		{
			return Tables;
		}

		Library->LoadAssetsFromPath(Path);
		TArray<UObject*> Objects;
		Library->GetObjects(Objects);
		for (UObject* Object : Objects)
		{
			if (UStringTable* Table = Cast<UStringTable>(Object))
			{
				Tables.Add(Table);
			}
		}

		Tables.Sort([](const UStringTable& A, const UStringTable& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		return Tables;
	}

	UStringTable* LoadStringTableFromParams(const TSharedPtr<FJsonObject>& Params, FString& OutAssetPath, FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset_path"), OutAssetPath) || OutAssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required param 'asset_path'");
			return nullptr;
		}

		OutAssetPath = FMonolithAssetUtils::ResolveAssetPath(OutAssetPath);
		if (!IsProjectContentPath(OutAssetPath))
		{
			OutError = FString::Printf(TEXT("StringTable asset path '%s' must resolve under /Game"), *OutAssetPath);
			return nullptr;
		}

		UObject* Asset = FMonolithAssetUtils::LoadAssetByPath(OutAssetPath);
		UStringTable* Table = Cast<UStringTable>(Asset);
		if (!Table)
		{
			OutError = FString::Printf(TEXT("StringTable asset not found at '%s'"), *OutAssetPath);
		}
		return Table;
	}
}

void FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("localization"), TEXT("list_cultures"),
		TEXT("List available cultures known to Unreal internationalization."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListCultures),
		FParamSchemaBuilder()
			.Optional(TEXT("culture_names"), TEXT("array"), TEXT("Optional culture names to resolve; omitted returns configured/default culture context"))
			.Optional(TEXT("include_derived"), TEXT("boolean"), TEXT("Include derived cultures when resolving culture_names"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("list_string_tables"),
		TEXT("List StringTable assets under a project content path."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ListStringTables),
		FParamSchemaBuilder()
			.Optional(TEXT("path"), TEXT("string"), TEXT("Content path to scan"), TEXT("/Game"))
			.Optional(TEXT("include_entries"), TEXT("boolean"), TEXT("Include capped entry rows"), TEXT("false"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata when entries are included"), TEXT("false"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum tables or entries to return"), TEXT("100"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("get_string_table"),
		TEXT("Inspect a StringTable asset and return capped entries."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::GetStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Optional(TEXT("include_metadata"), TEXT("boolean"), TEXT("Include per-entry metadata"), TEXT("true"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum entries to return"), TEXT("200"))
			.Build());

	Registry.RegisterAction(TEXT("localization"), TEXT("validate_string_table"),
		TEXT("Validate a StringTable asset for empty keys, empty strings, duplicate-looking keys, and large output warnings."),
		FMonolithActionHandler::CreateStatic(&FMonolithLocalizationActions::ValidateStringTable),
		FParamSchemaBuilder()
			.Required(TEXT("asset_path"), TEXT("string"), TEXT("StringTable asset path"))
			.Build());
}

FMonolithActionResult FMonolithLocalizationActions::ListCultures(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FString> CultureNames;
	bool bIncludeDerived = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_derived"), bIncludeDerived);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Params->TryGetArrayField(TEXT("culture_names"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name) && !Name.IsEmpty())
				{
					CultureNames.Add(Name);
				}
			}
		}
	}

	TArray<FCultureRef> Cultures;
	if (CultureNames.Num() > 0)
	{
		Cultures = FInternationalization::Get().GetAvailableCultures(CultureNames, bIncludeDerived);
	}
	else
	{
		const FCultureRef Current = FInternationalization::Get().GetCurrentCulture();
		Cultures.Add(Current);
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FCultureRef& Culture : Cultures)
	{
		Rows.Add(MakeShared<FJsonValueObject>(CultureToJson(Culture)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("current_culture"), FInternationalization::Get().GetCurrentCulture()->GetName());
	Result->SetStringField(TEXT("current_language"), FInternationalization::Get().GetCurrentLanguage()->GetName());
	Result->SetNumberField(TEXT("count"), Rows.Num());
	Result->SetArrayField(TEXT("cultures"), Rows);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ListStringTables(const TSharedPtr<FJsonObject>& Params)
{
	FString Path, Error;
	if (!ResolveProjectContentPath(Params, TEXT("path"), Path, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const int32 Limit = GetClampedLimit(Params, 100);
	bool bIncludeEntries = false;
	bool bIncludeMetadata = false;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("include_entries"), bIncludeEntries);
		Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	}

	TArray<UStringTable*> Tables = LoadStringTablesUnderPath(Path);
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (UStringTable* Table : Tables)
	{
		if (Rows.Num() >= Limit)
		{
			break;
		}
		Rows.Add(MakeShared<FJsonValueObject>(StringTableSummaryToJson(Table, bIncludeEntries, Limit, bIncludeMetadata)));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(TEXT("matched_count"), Tables.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetArrayField(TEXT("string_tables"), Rows);
	if (Tables.Num() > Rows.Num())
	{
		Result->SetNumberField(TEXT("truncated_remaining"), Tables.Num() - Rows.Num());
	}
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::GetStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeMetadata = true;
	Params->TryGetBoolField(TEXT("include_metadata"), bIncludeMetadata);
	const int32 Limit = GetClampedLimit(Params, 200);

	TSharedPtr<FJsonObject> Result = StringTableSummaryToJson(Table, true, Limit, bIncludeMetadata);
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithLocalizationActions::ValidateStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, Error;
	UStringTable* Table = LoadStringTableFromParams(Params, AssetPath, Error);
	if (!Table)
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	auto AddIssue = [&Issues](const FString& Code, const FString& Message, const FString& Key = FString())
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Key.IsEmpty())
		{
			Issue->SetStringField(TEXT("key"), Key);
		}
		Issues.Add(MakeShared<FJsonValueObject>(Issue));
	};

	TSet<FString> LowerKeys;
	int32 EntryCount = 0;
	Table->GetStringTable()->EnumerateKeysAndSourceStrings(
		[&EntryCount, &LowerKeys, &AddIssue](const FTextKey& Key, const FString& SourceString)
		{
			++EntryCount;
			const FString KeyString = Key.ToString();
			if (KeyString.TrimStartAndEnd().IsEmpty())
			{
				AddIssue(TEXT("empty_key"), TEXT("StringTable entry has an empty key."));
			}
			if (SourceString.IsEmpty())
			{
				AddIssue(TEXT("empty_source_string"), TEXT("StringTable entry has an empty source string."), KeyString);
			}

			const FString Lower = KeyString.ToLower();
			if (LowerKeys.Contains(Lower))
			{
				AddIssue(TEXT("case_insensitive_duplicate_key"), TEXT("Another key differs only by case."), KeyString);
			}
			LowerKeys.Add(Lower);
			return true;
		});

	if (EntryCount == 0)
	{
		AddIssue(TEXT("empty_table"), TEXT("StringTable has no entries."));
	}
	if (EntryCount > 1000)
	{
		AddIssue(TEXT("large_table"), TEXT("StringTable has more than 1000 entries; use capped reads for agent workflows."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AssetPath);
	Result->SetStringField(TEXT("table_id"), Table->GetStringTableId().ToString());
	Result->SetStringField(TEXT("namespace"), Table->GetStringTable()->GetNamespace());
	Result->SetNumberField(TEXT("entry_count"), EntryCount);
	Result->SetArrayField(TEXT("issues"), Issues);
	Result->SetNumberField(TEXT("issue_count"), Issues.Num());
	Result->SetBoolField(TEXT("valid"), Issues.Num() == 0);
	Result->SetBoolField(TEXT("read_only"), true);
	return FMonolithActionResult::Success(Result);
}
