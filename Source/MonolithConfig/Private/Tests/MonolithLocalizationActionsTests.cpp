#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

namespace MonolithLocalizationActionsTests
{
	const TArray<FString> RequiredActions = {
		TEXT("list_cultures"),
		TEXT("list_string_tables"),
		TEXT("get_string_table"),
		TEXT("validate_string_table")
	};

	void EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("localization"), TEXT("list_cultures")))
		{
			FMonolithLocalizationActions::RegisterActions(Registry);
		}
	}

	FMonolithActionResult Execute(
		const TCHAR* Action,
		const TSharedPtr<FJsonObject>& Params = MakeShared<FJsonObject>())
	{
		EnsureRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(
			TEXT("localization"),
			Action,
			Params);
	}

	bool GetBool(
		const TSharedPtr<FJsonObject>& Json,
		const TCHAR* Field,
		bool DefaultValue = false)
	{
		bool Value = DefaultValue;
		return Json.IsValid() && Json->TryGetBoolField(Field, Value) ? Value : DefaultValue;
	}

	int32 GetInt(
		const TSharedPtr<FJsonObject>& Json,
		const TCHAR* Field,
		int32 DefaultValue = 0)
	{
		double Value = static_cast<double>(DefaultValue);
		return Json.IsValid() && Json->TryGetNumberField(Field, Value)
			? static_cast<int32>(Value)
			: DefaultValue;
	}

	FString GetString(
		const TSharedPtr<FJsonObject>& Json,
		const TCHAR* Field)
	{
		FString Value;
		if (Json.IsValid())
		{
			Json->TryGetStringField(Field, Value);
		}
		return Value;
	}

	const TArray<TSharedPtr<FJsonValue>>* GetArray(
		const TSharedPtr<FJsonObject>& Json,
		const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Json.IsValid() && Json->TryGetArrayField(Field, Values) ? Values : nullptr;
	}

	void SetSourceString(
		const FStringTableRef& StringTable,
		const FString& Key,
		const FString& SourceString)
	{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8 && WITH_EDITORONLY_DATA
		StringTable->SetSourceString(FTextKey(Key), SourceString, FString());
#else
		StringTable->SetSourceString(FTextKey(Key), SourceString);
#endif
	}

	struct FStringTableFixture
	{
		FString RootPath;
		UPackage* PackageA = nullptr;
		UPackage* PackageB = nullptr;
		UStringTable* TableA = nullptr;
		UStringTable* TableB = nullptr;

		FStringTableFixture()
		{
			const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			RootPath = FString::Printf(TEXT("/Game/__MonolithLocalizationTests/%s"), *Suffix);
			PackageA = CreatePackage(*(RootPath + TEXT("/ST_A")));
			PackageB = CreatePackage(*(RootPath + TEXT("/ST_B")));
			TableA = NewObject<UStringTable>(
				PackageA,
				TEXT("ST_A"),
				RF_Public | RF_Standalone);
			TableB = NewObject<UStringTable>(
				PackageB,
				TEXT("ST_B"),
				RF_Public | RF_Standalone);

			const FStringTableRef CoreTable = TableA->GetMutableStringTable();
			SetSourceString(CoreTable, TEXT(" Case"), TEXT("Whitespace key"));
			SetSourceString(CoreTable, TEXT("Case"), TEXT("Primary value"));
			SetSourceString(CoreTable, TEXT("Long"), TEXT("1234567890"));
			SetSourceString(CoreTable, TEXT("case"), FString());
			CoreTable->SetMetaData(FTextKey(TEXT("Case")), TEXT("Context"), TEXT("Menu"));
			CoreTable->SetMetaData(FTextKey(TEXT("Case")), TEXT("Note"), TEXT("LongMetadataValue"));
			CoreTable->SetMetaData(FTextKey(TEXT("Long")), TEXT("Context"), TEXT("Gameplay"));

			FAssetRegistryModule::AssetCreated(TableA);
			FAssetRegistryModule::AssetCreated(TableB);
			PackageA->SetDirtyFlag(false);
			PackageB->SetDirtyFlag(false);
		}

		~FStringTableFixture()
		{
			if (TableA)
			{
				FAssetRegistryModule::AssetDeleted(TableA);
				TableA->ClearFlags(RF_Public | RF_Standalone);
				TableA->MarkAsGarbage();
			}
			if (TableB)
			{
				FAssetRegistryModule::AssetDeleted(TableB);
				TableB->ClearFlags(RF_Public | RF_Standalone);
				TableB->MarkAsGarbage();
			}
			if (PackageA)
			{
				PackageA->SetDirtyFlag(false);
				PackageA->MarkAsGarbage();
			}
			if (PackageB)
			{
				PackageB->SetDirtyFlag(false);
				PackageB->MarkAsGarbage();
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationRegistrationTest,
	"Monolith.Localization.Read.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	MonolithLocalizationActionsTests::EnsureRegistered();
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	bool bPassed = true;
	for (const FString& Action : MonolithLocalizationActionsTests::RequiredActions)
	{
		bPassed &= TestTrue(
			*FString::Printf(TEXT("localization.%s is registered"), *Action),
			Registry.HasAction(TEXT("localization"), Action));
	}
	bPassed &= TestEqual(
		TEXT("The read-only localization namespace owns exactly four actions"),
		Registry.GetActions(TEXT("localization")).Num(),
		4);
	bPassed &= TestFalse(
		TEXT("Mutation actions are not exposed"),
		Registry.HasAction(TEXT("localization"), TEXT("create_string_table")));

	const FMonolithDispatcherAnnotations Annotations =
		Registry.GetDispatcherAnnotations(TEXT("localization"));
	bPassed &= TestTrue(TEXT("localization dispatcher is read-only"), Annotations.bReadOnlyHint);
	bPassed &= TestTrue(TEXT("localization dispatcher is idempotent"), Annotations.bIdempotentHint);

	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("localization")))
	{
		if (Info.Action == TEXT("get_string_table"))
		{
			bPassed &= TestTrue(
				TEXT("StringTable read schema publishes cursor and budgets"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("after_key"))
					&& Info.ParamSchema->HasField(TEXT("entry_limit"))
					&& Info.ParamSchema->HasField(TEXT("metadata_limit")));
		}
		if (Info.Action == TEXT("validate_string_table"))
		{
			bPassed &= TestTrue(
				TEXT("Validation schema publishes scan and issue bounds"),
				Info.ParamSchema.IsValid()
					&& Info.ParamSchema->HasField(TEXT("scan_limit"))
					&& Info.ParamSchema->HasField(TEXT("issue_offset"))
					&& Info.ParamSchema->HasField(TEXT("issue_limit")));
		}
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationParamGuardsTest,
	"Monolith.Localization.Read.ParamGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationParamGuardsTest::RunTest(const FString& /*Parameters*/)
{
	bool bPassed = true;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 0);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("list_cultures"), Params);
		bPassed &= TestFalse(TEXT("Zero culture limit is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Zero culture limit is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Names;
		for (int32 Index = 0; Index < 257; ++Index)
		{
			Names.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("x-%d"), Index)));
		}
		Params->SetArrayField(TEXT("culture_names"), Names);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("list_cultures"), Params);
		bPassed &= TestFalse(TEXT("Oversized culture list is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Oversized culture list is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), TEXT("/Game/"));
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("list_string_tables"), Params);
		bPassed &= TestFalse(TEXT("Non-canonical package prefix is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Package prefix is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI.Wrong"));
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestFalse(TEXT("Mismatched object leaf is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Mismatched object leaf is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetStringField(TEXT("after_key"), FString::ChrN(4097, TEXT('x')));
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestFalse(TEXT("Oversized cursor is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Oversized cursor is invalid params"), Result.ErrorCode, -32602);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Localization/ST_UI"));
		Params->SetNumberField(TEXT("scan_limit"), 0);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestFalse(TEXT("Zero validation scan is rejected"), Result.bSuccess);
		bPassed &= TestEqual(TEXT("Zero validation scan is invalid params"), Result.ErrorCode, -32602);
	}
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLocalizationReadbackTest,
	"Monolith.Localization.Read.ReadbackAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLocalizationReadbackTest::RunTest(const FString& /*Parameters*/)
{
	MonolithLocalizationActionsTests::FStringTableFixture Fixture;
	bool bPassed = true;

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("list_cultures"), Params);
		bPassed &= TestTrue(TEXT("Culture discovery succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("Culture page obeys its limit"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("count")),
			1);
		bPassed &= TestFalse(
			TEXT("Current culture is reported"),
			MonolithLocalizationActionsTests::GetString(Result.Result, TEXT("current_culture")).IsEmpty());
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path"), Fixture.RootPath);
		Params->SetNumberField(TEXT("limit"), 1);
		Params->SetBoolField(TEXT("include_details"), true);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("list_string_tables"), Params);
		bPassed &= TestTrue(TEXT("StringTable discovery succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("Both fixture tables are discovered"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("total")),
			2);
		bPassed &= TestEqual(
			TEXT("Table page obeys its limit"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("count")),
			1);
		bPassed &= TestTrue(
			TEXT("Table page reports another page"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("has_more")));
	}

	FString FirstCursor;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 1);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("Bounded entry read succeeds"), Result.bSuccess);
		bPassed &= TestEqual(
			TEXT("One entry is returned"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("entries_returned")),
			1);
		bPassed &= TestTrue(
			TEXT("First entry page reports more entries"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("has_more_entries")));
		bPassed &= TestFalse(
			TEXT("First entry page cannot claim completeness"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete"), true));
		FirstCursor = MonolithLocalizationActionsTests::GetString(Result.Result, TEXT("next_after_key"));
		bPassed &= TestFalse(TEXT("First entry page returns a cursor"), FirstCursor.IsEmpty());
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetStringField(TEXT("after_key"), FirstCursor);
		Params->SetNumberField(TEXT("entry_limit"), 1);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("Cursor continuation succeeds"), Result.bSuccess);
		const TArray<TSharedPtr<FJsonValue>>* Entries =
			MonolithLocalizationActionsTests::GetArray(Result.Result, TEXT("entries"));
		FString ContinuedKey;
		if (Entries && Entries->Num() == 1 && (*Entries)[0].IsValid())
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if ((*Entries)[0]->TryGetObject(EntryObject) && EntryObject && EntryObject->IsValid())
			{
				ContinuedKey = MonolithLocalizationActionsTests::GetString(*EntryObject, TEXT("key"));
			}
		}
		bPassed &= TestTrue(TEXT("Cursor advances in stable key order"), ContinuedKey > FirstCursor);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 10);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetNumberField(TEXT("metadata_limit"), 1);
		Params->SetNumberField(TEXT("text_limit"), 5);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("Metadata-bounded read succeeds"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("All entries fit in the entry page"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("all_entries_covered")));
		bPassed &= TestFalse(
			TEXT("Shared metadata cutoff is explicit"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("metadata_complete"), true));
		bPassed &= TestFalse(
			TEXT("Metadata cutoff prevents complete=true"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete"), true));
		bPassed &= TestEqual(
			TEXT("Metadata budget is enforced globally"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("returned_metadata_count")),
			1);

		const TArray<TSharedPtr<FJsonValue>>* Entries =
			MonolithLocalizationActionsTests::GetArray(Result.Result, TEXT("entries"));
		bool bFoundTruncatedLongSource = false;
		if (Entries)
		{
			for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
			{
				const TSharedPtr<FJsonObject>* EntryObject = nullptr;
				if (EntryValue.IsValid()
					&& EntryValue->TryGetObject(EntryObject)
					&& EntryObject
					&& EntryObject->IsValid()
					&& MonolithLocalizationActionsTests::GetString(*EntryObject, TEXT("key")) == TEXT("Long"))
				{
					bFoundTruncatedLongSource =
						MonolithLocalizationActionsTests::GetString(*EntryObject, TEXT("source_string")) == TEXT("12345")
						&& MonolithLocalizationActionsTests::GetBool(*EntryObject, TEXT("source_string_truncated"));
				}
			}
		}
		bPassed &= TestTrue(TEXT("Long source strings are explicitly truncated"), bFoundTruncatedLongSource);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("entry_limit"), 10);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("get_string_table"), Params);
		bPassed &= TestTrue(TEXT("Full projection read succeeds"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("A full requested projection is complete"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("scan_limit"), 10);
		Params->SetNumberField(TEXT("issue_limit"), 1);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("StringTable validation succeeds"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("Full validation scan is complete"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestTrue(
			TEXT("Warnings do not invalidate a complete table"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("valid")));
		bPassed &= TestEqual(
			TEXT("No structural errors are reported"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("errors")),
			0);
		bPassed &= TestEqual(
			TEXT("Empty source and edge whitespace are warnings"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("warnings")),
			2);
		bPassed &= TestTrue(
			TEXT("Issue pagination reports remaining issues"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("has_more_issues")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableB->GetPathName());
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("Empty-table validation returns structured output"), Result.bSuccess);
		bPassed &= TestTrue(
			TEXT("Empty-table validation is complete"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete")));
		bPassed &= TestFalse(
			TEXT("An empty table is invalid"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("valid"), true));
		bPassed &= TestEqual(
			TEXT("An empty table reports one error"),
			MonolithLocalizationActionsTests::GetInt(Result.Result, TEXT("errors")),
			1);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Fixture.TableA->GetPathName());
		Params->SetNumberField(TEXT("scan_limit"), 1);
		const FMonolithActionResult Result =
			MonolithLocalizationActionsTests::Execute(TEXT("validate_string_table"), Params);
		bPassed &= TestTrue(TEXT("Bounded validation returns structured output"), Result.bSuccess);
		bPassed &= TestFalse(
			TEXT("Scan cutoff is incomplete"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("complete"), true));
		bPassed &= TestFalse(
			TEXT("Incomplete validation cannot claim valid"),
			MonolithLocalizationActionsTests::GetBool(Result.Result, TEXT("valid"), true));
	}

	bPassed &= TestFalse(TEXT("Primary fixture package remains clean"), Fixture.PackageA->IsDirty());
	bPassed &= TestFalse(TEXT("Secondary fixture package remains clean"), Fixture.PackageB->IsDirty());
	return bPassed;
}

#endif
