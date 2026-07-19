#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardBasicEmitterTests, "Monolith.ParamGuard.Niagara.BasicEmitterTests", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardBasicEmitterTests::RunTest(const FString& Parameters)
{
    // Test HandleAddEmitter: wrong type for emitter_asset
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter_asset"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleAddEmitter(Params);
        TestFalse(TEXT("HandleAddEmitter should fail gracefully with wrong-type emitter_asset"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter_asset type issue"), Result.ErrorMessage.Contains(TEXT("emitter_asset")));
    }

    // Test HandleRemoveEmitter: wrong type for emitter
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRemoveEmitter(Params);
        TestFalse(TEXT("HandleRemoveEmitter should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    // Test HandleDuplicateEmitter: wrong type for source_emitter
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("source_emitter"), 12345);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleDuplicateEmitter(Params);
        TestFalse(TEXT("HandleDuplicateEmitter should fail gracefully with wrong-type source_emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention source_emitter type issue"), Result.ErrorMessage.Contains(TEXT("source_emitter")));
    }

    // Test HandleSetEmitterEnabled: wrong type for emitter
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/NonExistentSystem"));
        Params->SetNumberField(TEXT("emitter"), 12345);
        Params->SetBoolField(TEXT("enabled"), true);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetEmitterEnabled(Params);
        TestFalse(TEXT("HandleSetEmitterEnabled should fail gracefully with wrong-type emitter"), Result.bSuccess);
        TestTrue(TEXT("Error message should mention emitter type issue"), Result.ErrorMessage.Contains(TEXT("emitter")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardNiagaraUsageIdTest, "Monolith.ParamGuard.Niagara.UsageId", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardNiagaraUsageIdTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("usage_id"), 12345); // Malformed, should be string
	Params->SetNumberField(TEXT("stage_index"), 0);

	FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetCustomHLSLText(Params);
	TestFalse(TEXT("Should fail gracefully for missing script_path, but more importantly not crash on node_guid"), Result.bSuccess);

	// Better test:
	Params->SetStringField(TEXT("script_path"), TEXT("/Some/Path/To/Script"));
	Params->SetNumberField(TEXT("node_guid"), 12345); // Malformed

	Result = FMonolithNiagaraActions::HandleGetCustomHLSLText(Params);
	TestFalse(TEXT("Should fail when node_guid is not a string"), Result.bSuccess);
	TestTrue(TEXT("Error message should mention node_guid must be a string"), Result.ErrorMessage.Contains(TEXT("Parameter 'node_guid' must be a string")));

	// Test HandleGetCustomHLSLText: missing script_path
	{
		TSharedRef<FJsonObject> GetParams = MakeShared<FJsonObject>();
		GetParams->SetNumberField(TEXT("script_path"), 123); // wrong type

		Result = FMonolithNiagaraActions::HandleGetCustomHLSLText(GetParams);
		TestFalse(TEXT("HandleGetCustomHLSLText should fail gracefully with wrong-type script_path"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention script_path must be a string"), Result.ErrorMessage.Contains(TEXT("script_path must be a string")));
	}

	// Test HandleSetCustomHLSLText: missing script_path
	{
		TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetNumberField(TEXT("script_path"), 123); // wrong type
		SetParams->SetStringField(TEXT("hlsl"), TEXT("return 0;"));

		Result = FMonolithNiagaraActions::HandleSetCustomHLSLText(SetParams);
		TestFalse(TEXT("HandleSetCustomHLSLText should fail gracefully with wrong-type script_path"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention script_path must be a string"), Result.ErrorMessage.Contains(TEXT("script_path must be a string")));
	}

	// Test HandleSetCustomHLSLText: missing hlsl
	{
		TSharedRef<FJsonObject> SetParams = MakeShared<FJsonObject>();
		SetParams->SetStringField(TEXT("script_path"), TEXT("/Some/Path/To/Script"));
		SetParams->SetNumberField(TEXT("hlsl"), 123); // wrong type

		Result = FMonolithNiagaraActions::HandleSetCustomHLSLText(SetParams);
		TestFalse(TEXT("HandleSetCustomHLSLText should fail gracefully with wrong-type hlsl"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention hlsl must be a string"), Result.ErrorMessage.Contains(TEXT("hlsl must be a string")));
	}

	return true;
}
