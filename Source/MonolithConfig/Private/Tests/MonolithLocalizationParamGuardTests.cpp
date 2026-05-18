#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "MonolithLocalizationActions.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableActionsRegisterTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableActionsRegister", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableActionsRegisterTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

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
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

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
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/ST_DryRunOnly"));
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("create_string_table"), Params);
	TestTrue(TEXT("create_string_table dry_run succeeds without creating an asset"), Result.bSuccess);
	TestTrue(TEXT("dry_run result reports would_create"), Result.Result.IsValid() && Result.Result->GetBoolField(TEXT("would_create")));
	TestTrue(TEXT("dry_run result reports changed=false"), Result.Result.IsValid() && !Result.Result->GetBoolField(TEXT("changed")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardLocalizationStringTableMalformedParamsTest, "Monolith.ParamGuard.MonolithConfig.LocalizationStringTableRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardLocalizationStringTableMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithLocalizationActions::RegisterActions(FMonolithToolRegistry::Get());

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
	}

	{
		const FString CsvPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MonolithTests/empty_string_table_import.csv"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(CsvPath), true);
		TestTrue(TEXT("header-only CSV fixture is written"), FFileHelper::SaveStringToFile(TEXT("key,source_string\n"), *CsvPath));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Localization/MissingTable"));
		Params->SetStringField(TEXT("file_path"), CsvPath);
		Params->SetBoolField(TEXT("replace_existing"), true);
		Params->SetBoolField(TEXT("confirm"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("localization"), TEXT("import_string_table_csv"), Params);
		TestFalse(TEXT("import_string_table_csv rejects destructive empty replace"), Result.bSuccess);
		TestTrue(TEXT("import_string_table_csv reports replace_existing guard"), Result.ErrorMessage.Contains(TEXT("replace_existing")));
	}

	return true;
}
