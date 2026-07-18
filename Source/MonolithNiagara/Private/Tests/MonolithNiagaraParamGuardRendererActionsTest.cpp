#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraParamGuardRendererActionsTest, "Monolith.Niagara.ParamGuard.RendererActions", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraParamGuardRendererActionsTest::RunTest(const FString& Parameters)
{
    // Test HandleAddRenderer
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleAddRenderer(Params);
        TestFalse(TEXT("HandleAddRenderer should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleAddRenderer error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetField(TEXT("class"), MakeShared<FJsonValueNumber>(123)); // invalid type

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleAddRenderer(Params);
        TestFalse(TEXT("HandleAddRenderer should fail when class is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleAddRenderer error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test HandleRemoveRenderer
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetNumberField(TEXT("renderer_index"), 0);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleRemoveRenderer(Params);
        TestFalse(TEXT("HandleRemoveRenderer should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleRemoveRenderer error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test HandleSetRendererMaterial
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetStringField(TEXT("material"), TEXT("/Game/TestMat"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererMaterial(Params);
        TestFalse(TEXT("HandleSetRendererMaterial should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererMaterial error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetField(TEXT("material"), MakeShared<FJsonValueNumber>(123)); // invalid type

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererMaterial(Params);
        TestFalse(TEXT("HandleSetRendererMaterial should fail when material is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererMaterial error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test HandleSetRendererProperty
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetStringField(TEXT("property"), TEXT("TestProp"));
        Params->SetField(TEXT("value"), MakeShared<FJsonValueNumber>(1));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererProperty(Params);
        TestFalse(TEXT("HandleSetRendererProperty should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererProperty error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetField(TEXT("property"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetField(TEXT("value"), MakeShared<FJsonValueNumber>(1));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererProperty(Params);
        TestFalse(TEXT("HandleSetRendererProperty should fail when property is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererProperty error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test HandleGetRendererBindings
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetNumberField(TEXT("renderer_index"), 0);

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetRendererBindings(Params);
        TestFalse(TEXT("HandleGetRendererBindings should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleGetRendererBindings error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    // Test HandleSetRendererBinding
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetField(TEXT("emitter"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetStringField(TEXT("binding_name"), TEXT("TestBinding"));
        Params->SetStringField(TEXT("attribute"), TEXT("TestAttr"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererBinding(Params);
        TestFalse(TEXT("HandleSetRendererBinding should fail when emitter is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererBinding error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetField(TEXT("binding_name"), MakeShared<FJsonValueNumber>(123)); // invalid type
        Params->SetStringField(TEXT("attribute"), TEXT("TestAttr"));

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererBinding(Params);
        TestFalse(TEXT("HandleSetRendererBinding should fail when binding_name is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererBinding error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }
    {
        TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/TestSystem"));
        Params->SetStringField(TEXT("emitter"), TEXT("TestEmitter"));
        Params->SetNumberField(TEXT("renderer_index"), 0);
        Params->SetStringField(TEXT("binding_name"), TEXT("TestBinding"));
        Params->SetField(TEXT("attribute"), MakeShared<FJsonValueNumber>(123)); // invalid type

        FMonolithActionResult Result = FMonolithNiagaraActions::HandleSetRendererBinding(Params);
        TestFalse(TEXT("HandleSetRendererBinding should fail when attribute is not a string"), Result.bSuccess);
        TestTrue(TEXT("HandleSetRendererBinding error code should be ErrInvalidParams"), Result.ErrorCode == FMonolithJsonUtils::ErrInvalidParams);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
