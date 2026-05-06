// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithUIBindingActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIBindingActionsMalformedParamsTest,
    "Monolith.ParamGuard.MonolithUI.BindingActionsRejectsMalformedParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIBindingActionsMalformedParamsTest::RunTest(const FString& /*Parameters*/)
{
    // Test HandleListWidgetEvents
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // Missing asset_path
        FMonolithActionResult Result = FMonolithUIBindingActions::HandleListWidgetEvents(Params);
        TestTrue(TEXT("HandleListWidgetEvents rejects missing asset_path"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("asset_path")));

        Params->SetStringField(TEXT("asset_path"), TEXT(""));
        // Empty asset_path
        Result = FMonolithUIBindingActions::HandleListWidgetEvents(Params);
        TestTrue(TEXT("HandleListWidgetEvents rejects empty asset_path"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("asset_path")));
    }

    // Test HandleListWidgetProperties
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/SomeWidget"));
        // Missing widget_name
        FMonolithActionResult Result = FMonolithUIBindingActions::HandleListWidgetProperties(Params);
        TestTrue(TEXT("HandleListWidgetProperties rejects missing widget_name"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("widget_name")));

        Params->SetStringField(TEXT("widget_name"), TEXT(""));
        // Empty widget_name
        Result = FMonolithUIBindingActions::HandleListWidgetProperties(Params);
        TestTrue(TEXT("HandleListWidgetProperties rejects empty widget_name"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("widget_name")));
    }

    // Test HandleSetupListView
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), TEXT("/Game/SomeWidget"));
        Params->SetStringField(TEXT("list_widget_name"), TEXT("MyList"));
        // Missing entry_widget_class
        FMonolithActionResult Result = FMonolithUIBindingActions::HandleSetupListView(Params);
        TestTrue(TEXT("HandleSetupListView rejects missing entry_widget_class"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("entry_widget_class")));
    }

    // Test HandleGetWidgetBindings
    {
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        // Missing asset_path
        FMonolithActionResult Result = FMonolithUIBindingActions::HandleGetWidgetBindings(Params);
        TestTrue(TEXT("HandleGetWidgetBindings rejects missing asset_path"), !Result.bSuccess && Result.ErrorMessage.Contains(TEXT("asset_path")));
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
