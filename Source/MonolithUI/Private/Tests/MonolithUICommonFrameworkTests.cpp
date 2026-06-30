// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICommonFrameworkStatusTest,
    "Monolith.UI.CommonFramework.Status",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICommonFrameworkStatusTest::RunTest(const FString& /*Parameters*/)
{
    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetBoolField(TEXT("include_properties"), true);
    Params->SetNumberField(TEXT("property_limit"), 4);

    const FMonolithActionResult Result = FMonolithUIActions::HandleGetCommonFrameworkStatus(Params);
    TestTrue(TEXT("status action succeeds"), Result.bSuccess);
    TestTrue(TEXT("status result object is valid"), Result.Result.IsValid());
    if (!Result.bSuccess || !Result.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Classes = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Structs = nullptr;
    TestTrue(TEXT("plugins array exists"), Result.Result->TryGetArrayField(TEXT("plugins"), Plugins) && Plugins && Plugins->Num() >= 9);
    TestTrue(TEXT("modules array exists"), Result.Result->TryGetArrayField(TEXT("modules"), Modules) && Modules && Modules->Num() >= 11);
    TestTrue(TEXT("classes array exists"), Result.Result->TryGetArrayField(TEXT("classes"), Classes) && Classes && Classes->Num() >= 20);
    TestTrue(TEXT("structs array exists"), Result.Result->TryGetArrayField(TEXT("structs"), Structs) && Structs && Structs->Num() >= 4);

    const auto ArrayContainsStringField = [](const TArray<TSharedPtr<FJsonValue>>* Values, const TCHAR* FieldName, const TCHAR* ExpectedValue) -> bool
    {
        if (!Values)
        {
            return false;
        }

        for (const TSharedPtr<FJsonValue>& Value : *Values)
        {
            const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
            FString ActualValue;
            if (Obj.IsValid() && Obj->TryGetStringField(FieldName, ActualValue) && ActualValue == ExpectedValue)
            {
                return true;
            }
        }
        return false;
    };

    bool bAvailabilityField = false;
    TestTrue(TEXT("common_loading_screen_available field exists"), Result.Result->TryGetBoolField(TEXT("common_loading_screen_available"), bAvailabilityField));
    TestTrue(TEXT("game_settings_available field exists"), Result.Result->TryGetBoolField(TEXT("game_settings_available"), bAvailabilityField));
    TestTrue(TEXT("gameplay_message_router_available field exists"), Result.Result->TryGetBoolField(TEXT("gameplay_message_router_available"), bAvailabilityField));
    TestTrue(TEXT("modular_gameplay_actors_available field exists"), Result.Result->TryGetBoolField(TEXT("modular_gameplay_actors_available"), bAvailabilityField));
    TestTrue(TEXT("game_subtitles_available field exists"), Result.Result->TryGetBoolField(TEXT("game_subtitles_available"), bAvailabilityField));

    TestTrue(TEXT("CommonLoadingScreen plugin is reported"), ArrayContainsStringField(Plugins, TEXT("name"), TEXT("CommonLoadingScreen")));
    TestTrue(TEXT("GameSettings plugin is reported"), ArrayContainsStringField(Plugins, TEXT("name"), TEXT("GameSettings")));
    TestTrue(TEXT("GameplayMessageRouter plugin is reported"), ArrayContainsStringField(Plugins, TEXT("name"), TEXT("GameplayMessageRouter")));
    TestTrue(TEXT("ModularGameplayActors plugin is reported"), ArrayContainsStringField(Plugins, TEXT("name"), TEXT("ModularGameplayActors")));
    TestTrue(TEXT("GameSubtitles plugin is reported"), ArrayContainsStringField(Plugins, TEXT("name"), TEXT("GameSubtitles")));

    TestTrue(TEXT("PrimaryGameLayout class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/CommonGame.PrimaryGameLayout")));
    TestTrue(TEXT("LoadingScreenManager class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/CommonLoadingScreen.LoadingScreenManager")));
    TestTrue(TEXT("GameSettingRegistry class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/GameSettings.GameSettingRegistry")));
    TestTrue(TEXT("GameplayMessageSubsystem class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageSubsystem")));
    TestTrue(TEXT("ModularCharacter class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/ModularGameplayActors.ModularCharacter")));
    TestTrue(TEXT("SubtitleDisplaySubsystem class spec is reported"), ArrayContainsStringField(Classes, TEXT("path"), TEXT("/Script/GameSubtitles.SubtitleDisplaySubsystem")));

    TestTrue(TEXT("GameplayMessageListenerHandle struct spec is reported"), ArrayContainsStringField(Structs, TEXT("path"), TEXT("/Script/GameplayMessageRuntime.GameplayMessageListenerHandle")));
    TestTrue(TEXT("GameSettingFilterState struct spec is reported"), ArrayContainsStringField(Structs, TEXT("path"), TEXT("/Script/GameSettings.GameSettingFilterState")));
    TestTrue(TEXT("SubtitleFormat struct spec is reported"), ArrayContainsStringField(Structs, TEXT("path"), TEXT("/Script/GameSubtitles.SubtitleFormat")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICommonFrameworkRegistrationTest,
    "Monolith.UI.CommonFramework.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICommonFrameworkRegistrationTest::RunTest(const FString& /*Parameters*/)
{
    FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

    bool bHasStatus = false;
    bool bHasAddExtensionPoint = false;
    bool bHasDescribe = false;
    bool bHasAddLayer = false;
    bool bHasDescribeMessagingFlow = false;
    bool bHasValidateDialogContract = false;
    bool bHasValidateLayerPushContract = false;
    bool bStatusCategory = false;
    bool bDescribeCategory = false;
    bool bAddLayerCategory = false;
    bool bDescribeMessagingFlowCategory = false;
    bool bValidateDialogContractCategory = false;
    bool bValidateLayerPushContractCategory = false;
    for (const FMonolithActionInfo& ActionInfo : Registry.GetActions(TEXT("ui")))
    {
        if (ActionInfo.Action == TEXT("get_common_framework_status"))
        {
            bHasStatus = true;
            bStatusCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
        else if (ActionInfo.Action == TEXT("add_extension_point_widget"))
        {
            bHasAddExtensionPoint = true;
        }
        else if (ActionInfo.Action == TEXT("add_primary_game_layout_layer"))
        {
            bHasAddLayer = true;
            bAddLayerCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
        else if (ActionInfo.Action == TEXT("describe_common_widget_blueprint"))
        {
            bHasDescribe = true;
            bDescribeCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
        else if (ActionInfo.Action == TEXT("describe_common_messaging_flow"))
        {
            bHasDescribeMessagingFlow = true;
            bDescribeMessagingFlowCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
        else if (ActionInfo.Action == TEXT("validate_common_dialog_contract"))
        {
            bHasValidateDialogContract = true;
            bValidateDialogContractCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
        else if (ActionInfo.Action == TEXT("validate_common_layer_push_contract"))
        {
            bHasValidateLayerPushContract = true;
            bValidateLayerPushContractCategory = ActionInfo.Category == TEXT("CommonFramework");
        }
    }

    TestTrue(TEXT("get_common_framework_status registered"), bHasStatus);
    TestTrue(TEXT("get_common_framework_status category"), bStatusCategory);
    TestTrue(TEXT("add_extension_point_widget registered"), bHasAddExtensionPoint);
    TestTrue(TEXT("add_primary_game_layout_layer registered"), bHasAddLayer);
    TestTrue(TEXT("add_primary_game_layout_layer category"), bAddLayerCategory);
    TestTrue(TEXT("describe_common_widget_blueprint registered"), bHasDescribe);
    TestTrue(TEXT("describe_common_widget_blueprint category"), bDescribeCategory);
    TestTrue(TEXT("describe_common_messaging_flow registered"), bHasDescribeMessagingFlow);
    TestTrue(TEXT("describe_common_messaging_flow category"), bDescribeMessagingFlowCategory);
    TestTrue(TEXT("validate_common_dialog_contract registered"), bHasValidateDialogContract);
    TestTrue(TEXT("validate_common_dialog_contract category"), bValidateDialogContractCategory);
    TestTrue(TEXT("validate_common_layer_push_contract registered"), bHasValidateLayerPushContract);
    TestTrue(TEXT("validate_common_layer_push_contract category"), bValidateLayerPushContractCategory);

    const FMonolithActionResult MissingExtensionPointAssetPath = FMonolithUIActions::HandleAddExtensionPointWidget(MakeShared<FJsonObject>());
    TestFalse(TEXT("add_extension_point_widget rejects missing asset_path"), MissingExtensionPointAssetPath.bSuccess);
    TestTrue(TEXT("missing extension point asset_path error is clear"), MissingExtensionPointAssetPath.ErrorMessage.Contains(TEXT("asset_path")));

    const FMonolithActionResult MissingLayerAssetPath = FMonolithUIActions::HandleAddPrimaryGameLayoutLayer(MakeShared<FJsonObject>());
    TestFalse(TEXT("add_primary_game_layout_layer rejects missing asset_path"), MissingLayerAssetPath.bSuccess);
    TestTrue(TEXT("missing layer asset_path error is clear"), MissingLayerAssetPath.ErrorMessage.Contains(TEXT("asset_path")));

    const FMonolithActionResult MissingAssetPath = FMonolithUIActions::HandleDescribeCommonWidgetBlueprint(MakeShared<FJsonObject>());
    TestFalse(TEXT("describe_common_widget_blueprint rejects missing asset_path"), MissingAssetPath.bSuccess);
    TestTrue(TEXT("missing asset_path error is clear"), MissingAssetPath.ErrorMessage.Contains(TEXT("asset_path")));

    TSharedPtr<FJsonObject> RequireLayoutParams = MakeShared<FJsonObject>();
    RequireLayoutParams->SetBoolField(TEXT("require_layout_asset"), true);
    const FMonolithActionResult MissingLayoutPath = FMonolithUIActions::HandleValidateCommonLayerPushContract(RequireLayoutParams);
    TestFalse(TEXT("validate_common_layer_push_contract rejects missing required layout_asset_path"), MissingLayoutPath.bSuccess);
    TestTrue(TEXT("missing layout_asset_path error is clear"), MissingLayoutPath.ErrorMessage.Contains(TEXT("layout_asset_path")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUICommonFrameworkMessagingContractTest,
    "Monolith.UI.CommonFramework.MessagingContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUICommonFrameworkMessagingContractTest::RunTest(const FString& /*Parameters*/)
{
    const FMonolithActionResult DescribeResult = FMonolithUIActions::HandleDescribeCommonMessagingFlow(MakeShared<FJsonObject>());
    TestTrue(TEXT("describe_common_messaging_flow succeeds"), DescribeResult.bSuccess);
    TestTrue(TEXT("describe_common_messaging_flow result object is valid"), DescribeResult.Result.IsValid());
    if (!DescribeResult.bSuccess || !DescribeResult.Result.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Checks = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
    TestTrue(TEXT("describe checks array exists"), DescribeResult.Result->TryGetArrayField(TEXT("checks"), Checks) && Checks);
    TestTrue(TEXT("describe issues array exists"), DescribeResult.Result->TryGetArrayField(TEXT("issues"), Issues) && Issues);
    TestTrue(TEXT("describe warnings array exists"), DescribeResult.Result->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);

    FString ModalLayerTag;
    TestTrue(TEXT("modal_layer_tag exists"), DescribeResult.Result->TryGetStringField(TEXT("modal_layer_tag"), ModalLayerTag));
    TestEqual(TEXT("default modal layer tag"), ModalLayerTag, FString(TEXT("UI.Layer.Modal")));

    const TSharedPtr<FJsonObject>* MessagingClass = nullptr;
    const TSharedPtr<FJsonObject>* ConfirmationDialog = nullptr;
    TestTrue(TEXT("messaging_class object exists"), DescribeResult.Result->TryGetObjectField(TEXT("messaging_class"), MessagingClass) && MessagingClass && MessagingClass->IsValid());
    TestTrue(TEXT("confirmation_dialog object exists"), DescribeResult.Result->TryGetObjectField(TEXT("confirmation_dialog"), ConfirmationDialog) && ConfirmationDialog && ConfirmationDialog->IsValid());

    TSharedPtr<FJsonObject> BadDialogParams = MakeShared<FJsonObject>();
    BadDialogParams->SetStringField(TEXT("confirmation_dialog_class"), TEXT("/Script/Engine.Actor"));
    BadDialogParams->SetStringField(TEXT("error_dialog_class"), TEXT("/Script/Engine.Actor"));
    const FMonolithActionResult BadDialogResult = FMonolithUIActions::HandleValidateCommonDialogContract(BadDialogParams);
    TestTrue(TEXT("validate_common_dialog_contract returns structured result for invalid dialog classes"), BadDialogResult.bSuccess);
    TestTrue(TEXT("validate_common_dialog_contract result object is valid"), BadDialogResult.Result.IsValid());
    if (BadDialogResult.Result.IsValid())
    {
        bool bOk = true;
        TestTrue(TEXT("dialog contract ok field exists"), BadDialogResult.Result->TryGetBoolField(TEXT("ok"), bOk));
        TestFalse(TEXT("Actor is not a valid CommonGameDialog"), bOk);

        const TArray<TSharedPtr<FJsonValue>>* Dialogs = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* DialogIssues = nullptr;
        TestTrue(TEXT("dialogs array exists"), BadDialogResult.Result->TryGetArrayField(TEXT("dialogs"), Dialogs) && Dialogs && Dialogs->Num() == 2);
        TestTrue(TEXT("dialog issues array exists"), BadDialogResult.Result->TryGetArrayField(TEXT("issues"), DialogIssues) && DialogIssues && DialogIssues->Num() > 0);
    }

    TSharedPtr<FJsonObject> LayerParams = MakeShared<FJsonObject>();
    LayerParams->SetStringField(TEXT("dialog_class"), TEXT("/Script/Engine.Actor"));
    const FMonolithActionResult LayerResult = FMonolithUIActions::HandleValidateCommonLayerPushContract(LayerParams);
    TestTrue(TEXT("validate_common_layer_push_contract returns structured result without layout asset"), LayerResult.bSuccess);
    TestTrue(TEXT("validate_common_layer_push_contract result object is valid"), LayerResult.Result.IsValid());
    if (LayerResult.Result.IsValid())
    {
        bool bOk = true;
        TestTrue(TEXT("layer contract ok field exists"), LayerResult.Result->TryGetBoolField(TEXT("ok"), bOk));
        TestFalse(TEXT("Actor is not valid for layer push dialog"), bOk);
        const TSharedPtr<FJsonObject>* Layout = nullptr;
        TestTrue(TEXT("layout object exists"), LayerResult.Result->TryGetObjectField(TEXT("layout"), Layout) && Layout && Layout->IsValid());
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
