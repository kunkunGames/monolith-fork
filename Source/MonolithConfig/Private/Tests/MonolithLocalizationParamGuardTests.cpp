#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "Internationalization/StringTableRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MonolithAssetUtils.h"
#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

namespace
{
	void EnsureLocalizationActionsRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("localization"), TEXT("list_cultures")))
		{
			FMonolithLocalizationActions::RegisterActions(Registry);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableActionsRegisterTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableActionsRegister", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableActionsRegisterTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	TestTrue(TEXT("list_cultures action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("list_cultures")));
	TestTrue(TEXT("list_string_tables action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("list_string_tables")));
	TestTrue(TEXT("get_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("get_string_table")));
	TestTrue(TEXT("validate_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("validate_string_table")));
	TestTrue(TEXT("create_string_table action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("create_string_table")));
	TestTrue(TEXT("set_string_entry action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_entry")));
	TestTrue(TEXT("remove_string_entry action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("remove_string_entry")));
	TestTrue(TEXT("set_string_metadata action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_metadata")));
	TestTrue(TEXT("import_string_table_csv action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("import_string_table_csv")));
	TestTrue(TEXT("export_string_table_csv action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("export_string_table_csv")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableWriteGateTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableWriteGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableWriteGateTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_WriteGate"));

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
	TestFalse(TEXT("create_string_table rejects mutation without dry_run or confirm"), Result.bSuccess);
	TestTrue(TEXT("write gate error mentions dry_run or confirm"), Result.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableCreateDryRunTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableCreateDryRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableCreateDryRunTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_DryRunOnly"));
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
	TestTrue(TEXT("create_string_table dry_run succeeds without creating an asset"), Result.bSuccess);
	bool bWouldCreate = false;
	bool bChanged = true; // default to true so failure to parse doesn't artificially pass the !changed check
	if (Result.Result.IsValid())
	{
		Result.Result->TryGetBoolField(TEXT("would_create"), bWouldCreate);
		Result.Result->TryGetBoolField(TEXT("changed"), bChanged);
	}
	TestTrue(TEXT("dry_run result reports would_create"), bWouldCreate);
	TestTrue(TEXT("dry_run result reports changed=false"), !bChanged);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableMalformedParamsTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableMalformedParamsTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("source_string"), TEXT("Hello"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestFalse(TEXT("set_string_entry rejects missing key before asset load"), Result.bSuccess);
		TestTrue(TEXT("set_string_entry reports missing key"), Result.ErrorMessage.Contains(TEXT("key")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), TEXT("D:/MonolithOutsideExport.csv"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), Params);
		TestFalse(TEXT("export_string_table_csv rejects filesystem paths outside the project"), Result.bSuccess);
		TestTrue(TEXT("export_string_table_csv reports project directory scope"), Result.ErrorMessage.Contains(TEXT("project directory")));
		TestEqual(TEXT("outside path is an invalid-params error"), Result.ErrorCode, -32602);
	}

	{
		const FString CsvRelativePath = TEXT("Saved/MonolithTests/empty_string_table_import.csv");
		const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(TEXT("header-only CSV fixture is written"), FFileHelper::SaveStringToFile(TEXT("key,source_string\n"), *CsvPath));
		ON_SCOPE_EXIT
		{
			IFileManager::Get().Delete(*CsvPath, false, true);
		};

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects destructive empty replace"), Result.bSuccess);
		TestTrue(TEXT("import_string_table_csv reports replace_existing guard"), Result.ErrorMessage.Contains(TEXT("replace_existing")));
		TestEqual(TEXT("empty destructive import is an invalid-params error"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStrictJsonTypesTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStrictJsonTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStrictJsonTypesTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	auto ExpectInvalidParams = [this](const FString& Label, const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), Action, Params);
		TestFalse(*FString::Printf(TEXT("%s is rejected"), *Label), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("%s returns -32602"), *Label), Result.ErrorCode, -32602);
	};

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("20"));
		ExpectInvalidParams(TEXT("string limit"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1.5);
		ExpectInvalidParams(TEXT("fractional integer limit"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("include_entries"), TEXT("false"));
		ExpectInvalidParams(TEXT("string include_entries"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("include_derived"), TEXT("false"));
		ExpectInvalidParams(TEXT("string include_derived"), TEXT("list_cultures"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CultureNames;
		CultureNames.Add(MakeShared<FJsonValueBoolean>(true));
		Params->SetArrayField(TEXT("culture_names"), CultureNames);
		ExpectInvalidParams(TEXT("non-string culture_names element"), TEXT("list_cultures"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
		ExpectInvalidParams(TEXT("null path"), TEXT("list_string_tables"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("asset_path"), 17.0);
		Params->SetBoolField(TEXT("dry_run"), true);
		ExpectInvalidParams(TEXT("numeric asset_path"), TEXT("create_string_table"), Params);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_StrictType"));
		Params->SetStringField(TEXT("dry_run"), TEXT("true"));
		ExpectInvalidParams(TEXT("string dry_run"), TEXT("create_string_table"), Params);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableLifecycleTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableLifecycleTest::RunTest(const FString& Parameters)
{
	EnsureLocalizationActionsRegistered();

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FString::Printf(TEXT("ST_ActionLifecycle_%s"), *Suffix);
	const FString AssetPath = FString::Printf(TEXT("/Game/Tests/Monolith/Localization/%s"), *AssetName);
	const FString CsvRelativePath = FString::Printf(TEXT("Saved/MonolithTests/Localization/%s.csv"), *AssetName);
	const FString CsvPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), CsvRelativePath));

	UStringTable* CreatedTable = nullptr;
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*CsvPath, false, true);
		if (CreatedTable)
		{
			FStringTableRegistry::Get().UnregisterStringTable(CreatedTable->GetStringTableId());
			if (UPackage* Package = CreatedTable->GetOutermost())
			{
				Package->SetDirtyFlag(false);
			}
			CreatedTable->ClearFlags(RF_Public | RF_Standalone);
			CreatedTable->MarkAsGarbage();
		}
	};

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("namespace"), TEXT("Monolith.Automation"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
		if (!TestTrue(TEXT("create_string_table creates an in-memory fixture"), Result.bSuccess))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
	}

	CreatedTable = Cast<UStringTable>(FMonolithAssetUtils::LoadAssetByPath(AssetPath));
	if (!TestNotNull(TEXT("created StringTable resolves by asset path"), CreatedTable))
	{
		return false;
	}
	TestEqual(TEXT("created StringTable namespace is applied"), CreatedTable->GetStringTable()->GetNamespace(), FString(TEXT("Monolith.Automation")));

	const FString EntryKey = TEXT("MainMenu.Play");
	{
		TSharedPtr<FJsonObject> BadMetadata = MakeShared<FJsonObject>();
		BadMetadata->SetBoolField(TEXT("Context"), true);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("source_string"), TEXT("MustNotWrite"));
		Params->SetObjectField(TEXT("metadata"), BadMetadata);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestFalse(TEXT("set_string_entry rejects non-string metadata value"), Result.bSuccess);
		TestEqual(TEXT("non-string metadata is an invalid-params error"), Result.ErrorCode, -32602);

		FString UnexpectedSource;
		TestFalse(TEXT("failed metadata validation does not create the entry"),
			CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), UnexpectedSource));
	}

	{
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(TEXT("Context"), TEXT("Main menu button"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("source_string"), TEXT("Play"));
		Params->SetObjectField(TEXT("metadata"), Metadata);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_entry"), Params);
		TestTrue(TEXT("set_string_entry writes source and metadata"), Result.bSuccess);
	}

	FString SourceString;
	TestTrue(TEXT("written entry exists"), CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), SourceString));
	TestEqual(TEXT("written source string round-trips"), SourceString, FString(TEXT("Play")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetStringField(TEXT("metadata_key"), TEXT("Owner"));
		Params->SetStringField(TEXT("metadata_value"), TEXT("UI"));
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("set_string_metadata"), Params);
		TestTrue(TEXT("set_string_metadata writes one metadata field"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("validate_string_table"), Params);
		TestTrue(TEXT("validate_string_table executes"), Result.bSuccess);
		bool bValid = false;
		if (Result.Result.IsValid())
		{
			Result.Result->TryGetBoolField(TEXT("valid"), bValid);
		}
		TestTrue(TEXT("populated StringTable validates cleanly"), bValid);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("export_string_table_csv"), Params);
		if (!TestTrue(TEXT("export_string_table_csv writes a project-scoped CSV"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("export_string_table_csv error: %s"), *Result.ErrorMessage));
		}
		TestTrue(TEXT("exported CSV exists"), IFileManager::Get().FileExists(*CsvPath));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("remove_string_entry"), Params);
		TestTrue(TEXT("remove_string_entry removes the exported entry"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), CsvRelativePath);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		if (!TestTrue(TEXT("import_string_table_csv restores the exported entry"), Result.bSuccess))
		{
			AddError(FString::Printf(TEXT("import_string_table_csv error: %s"), *Result.ErrorMessage));
		}
	}

	SourceString.Reset();
	TestTrue(TEXT("imported entry exists"), CreatedTable->GetStringTable()->GetSourceString(FTextKey(EntryKey), SourceString));
	TestEqual(TEXT("imported source string round-trips"), SourceString, FString(TEXT("Play")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetBoolField(TEXT("include_metadata"), true);
		Params->SetNumberField(TEXT("limit"), 20);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("get_string_table"), Params);
		TestTrue(TEXT("get_string_table reads the imported fixture"), Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("key"), EntryKey);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("remove_string_entry"), Params);
		TestTrue(TEXT("final remove_string_entry succeeds"), Result.bSuccess);
	}

	return true;
}
