// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Components/Button.h"
#include "Components/VerticalBox.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_VariableGet.h"
#include "Misc/AutomationTest.h"

#include "MonolithToolRegistry.h"
#include "MonolithUIActions.h"
#include "MonolithUIFrontendFocusUtils.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"

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
    bool bHasValidateFrontendMenuFlow = false;
    bool bStatusCategory = false;
    bool bDescribeCategory = false;
    bool bAddLayerCategory = false;
    bool bDescribeMessagingFlowCategory = false;
    bool bValidateDialogContractCategory = false;
    bool bValidateLayerPushContractCategory = false;
    bool bValidateFrontendMenuFlowCategory = false;
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
        else if (ActionInfo.Action == TEXT("validate_frontend_menu_flow"))
        {
            bHasValidateFrontendMenuFlow = true;
            bValidateFrontendMenuFlowCategory = ActionInfo.Category == TEXT("CommonFramework");
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
    TestTrue(TEXT("validate_frontend_menu_flow registered"), bHasValidateFrontendMenuFlow);
    TestTrue(TEXT("validate_frontend_menu_flow category"), bValidateFrontendMenuFlowCategory);

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

    const FMonolithActionResult EmptyFrontendFlow = FMonolithUIActions::HandleValidateFrontendMenuFlow(MakeShared<FJsonObject>());
    TestFalse(TEXT("validate_frontend_menu_flow rejects empty contract"), EmptyFrontendFlow.bSuccess);
    TestTrue(TEXT("empty frontend flow error is clear"), EmptyFrontendFlow.ErrorMessage.Contains(TEXT("requires at least one")));

    TSharedPtr<FJsonObject> BadScreensParams = MakeShared<FJsonObject>();
    BadScreensParams->SetObjectField(TEXT("screens"), MakeShared<FJsonObject>());
    const FMonolithActionResult BadScreens = FMonolithUIActions::HandleValidateFrontendMenuFlow(BadScreensParams);
    TestFalse(TEXT("validate_frontend_menu_flow rejects non-array screens"), BadScreens.bSuccess);
    TestTrue(TEXT("bad screens error is clear"), BadScreens.ErrorMessage.Contains(TEXT("screens")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIFrontendFocusResolutionTest,
    "Monolith.UI.CommonFramework.FrontendFocusResolution",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIFrontendFocusResolutionTest::RunTest(const FString& /*Parameters*/)
{
    UWidgetBlueprint* WidgetBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage());
    TestNotNull(TEXT("transient Widget Blueprint created"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }

    WidgetBlueprint->WidgetTree = NewObject<UWidgetTree>(WidgetBlueprint);
    UVerticalBox* RootWidget = NewObject<UVerticalBox>(WidgetBlueprint->WidgetTree, TEXT("Root"));
    UButton* FocusWidget = NewObject<UButton>(WidgetBlueprint->WidgetTree, TEXT("LyraListView"));
    UButton* AlternateFocusWidget = NewObject<UButton>(WidgetBlueprint->WidgetTree, TEXT("AlternateFocus"));
    RootWidget->AddChildToVerticalBox(FocusWidget);
    RootWidget->AddChildToVerticalBox(AlternateFocusWidget);
    WidgetBlueprint->WidgetTree->RootWidget = RootWidget;

    UEdGraph* FocusGraph = NewObject<UEdGraph>(WidgetBlueprint, TEXT("BP_GetDesiredFocusTarget"));
    WidgetBlueprint->FunctionGraphs.Add(FocusGraph);

    UEdGraphNode* DecoyNode = NewObject<UEdGraphNode>(FocusGraph);
    UK2Node_FunctionResult* ReturnNode = NewObject<UK2Node_FunctionResult>(FocusGraph);
    UK2Node_VariableGet* VariableGet = NewObject<UK2Node_VariableGet>(FocusGraph);
    VariableGet->VariableReference.SetSelfMember(FName(TEXT("LyraListView")));
    FocusGraph->Nodes.Add(DecoyNode);
    FocusGraph->Nodes.Add(ReturnNode);
    FocusGraph->Nodes.Add(VariableGet);

    DecoyNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("ReturnValue"));
    UEdGraphPin* ReturnPin = ReturnNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("ReturnValue"));
    UEdGraphPin* WidgetPin = VariableGet->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("LyraListView"));
    WidgetPin->MakeLinkTo(ReturnPin);

    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution ConnectedResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestEqual(TEXT("connected Blueprint override resolves widget"), ConnectedResolution.WidgetName, FName(TEXT("LyraListView")));
    TestEqual(TEXT("connected Blueprint override reports source"), ConnectedResolution.Source, FString(TEXT("blueprint_override")));
    TestTrue(TEXT("connected Blueprint override is detected"), ConnectedResolution.bOverrideGraphPresent);

    UEdGraphPin* NonValuePin = VariableGet->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("NonValuePin"));
    WidgetPin->BreakLinkTo(ReturnPin);
    NonValuePin->MakeLinkTo(ReturnPin);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution NonValuePinResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("non-value output on a variable getter fails closed"), NonValuePinResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("non-value output reports unsupported return"),
        NonValuePinResolution.Source,
        FString(TEXT("blueprint_override_unsupported_return")));
    NonValuePin->BreakLinkTo(ReturnPin);
    WidgetPin->MakeLinkTo(ReturnPin);

    UEdGraphPin* UnsupportedOutputPin = DecoyNode->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("LyraListView"));
    WidgetPin->BreakLinkTo(ReturnPin);
    UnsupportedOutputPin->MakeLinkTo(ReturnPin);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution UnsupportedResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("ordinary graph node return fails closed"), UnsupportedResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("ordinary graph node reports unsupported return"),
        UnsupportedResolution.Source,
        FString(TEXT("blueprint_override_unsupported_return")));
    UnsupportedOutputPin->BreakLinkTo(ReturnPin);
    WidgetPin->MakeLinkTo(ReturnPin);

    UK2Node_VariableGet* MissingVariableGet = NewObject<UK2Node_VariableGet>(FocusGraph);
    MissingVariableGet->VariableReference.SetSelfMember(FName(TEXT("MissingWidget")));
    FocusGraph->Nodes.Add(MissingVariableGet);
    UEdGraphPin* MissingWidgetPin = MissingVariableGet->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("MissingWidget"));
    WidgetPin->BreakLinkTo(ReturnPin);
    MissingWidgetPin->MakeLinkTo(ReturnPin);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution MissingWidgetResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("missing widget variable fails closed"), MissingWidgetResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("missing widget variable reports missing widget"),
        MissingWidgetResolution.Source,
        FString(TEXT("blueprint_override_widget_missing")));
    MissingWidgetPin->BreakLinkTo(ReturnPin);
    FocusGraph->Nodes.Remove(MissingVariableGet);
    WidgetPin->MakeLinkTo(ReturnPin);

    UK2Node_FunctionResult* MatchingReturnNode = NewObject<UK2Node_FunctionResult>(FocusGraph);
    UK2Node_VariableGet* MatchingVariableGet = NewObject<UK2Node_VariableGet>(FocusGraph);
    MatchingVariableGet->VariableReference.SetSelfMember(FName(TEXT("LyraListView")));
    FocusGraph->Nodes.Add(MatchingReturnNode);
    FocusGraph->Nodes.Add(MatchingVariableGet);
    UEdGraphPin* MatchingReturnPin = MatchingReturnNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("ReturnValue"));
    UEdGraphPin* MatchingWidgetPin = MatchingVariableGet->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("LyraListView"));
    MatchingWidgetPin->MakeLinkTo(MatchingReturnPin);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution MatchingReturnsResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestEqual(
        TEXT("matching Blueprint return nodes resolve the shared widget"),
        MatchingReturnsResolution.WidgetName,
        FName(TEXT("LyraListView")));
    TestEqual(
        TEXT("matching Blueprint return nodes report override source"),
        MatchingReturnsResolution.Source,
        FString(TEXT("blueprint_override")));
    FocusGraph->Nodes.Remove(MatchingReturnNode);
    FocusGraph->Nodes.Remove(MatchingVariableGet);

    UK2Node_FunctionResult* ConflictingReturnNode = NewObject<UK2Node_FunctionResult>(FocusGraph);
    UK2Node_VariableGet* AlternateVariableGet = NewObject<UK2Node_VariableGet>(FocusGraph);
    AlternateVariableGet->VariableReference.SetSelfMember(FName(TEXT("AlternateFocus")));
    FocusGraph->Nodes.Add(ConflictingReturnNode);
    FocusGraph->Nodes.Add(AlternateVariableGet);
    UEdGraphPin* ConflictingReturnPin = ConflictingReturnNode->CreatePin(
        EGPD_Input,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("ReturnValue"));
    UEdGraphPin* AlternateWidgetPin = AlternateVariableGet->CreatePin(
        EGPD_Output,
        UEdGraphSchema_K2::PC_Object,
        UWidget::StaticClass(),
        TEXT("AlternateFocus"));
    AlternateWidgetPin->MakeLinkTo(ConflictingReturnPin);

    AlternateWidgetPin->MakeLinkTo(ReturnPin);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution MultiplyLinkedResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("multiply-linked return fails closed"), MultiplyLinkedResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("multiply-linked return reports ambiguity"),
        MultiplyLinkedResolution.Source,
        FString(TEXT("blueprint_override_ambiguous")));
    AlternateWidgetPin->BreakLinkTo(ReturnPin);

    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution AmbiguousResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("conflicting Blueprint return nodes fail closed"), AmbiguousResolution.WidgetName.IsNone());
    TestEqual(TEXT("conflicting Blueprint return nodes report ambiguity"), AmbiguousResolution.Source, FString(TEXT("blueprint_override_ambiguous")));
    TestTrue(TEXT("conflicting Blueprint return nodes remain detected"), AmbiguousResolution.bOverrideGraphPresent);

    FocusGraph->Nodes.Remove(ConflictingReturnNode);
    FocusGraph->Nodes.Remove(AlternateVariableGet);
    ReturnPin->BreakAllPinLinks(false);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution UnconnectedResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(WidgetBlueprint);
    TestTrue(TEXT("unconnected Blueprint override fails closed"), UnconnectedResolution.WidgetName.IsNone());
    TestEqual(TEXT("unconnected Blueprint override reports source"), UnconnectedResolution.Source, FString(TEXT("blueprint_override_unconnected")));
    TestTrue(TEXT("unconnected Blueprint override remains detected"), UnconnectedResolution.bOverrideGraphPresent);

    UWidgetBlueprint* EmptyOverrideBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage());
    UEdGraph* EmptyOverrideGraph =
        NewObject<UEdGraph>(EmptyOverrideBlueprint, TEXT("BP_GetDesiredFocusTarget"));
    EmptyOverrideBlueprint->FunctionGraphs.Add(EmptyOverrideGraph);
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution MissingReturnResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(EmptyOverrideBlueprint);
    TestTrue(TEXT("override without a function result fails closed"), MissingReturnResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("override without a function result reports missing return"),
        MissingReturnResolution.Source,
        FString(TEXT("blueprint_override_return_missing")));
    TestTrue(TEXT("empty override graph remains detected"), MissingReturnResolution.bOverrideGraphPresent);

    UWidgetBlueprint* NoOverrideBlueprint = NewObject<UWidgetBlueprint>(GetTransientPackage());
    const MonolithUIFrontendFlowInternal::FDesiredFocusResolution NoOverrideResolution =
        MonolithUIFrontendFlowInternal::ResolveDesiredFocusWidget(NoOverrideBlueprint);
    TestTrue(TEXT("class-default fallback without generated class resolves none"), NoOverrideResolution.WidgetName.IsNone());
    TestEqual(
        TEXT("class-default fallback reports none"),
        NoOverrideResolution.Source,
        FString(TEXT("class_default_none")));
    TestFalse(TEXT("class-default fallback has no override graph"), NoOverrideResolution.bOverrideGraphPresent);

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

    TSharedPtr<FJsonObject> FrontendParams = MakeShared<FJsonObject>();
    FrontendParams->SetStringField(TEXT("dialog_class"), TEXT("/Script/Engine.Actor"));
    const FMonolithActionResult FrontendResult = FMonolithUIActions::HandleValidateFrontendMenuFlow(FrontendParams);
    TestTrue(TEXT("validate_frontend_menu_flow returns structured result for invalid dialog class"), FrontendResult.bSuccess);
    TestTrue(TEXT("validate_frontend_menu_flow result object is valid"), FrontendResult.Result.IsValid());
    if (FrontendResult.Result.IsValid())
    {
        bool bOk = true;
        TestTrue(TEXT("frontend flow ok field exists"), FrontendResult.Result->TryGetBoolField(TEXT("ok"), bOk));
        TestFalse(TEXT("Actor is not a valid frontend flow dialog"), bOk);

        const TArray<TSharedPtr<FJsonValue>>* FrontendIssues = nullptr;
        TestTrue(TEXT("frontend flow issues array exists"), FrontendResult.Result->TryGetArrayField(TEXT("issues"), FrontendIssues) && FrontendIssues && FrontendIssues->Num() > 0);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
