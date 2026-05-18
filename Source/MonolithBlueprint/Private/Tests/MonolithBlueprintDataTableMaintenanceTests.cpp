#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Misc/AutomationTest.h"
#include "MonolithBlueprintDataTableTestRows.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

namespace
{
	UDataTable* GetOrCreateDataTableMaintenanceTestAsset()
	{
		const FString PackageName = TEXT("/Game/Tests/Monolith/Blueprint/DT_DataTableMaintenanceGuard");
		UPackage* Package = CreatePackage(*PackageName);
		UDataTable* DataTable = FindObject<UDataTable>(Package, TEXT("DT_DataTableMaintenanceGuard"));
		if (!DataTable)
		{
			DataTable = NewObject<UDataTable>(Package, TEXT("DT_DataTableMaintenanceGuard"), RF_Public | RF_Standalone);
		}
		DataTable->RowStruct = FTableRowBase::StaticStruct();
		if (!DataTable->GetRowMap().Contains(TEXT("RowA")))
		{
			FTableRowBase Row;
			DataTable->AddRow(TEXT("RowA"), Row);
		}
		return DataTable;
	}

	UDataTable* GetOrCreateTypedDataTableMaintenanceTestAsset()
	{
		const FString PackageName = TEXT("/Game/Tests/Monolith/Blueprint/DT_DataTableMaintenanceTypedGuard");
		UPackage* Package = CreatePackage(*PackageName);
		UDataTable* DataTable = FindObject<UDataTable>(Package, TEXT("DT_DataTableMaintenanceTypedGuard"));
		if (!DataTable)
		{
			DataTable = NewObject<UDataTable>(Package, TEXT("DT_DataTableMaintenanceTypedGuard"), RF_Public | RF_Standalone);
		}
		DataTable->RowStruct = FMonolithBlueprintDataTableTypedTestRow::StaticStruct();
		return DataTable;
	}

	bool JsonStringArrayContains(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, const FString& Expected)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid() && Value->AsString() == Expected)
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintDataTableMaintenanceRegistersTest, "Monolith.ParamGuard.Blueprint.DataTableMaintenanceRegisters", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintDataTableMaintenanceRegistersTest::RunTest(const FString& Parameters)
{
	FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry::Get());

	TestTrue(TEXT("get_data_table_schema action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("get_data_table_schema")));
	TestTrue(TEXT("update_data_table_row action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("update_data_table_row")));
	TestTrue(TEXT("remove_data_table_row action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("remove_data_table_row")));
	TestTrue(TEXT("export_data_table_csv action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("export_data_table_csv")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintDataTableMaintenanceWriteGateTest, "Monolith.ParamGuard.Blueprint.DataTableMaintenanceWriteGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintDataTableMaintenanceWriteGateTest::RunTest(const FString& Parameters)
{
	FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/DT_Missing"));
	Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
	Params->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("update_data_table_row"), Params);
	TestFalse(TEXT("update_data_table_row rejects mutation without dry_run or confirm"), Result.bSuccess);
	TestTrue(TEXT("write gate error mentions dry_run or confirm"), Result.ErrorMessage.Contains(TEXT("dry_run=true or confirm=true")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintDataTableMaintenanceDryRunTest, "Monolith.ParamGuard.Blueprint.DataTableMaintenanceDryRun", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintDataTableMaintenanceDryRunTest::RunTest(const FString& Parameters)
{
	FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry::Get());
	UDataTable* DataTable = GetOrCreateDataTableMaintenanceTestAsset();
	TestNotNull(TEXT("test DataTable exists"), DataTable);

	const FString AssetPath = TEXT("/Game/Tests/Monolith/Blueprint/DT_DataTableMaintenanceGuard");

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("get_data_table_schema"), Params);
		TestTrue(TEXT("get_data_table_schema succeeds for in-memory test DataTable"), Result.bSuccess);
		TestTrue(TEXT("schema result is read-only"), Result.Result.IsValid() && Result.Result->GetBoolField(TEXT("read_only")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
		Params->SetObjectField(TEXT("values"), MakeShared<FJsonObject>());
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("update_data_table_row"), Params);
		TestTrue(TEXT("update_data_table_row dry_run succeeds"), Result.bSuccess);
		TestTrue(TEXT("update_data_table_row dry_run reports no-op without changed"), Result.Result.IsValid() && !Result.Result->GetBoolField(TEXT("changed")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("row_name"), TEXT("RowA"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("remove_data_table_row"), Params);
		TestTrue(TEXT("remove_data_table_row dry_run succeeds"), Result.bSuccess);
		TestTrue(TEXT("remove_data_table_row dry_run reports would_remove"), Result.Result.IsValid() && Result.Result->GetBoolField(TEXT("would_remove")));
		TestTrue(TEXT("remove_data_table_row dry_run does not report changed"), Result.Result.IsValid() && !Result.Result->GetBoolField(TEXT("changed")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("file_path"), TEXT("Saved/AutomationReports/datatable-maintenance-guard.csv"));
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("export_data_table_csv"), Params);
		TestTrue(TEXT("export_data_table_csv dry_run succeeds"), Result.bSuccess);
		TestTrue(TEXT("export_data_table_csv dry_run reports would_export"), Result.Result.IsValid() && Result.Result->GetBoolField(TEXT("would_export")));
	}

	{
		UDataTable* TypedDataTable = GetOrCreateTypedDataTableMaintenanceTestAsset();
		TestNotNull(TEXT("typed test DataTable exists"), TypedDataTable);

		TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
		Values->SetNumberField(TEXT("Count"), 5);
		Values->SetStringField(TEXT("Label"), TEXT("Five"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/DT_DataTableMaintenanceTypedGuard"));
		Params->SetStringField(TEXT("row_name"), TEXT("RowWithInt"));
		Params->SetObjectField(TEXT("values"), Values);
		Params->SetBoolField(TEXT("create_if_missing"), true);
		Params->SetBoolField(TEXT("dry_run"), true);

		const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("update_data_table_row"), Params);
		TestTrue(TEXT("update_data_table_row accepts integer JSON number for int property"), Result.bSuccess);
		TestEqual(TEXT("two fields are imported"), Result.Result.IsValid() ? Result.Result->GetIntegerField(TEXT("fields_set")) : 0, 2);
		TestTrue(TEXT("Count appears in set_fields"), JsonStringArrayContains(Result.Result, TEXT("set_fields"), TEXT("Count")));
		TestFalse(TEXT("no fields are skipped"), Result.Result.IsValid() && Result.Result->HasField(TEXT("skipped_fields")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintDataTableMaintenancePathGuardTest, "Monolith.ParamGuard.Blueprint.DataTableMaintenancePathGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintDataTableMaintenancePathGuardTest::RunTest(const FString& Parameters)
{
	FMonolithBlueprintStructActions::RegisterActions(FMonolithToolRegistry::Get());

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Blueprint/DT_Missing"));
	Params->SetStringField(TEXT("file_path"), TEXT("D:/MonolithOutsideDataTableExport.csv"));
	Params->SetBoolField(TEXT("dry_run"), true);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("export_data_table_csv"), Params);
	TestFalse(TEXT("export_data_table_csv rejects paths outside the project"), Result.bSuccess);
	TestTrue(TEXT("export_data_table_csv reports project directory scope"), Result.ErrorMessage.Contains(TEXT("project directory")));

	return true;
}
