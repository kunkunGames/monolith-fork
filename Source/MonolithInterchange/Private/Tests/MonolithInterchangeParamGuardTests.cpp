#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithInterchangeActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardInterchangeImportMalformedParamsTest, "Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardInterchangeImportMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithInterchangeActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("interchange.import_asset action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("import_asset")));
	TestTrue(TEXT("interchange.import_assets action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("import_assets")));
	TestTrue(TEXT("interchange.export_asset action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("interchange"), TEXT("export_asset")));

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
		Params->SetBoolField(TEXT("dry_run"), true);

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestFalse(TEXT("import_asset rejects missing source_file"), Result.bSuccess);
		TestTrue(TEXT("import_asset reports missing source_file"), Result.ErrorMessage.Contains(TEXT("source_file")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_file"), TEXT("missing.fbx"));
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));

		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestTrue(TEXT("import_asset returns structured row for guarded mutation failure"), Result.bSuccess);
		TestTrue(TEXT("import_asset response object is valid"), Result.Result.IsValid());
	}

	return true;
}
