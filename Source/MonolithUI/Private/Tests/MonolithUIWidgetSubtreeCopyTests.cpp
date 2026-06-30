// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithToolRegistry.h"
#include "MonolithUICommon.h"
#include "MonolithUIWidgetCopyActions.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "WidgetBlueprint.h"

namespace
{
    void EnsureWidgetSubtreeCopyActionRegistered()
    {
        FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
        if (!Registry.HasAction(TEXT("ui"), TEXT("copy_widget_subtree_with_class_remap")))
        {
            FMonolithUIWidgetCopyActions::RegisterActions(Registry);
        }
    }

    TSharedPtr<FJsonObject> MakeClassRemapObject(const FString& From, const FString& To)
    {
        TSharedPtr<FJsonObject> Remaps = MakeShared<FJsonObject>();
        Remaps->SetStringField(From, To);
        return Remaps;
    }

    TSharedPtr<FJsonObject> MakeBasicPayload()
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("source_asset_path"), TEXT("/Game/Tests/Monolith/UI/SubtreeCopy/WBP_MissingSource"));
        Payload->SetStringField(TEXT("destination_asset_path"), TEXT("/Game/Tests/Monolith/UI/SubtreeCopy/WBP_MissingDestination"));
        Payload->SetStringField(TEXT("source_widget_name"), TEXT("SourcePanel"));
        return Payload;
    }

    bool AddLabelToSourcePanel(UWidgetBlueprint* SourceWBP, FString& OutError)
    {
        if (!SourceWBP || !SourceWBP->WidgetTree)
        {
            OutError = TEXT("Source WBP fixture is invalid");
            return false;
        }

        UVerticalBox* SourcePanel = Cast<UVerticalBox>(SourceWBP->WidgetTree->FindWidget(TEXT("SourcePanel")));
        if (!SourcePanel)
        {
            OutError = TEXT("SourcePanel fixture widget is missing or not a UVerticalBox");
            return false;
        }

        UTextBlock* Label = SourceWBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Label"));
        if (!Label)
        {
            OutError = TEXT("Failed to construct Label TextBlock fixture widget");
            return false;
        }

        Label->SetText(FText::FromString(TEXT("Copied label")));
        SourcePanel->AddChild(Label);
        MonolithUI::RegisterCreatedWidget(SourceWBP, Label);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(SourceWBP);
        FKismetEditorUtilities::CompileBlueprint(SourceWBP);
        SourceWBP->GetOutermost()->MarkPackageDirty();
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIWidgetSubtreeCopySchemaTest,
    "Monolith.Registry.UI.CopyWidgetSubtreeWithClassRemapSchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIWidgetSubtreeCopySchemaTest::RunTest(const FString& /*Parameters*/)
{
    EnsureWidgetSubtreeCopyActionRegistered();

    bool bFoundAction = false;
    bool bCategoryOk = false;
    bool bHasSourceAsset = false;
    bool bHasDestinationAsset = false;
    bool bHasClassRemaps = false;
    bool bDryRunDefault = false;
    bool bConfirmDefault = false;

    for (const FMonolithActionInfo& ActionInfo : FMonolithToolRegistry::Get().GetActions(TEXT("ui")))
    {
        if (ActionInfo.Action != TEXT("copy_widget_subtree_with_class_remap"))
        {
            continue;
        }

        bFoundAction = true;
        bCategoryOk = ActionInfo.Category == TEXT("PostCopyRepair");
        if (ActionInfo.ParamSchema.IsValid())
        {
            const TSharedPtr<FJsonObject>* SourceAsset = nullptr;
            const TSharedPtr<FJsonObject>* DestinationAsset = nullptr;
            const TSharedPtr<FJsonObject>* ClassRemaps = nullptr;
            const TSharedPtr<FJsonObject>* DryRun = nullptr;
            const TSharedPtr<FJsonObject>* Confirm = nullptr;
            bHasSourceAsset = ActionInfo.ParamSchema->TryGetObjectField(TEXT("source_asset_path"), SourceAsset) && SourceAsset && SourceAsset->IsValid();
            bHasDestinationAsset = ActionInfo.ParamSchema->TryGetObjectField(TEXT("destination_asset_path"), DestinationAsset) && DestinationAsset && DestinationAsset->IsValid();
            bHasClassRemaps = ActionInfo.ParamSchema->TryGetObjectField(TEXT("class_remaps"), ClassRemaps) && ClassRemaps && ClassRemaps->IsValid();
            FString DefaultValue;
            bDryRunDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("dry_run"), DryRun) && DryRun && DryRun->IsValid()
                && (*DryRun)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("true");
            bConfirmDefault = ActionInfo.ParamSchema->TryGetObjectField(TEXT("confirm"), Confirm) && Confirm && Confirm->IsValid()
                && (*Confirm)->TryGetStringField(TEXT("default"), DefaultValue) && DefaultValue == TEXT("false");
        }
        break;
    }

    TestTrue(TEXT("copy_widget_subtree_with_class_remap registered"), bFoundAction);
    TestTrue(TEXT("copy_widget_subtree_with_class_remap category"), bCategoryOk);
    TestTrue(TEXT("source_asset_path schema exists"), bHasSourceAsset);
    TestTrue(TEXT("destination_asset_path schema exists"), bHasDestinationAsset);
    TestTrue(TEXT("class_remaps schema exists"), bHasClassRemaps);
    TestTrue(TEXT("dry_run defaults true"), bDryRunDefault);
    TestTrue(TEXT("confirm defaults false"), bConfirmDefault);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIWidgetSubtreeCopyRequiresRemapTest,
    "Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresRemap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIWidgetSubtreeCopyRequiresRemapTest::RunTest(const FString& /*Parameters*/)
{
    EnsureWidgetSubtreeCopyActionRegistered();

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        MakeBasicPayload());

    TestFalse(TEXT("copy_widget_subtree_with_class_remap rejects missing remap contract before loading assets"), Result.bSuccess);
    TestTrue(TEXT("missing remap error mentions class/object/root remaps"),
        Result.ErrorMessage.Contains(TEXT("class_remaps")) ||
        Result.ErrorMessage.Contains(TEXT("object_remaps")) ||
        Result.ErrorMessage.Contains(TEXT("root_remaps")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIWidgetSubtreeCopyRequiresConfirmTest,
    "Monolith.ParamGuard.UI.CopyWidgetSubtreeRequiresConfirm",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIWidgetSubtreeCopyRequiresConfirmTest::RunTest(const FString& /*Parameters*/)
{
    EnsureWidgetSubtreeCopyActionRegistered();

    TSharedPtr<FJsonObject> Payload = MakeBasicPayload();
    Payload->SetObjectField(TEXT("class_remaps"), MakeClassRemapObject(TEXT("VerticalBox"), TEXT("HorizontalBox")));
    Payload->SetBoolField(TEXT("dry_run"), false);

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        Payload);

    TestFalse(TEXT("mutating widget subtree copy requires confirm=true"), Result.bSuccess);
    TestTrue(TEXT("confirm guard error is clear"), Result.ErrorMessage.Contains(TEXT("confirm=true")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIWidgetSubtreeCopyDryRunAndLiveSmokeTest,
    "Monolith.UI.WidgetSubtreeCopy.DryRunAndLiveSmoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIWidgetSubtreeCopyDryRunAndLiveSmokeTest::RunTest(const FString& /*Parameters*/)
{
    EnsureWidgetSubtreeCopyActionRegistered();

    const FString SourcePath = TEXT("/Game/Tests/Monolith/UI/SubtreeCopy/WBP_SourceSubtreeCopy");
    const FString DestinationPath = TEXT("/Game/Tests/Monolith/UI/SubtreeCopy/WBP_DestinationSubtreeCopy");

    UWidget* SourceChild = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        SourcePath,
        TEXT("SourcePanel"),
        UVerticalBox::StaticClass(),
        Error,
        &SourceChild))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* SourceWBP = LoadObject<UWidgetBlueprint>(nullptr, *SourcePath);
    TestNotNull(TEXT("Source WBP loaded"), SourceWBP);
    if (!SourceWBP)
    {
        return false;
    }
    if (!AddLabelToSourcePanel(SourceWBP, Error))
    {
        AddError(Error);
        return false;
    }

    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        DestinationPath,
        NAME_None,
        nullptr,
        Error,
        nullptr))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* DestinationWBP = LoadObject<UWidgetBlueprint>(nullptr, *DestinationPath);
    TestNotNull(TEXT("Destination WBP loaded"), DestinationWBP);
    if (!DestinationWBP || !DestinationWBP->WidgetTree)
    {
        return false;
    }

    const bool bWasDirtyBeforeDryRun = DestinationWBP->GetOutermost()->IsDirty();

    TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
    Payload->SetStringField(TEXT("source_asset_path"), SourcePath);
    Payload->SetStringField(TEXT("destination_asset_path"), DestinationPath);
    Payload->SetStringField(TEXT("source_widget_name"), TEXT("SourcePanel"));
    Payload->SetStringField(TEXT("destination_widget_name"), TEXT("CopiedPanel"));
    Payload->SetObjectField(TEXT("class_remaps"), MakeClassRemapObject(TEXT("VerticalBox"), TEXT("HorizontalBox")));

    const FMonolithActionResult DryRunResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        Payload);
    TestTrue(TEXT("dry-run subtree copy succeeds"), DryRunResult.bSuccess);
    TestNull(TEXT("dry-run does not add CopiedPanel"), DestinationWBP->WidgetTree->FindWidget(TEXT("CopiedPanel")));
    TestEqual(TEXT("dry-run preserves destination package dirty flag"), DestinationWBP->GetOutermost()->IsDirty(), bWasDirtyBeforeDryRun);
    if (DryRunResult.Result.IsValid())
    {
        bool bChanged = true;
        TestTrue(TEXT("dry-run result has changed field"), DryRunResult.Result->TryGetBoolField(TEXT("changed"), bChanged));
        TestFalse(TEXT("dry-run changed=false"), bChanged);
    }

    Payload->SetBoolField(TEXT("dry_run"), false);
    Payload->SetBoolField(TEXT("confirm"), true);
    Payload->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult ApplyResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        Payload);
    TestTrue(TEXT("live subtree copy succeeds"), ApplyResult.bSuccess);
    if (!ApplyResult.bSuccess)
    {
        AddError(ApplyResult.ErrorMessage);
        return false;
    }

    UWidget* CopiedPanelWidget = DestinationWBP->WidgetTree->FindWidget(TEXT("CopiedPanel"));
    TestNotNull(TEXT("CopiedPanel exists after live copy"), CopiedPanelWidget);
    UHorizontalBox* CopiedPanel = Cast<UHorizontalBox>(CopiedPanelWidget);
    TestNotNull(TEXT("CopiedPanel uses remapped UHorizontalBox class"), CopiedPanel);
    if (!CopiedPanel)
    {
        return false;
    }

    TestEqual(TEXT("CopiedPanel retains one child"), CopiedPanel->GetChildrenCount(), 1);
    UWidget* CopiedLabel = CopiedPanel->GetChildAt(0);
    TestNotNull(TEXT("Copied label exists"), CopiedLabel);
    TestTrue(TEXT("Copied label remains a TextBlock"), CopiedLabel && CopiedLabel->IsA<UTextBlock>());

    if (ApplyResult.Result.IsValid())
    {
        double CopiedWidgetCount = 0.0;
        double PlannedRemappedWidgetCount = 0.0;
        TestTrue(TEXT("apply result has copied_widget_count"), ApplyResult.Result->TryGetNumberField(TEXT("copied_widget_count"), CopiedWidgetCount));
        TestTrue(TEXT("apply result has planned_remapped_widget_count"), ApplyResult.Result->TryGetNumberField(TEXT("planned_remapped_widget_count"), PlannedRemappedWidgetCount));
        TestTrue(TEXT("two widgets copied"), CopiedWidgetCount >= 2.0);
        TestEqual(TEXT("one widget class remapped"), static_cast<int32>(PlannedRemappedWidgetCount), 1);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
