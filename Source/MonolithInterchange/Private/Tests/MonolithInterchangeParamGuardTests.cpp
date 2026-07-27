#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithInterchangeActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardInterchangeImportMalformedParamsTest, "Monolith.ParamGuard.MonolithInterchange.ImportRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardInterchangeImportMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("interchange"), TEXT("get_supported_formats")))
	{
		FMonolithInterchangeActions::RegisterActions(Registry);
	}

	static const TCHAR* ExpectedActions[] = {
		TEXT("get_supported_formats"),
		TEXT("can_import"),
		TEXT("can_reimport"),
		TEXT("get_import_data"),
		TEXT("import_asset"),
		TEXT("import_assets"),
		TEXT("import_scene"),
		TEXT("import_mesh"),
		TEXT("import_skeletal_mesh"),
		TEXT("import_texture"),
		TEXT("import_audio"),
		TEXT("import_with_options"),
		TEXT("update_reimport_path"),
		TEXT("reimport_asset"),
		TEXT("reimport_assets"),
		TEXT("export_asset")
	};

	for (const TCHAR* ActionName : ExpectedActions)
	{
		TestTrue(
			FString::Printf(TEXT("interchange.%s action is registered"), ActionName),
			Registry.HasAction(TEXT("interchange"), ActionName));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));
		Params->SetBoolField(TEXT("dry_run"), true);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestFalse(TEXT("import_asset rejects missing source_file"), Result.bSuccess);
		TestTrue(TEXT("import_asset reports missing source_file"), Result.ErrorMessage.Contains(TEXT("source_file")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("source_file"), TEXT("missing.fbx"));
		Params->SetStringField(TEXT("destination_path"), TEXT("/Game/Imported"));
		Params->SetStringField(TEXT("conflict_policy"), TEXT("fail"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("interchange"), TEXT("import_asset"), Params);
		TestTrue(TEXT("import_asset returns structured row for guarded mutation failure"), Result.bSuccess);
		TestTrue(TEXT("import_asset response object is valid"), Result.Result.IsValid());
	}

	return true;
}
