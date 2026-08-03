// Copyright tumourlove. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithToolRegistry.h"
#include "MonolithUICommon.h"
#include "MonolithUIWidgetCopyActions.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBox.h"
#include "Components/PanelWidget.h"
#include "Components/RichTextBlock.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
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

        URichTextBlock* RichLabel = SourceWBP->WidgetTree->ConstructWidget<URichTextBlock>(URichTextBlock::StaticClass(), TEXT("RichLabel"));
        if (!RichLabel)
        {
            OutError = TEXT("Failed to construct RichLabel RichTextBlock fixture widget");
            return false;
        }

        RichLabel->SetText(FText::FromString(TEXT("Copied rich label")));
        SourcePanel->AddChild(RichLabel);
        MonolithUI::RegisterCreatedWidget(SourceWBP, RichLabel);
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
    UVerticalBox* SourcePanel = Cast<UVerticalBox>(SourceWBP->WidgetTree->FindWidget(TEXT("SourcePanel")));
    UTextBlock* SourceLabel = Cast<UTextBlock>(SourceWBP->WidgetTree->FindWidget(TEXT("Label")));
    URichTextBlock* SourceRichLabel = Cast<URichTextBlock>(SourceWBP->WidgetTree->FindWidget(TEXT("RichLabel")));
    TestNotNull(TEXT("source panel fixture remains available"), SourcePanel);
    TestNotNull(TEXT("source label fixture remains available"), SourceLabel);
    TestNotNull(TEXT("source rich label fixture remains available"), SourceRichLabel);
    if (!SourcePanel || !SourceLabel || !SourceRichLabel)
    {
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
    TSharedPtr<FJsonObject> ClassRemaps = MakeClassRemapObject(TEXT("VerticalBox"), TEXT("HorizontalBox"));
    ClassRemaps->SetStringField(TEXT("RichTextBlock"), TEXT("TextBlock"));
    Payload->SetObjectField(TEXT("class_remaps"), ClassRemaps);

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
    Payload->SetBoolField(TEXT("save"), true);

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

    TestEqual(TEXT("CopiedPanel retains two children"), CopiedPanel->GetChildrenCount(), 2);
    UWidget* CopiedLabel = CopiedPanel->GetChildAt(0);
    UTextBlock* CopiedRichLabel = Cast<UTextBlock>(CopiedPanel->GetChildAt(1));
    TestNotNull(TEXT("Copied label exists"), CopiedLabel);
    TestTrue(TEXT("Copied label remains a TextBlock"), CopiedLabel && CopiedLabel->IsA<UTextBlock>());
    TestNotNull(TEXT("RichLabel remaps from the larger RichTextBlock class to TextBlock"), CopiedRichLabel);
    TestEqual(TEXT("RichLabel preserves its compatible Text property"),
        CopiedRichLabel ? CopiedRichLabel->GetText().ToString() : FString(),
        FString(TEXT("Copied rich label")));
    TestTrue(TEXT("Copied panel is owned by destination WidgetTree"), CopiedPanel->GetOuter() == DestinationWBP->WidgetTree);
    TestTrue(TEXT("Copied label is owned by destination WidgetTree"), CopiedLabel && CopiedLabel->GetOuter() == DestinationWBP->WidgetTree);
    TestTrue(TEXT("Copied rich label is owned by destination WidgetTree"), CopiedRichLabel && CopiedRichLabel->GetOuter() == DestinationWBP->WidgetTree);
    TestTrue(TEXT("Copied label is a distinct destination object"), CopiedLabel != SourceLabel);
    TestTrue(TEXT("Copied rich label is a distinct destination object"),
        static_cast<const UObject*>(CopiedRichLabel) != static_cast<const UObject*>(SourceRichLabel));

    TestTrue(TEXT("Source panel remains owned by source WidgetTree"), SourcePanel->GetOuter() == SourceWBP->WidgetTree);
    TestTrue(TEXT("Source label remains owned by source WidgetTree"), SourceLabel->GetOuter() == SourceWBP->WidgetTree);
    TestTrue(TEXT("Source label remains attached to source panel"), SourceLabel->GetParent() == SourcePanel);
    TestTrue(TEXT("Source rich label remains attached to source panel"), SourceRichLabel->GetParent() == SourcePanel);
    TestTrue(TEXT("Source label remains discoverable in source WidgetTree"), SourceWBP->WidgetTree->FindWidget(TEXT("Label")) == SourceLabel);
    TestTrue(TEXT("Source rich label remains discoverable in source WidgetTree"), SourceWBP->WidgetTree->FindWidget(TEXT("RichLabel")) == SourceRichLabel);

    UVerticalBoxSlot* SourceRichSlot = Cast<UVerticalBoxSlot>(SourceRichLabel->Slot);
    TestNotNull(TEXT("source rich label has a VerticalBoxSlot before same-asset replacement"), SourceRichSlot);
    if (!SourceRichSlot)
    {
        return false;
    }
    SourceRichSlot->SetPadding(FMargin(3.0f, 4.0f, 5.0f, 6.0f));
    SourceRichSlot->SetHorizontalAlignment(HAlign_Right);
    SourceRichSlot->SetVerticalAlignment(VAlign_Center);
    FSlateChildSize SourceRichSize;
    SourceRichSize.Value = 0.37f;
    SourceRichSize.SizeRule = ESlateSizeRule::Fill;
    SourceRichSlot->SetSize(SourceRichSize);

    TSharedPtr<FJsonObject> SameAssetPayload = MakeShared<FJsonObject>();
    SameAssetPayload->SetStringField(TEXT("source_asset_path"), SourcePath);
    SameAssetPayload->SetStringField(TEXT("destination_asset_path"), SourcePath);
    SameAssetPayload->SetStringField(TEXT("source_widget_name"), TEXT("RichLabel"));
    SameAssetPayload->SetStringField(TEXT("destination_widget_name"), TEXT("RichLabel"));
    SameAssetPayload->SetStringField(TEXT("destination_parent_name"), TEXT("SourcePanel"));
    SameAssetPayload->SetObjectField(TEXT("class_remaps"), MakeClassRemapObject(TEXT("RichTextBlock"), TEXT("TextBlock")));
    SameAssetPayload->SetStringField(TEXT("existing_policy"), TEXT("replace"));
    SameAssetPayload->SetBoolField(TEXT("dry_run"), false);
    SameAssetPayload->SetBoolField(TEXT("confirm"), true);
    SameAssetPayload->SetBoolField(TEXT("compile"), false);
    SameAssetPayload->SetBoolField(TEXT("save"), false);

    const FMonolithActionResult SameAssetResult = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("copy_widget_subtree_with_class_remap"),
        SameAssetPayload);
    TestTrue(TEXT("same-asset class replacement succeeds"), SameAssetResult.bSuccess);
    if (!SameAssetResult.bSuccess)
    {
        AddError(SameAssetResult.ErrorMessage);
        return false;
    }

    UTextBlock* SameAssetRichLabel = Cast<UTextBlock>(SourceWBP->WidgetTree->FindWidget(TEXT("RichLabel")));
    UVerticalBoxSlot* SameAssetRichSlot = SameAssetRichLabel
        ? Cast<UVerticalBoxSlot>(SameAssetRichLabel->Slot)
        : nullptr;
    TestNotNull(TEXT("same-asset replacement uses remapped TextBlock class"), SameAssetRichLabel);
    TestNotNull(TEXT("same-asset replacement retains VerticalBoxSlot"), SameAssetRichSlot);
    TestEqual(TEXT("same-asset replacement preserves Text"),
        SameAssetRichLabel ? SameAssetRichLabel->GetText().ToString() : FString(),
        FString(TEXT("Copied rich label")));
    if (SameAssetRichSlot)
    {
        TestEqual(TEXT("same-asset replacement preserves slot padding"), SameAssetRichSlot->GetPadding(), FMargin(3.0f, 4.0f, 5.0f, 6.0f));
        TestEqual(TEXT("same-asset replacement preserves horizontal alignment"), SameAssetRichSlot->GetHorizontalAlignment(), HAlign_Right);
        TestEqual(TEXT("same-asset replacement preserves vertical alignment"), SameAssetRichSlot->GetVerticalAlignment(), VAlign_Center);
        TestEqual(TEXT("same-asset replacement preserves fill rule"), SameAssetRichSlot->GetSize().SizeRule, ESlateSizeRule::Fill);
        TestEqual(TEXT("same-asset replacement preserves fill weight"), SameAssetRichSlot->GetSize().Value, 0.37f);
    }

    if (ApplyResult.Result.IsValid())
    {
        double CopiedWidgetCount = 0.0;
        double PlannedRemappedWidgetCount = 0.0;
        bool bSaved = false;
        TestTrue(TEXT("apply result has copied_widget_count"), ApplyResult.Result->TryGetNumberField(TEXT("copied_widget_count"), CopiedWidgetCount));
        TestTrue(TEXT("apply result has planned_remapped_widget_count"), ApplyResult.Result->TryGetNumberField(TEXT("planned_remapped_widget_count"), PlannedRemappedWidgetCount));
        TestTrue(TEXT("apply result has saved"), ApplyResult.Result->TryGetBoolField(TEXT("saved"), bSaved));
        TestTrue(TEXT("three widgets copied"), CopiedWidgetCount >= 3.0);
        TestEqual(TEXT("two widget classes remapped"), static_cast<int32>(PlannedRemappedWidgetCount), 2);
        TestTrue(TEXT("destination package saves without source-private references"), bSaved);
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
