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
