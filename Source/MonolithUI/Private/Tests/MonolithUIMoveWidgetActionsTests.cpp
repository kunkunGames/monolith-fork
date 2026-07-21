#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithUISlotActions.h"
#include "MonolithUIMoveWidgetTransaction.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "WidgetBlueprint.h"

namespace
{
    static UWidgetBlueprint* LoadTestWidgetBlueprint(const FString& AssetPath)
    {
        return LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    }

    static bool AlmostEqual(double A, double B)
    {
        return FMath::IsNearlyEqual(A, B, 0.001);
    }

    static void RegisterWidgetGuid(UWidgetBlueprint* WBP, UWidget* Widget)
    {
        if (WBP && Widget && !WBP->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
        {
            WBP->WidgetVariableNameToGuidMap.Add(Widget->GetFName(), FGuid::NewGuid());
        }
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetRollsBackTargetAddFailure,
    "Monolith.UI.MoveWidget.RollsBackTargetAddFailure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetRollsBackTargetAddFailure::RunTest(const FString& Parameters)
{
    (void)Parameters;
    UCanvasPanel* SourcePanel = NewObject<UCanvasPanel>();
    UCanvasPanel* TargetPanel = NewObject<UCanvasPanel>();
    UImage* Before = NewObject<UImage>();
    UImage* Badge = NewObject<UImage>();
    UImage* After = NewObject<UImage>();
    if (!TestNotNull(TEXT("source panel exists"), SourcePanel)
        || !TestNotNull(TEXT("target panel exists"), TargetPanel)
        || !TestNotNull(TEXT("leading sibling exists"), Before)
        || !TestNotNull(TEXT("moved widget exists"), Badge)
        || !TestNotNull(TEXT("trailing sibling exists"), After))
    {
        return false;
    }

    SourcePanel->AddChild(Before);
    UCanvasPanelSlot* OriginalSlot = Cast<UCanvasPanelSlot>(SourcePanel->AddChild(Badge));
    SourcePanel->AddChild(After);
    if (!TestNotNull(TEXT("moved widget starts in a canvas slot"), OriginalSlot))
    {
        return false;
    }
    const FAnchors OriginalAnchors(0.25f, 0.5f, 0.75f, 0.5f);
    OriginalSlot->SetAnchors(OriginalAnchors);

    bool bRestoreCallbackInvoked = false;
    const MonolithUI::MoveWidgetTransaction::FResult Result =
        MonolithUI::MoveWidgetTransaction::MoveCrossParent(
            *SourcePanel,
            *TargetPanel,
            *Badge,
            1,
            true,
            0,
            [](UPanelWidget&, UWidget&) -> UPanelSlot*
            {
                return nullptr;
            },
            [&bRestoreCallbackInvoked, OriginalAnchors](UPanelSlot& RestoredSlot, int32 RestoredIndex)
            {
                bRestoreCallbackInvoked = true;
                if (UCanvasPanelSlot* RestoredCanvasSlot = Cast<UCanvasPanelSlot>(&RestoredSlot))
                {
                    RestoredCanvasSlot->SetAnchors(OriginalAnchors);
                    return RestoredIndex == 1;
                }
                return false;
            });

    TestFalse(TEXT("target AddChild failure does not report success"), Result.bSucceeded);
    TestTrue(TEXT("target AddChild failure reports the failing stage"),
        Result.FailureReason.Contains(TEXT("target slot creation failed")));
    TestTrue(TEXT("post-removal failure attempts rollback"), Result.bRollbackAttempted);
    TestTrue(TEXT("parent, index, and slot-state rollback succeeds"), Result.bRollbackSucceeded);
    TestTrue(TEXT("slot-state restoration participates in the transaction"), bRestoreCallbackInvoked);
    TestEqual(TEXT("source child count is restored"), SourcePanel->GetChildrenCount(), 3);
    TestEqual(TEXT("leading sibling remains first"), SourcePanel->GetChildAt(0), static_cast<UWidget*>(Before));
    TestEqual(TEXT("moved widget returns to its original index"), SourcePanel->GetChildAt(1), static_cast<UWidget*>(Badge));
    TestEqual(TEXT("trailing sibling remains last"), SourcePanel->GetChildAt(2), static_cast<UWidget*>(After));
    TestEqual(TEXT("failed target remains empty"), TargetPanel->GetChildrenCount(), 0);
    TestEqual(TEXT("transaction reports the restored index"), Result.RestoredIndex, 1);
    UCanvasPanelSlot* RestoredSlot = Cast<UCanvasPanelSlot>(Badge->Slot);
    TestNotNull(TEXT("rollback creates a valid source slot"), RestoredSlot);
    if (RestoredSlot)
    {
        TestTrue(TEXT("compatible source slot state is restored"),
            RestoredSlot->GetAnchors() == OriginalAnchors);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetPreservesCanvasSlot,
    "Monolith.UI.MoveWidget.PreservesCanvasSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetPreservesCanvasSlot::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MoveWidgetPreservesCanvasSlot");
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, NAME_None, UImage::StaticClass(), Error))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("test WBP loads"), WBP) || !TestNotNull(TEXT("widget tree exists"), WBP ? WBP->WidgetTree.Get() : nullptr))
    {
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("root canvas exists"), Root))
    {
        return false;
    }

    UCanvasPanel* SourcePanel = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("SourcePanel"));
    UCanvasPanel* TargetPanel = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("TargetPanel"));
    UImage* Badge = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Badge"));
    if (!TestNotNull(TEXT("source panel constructed"), SourcePanel)
        || !TestNotNull(TEXT("target panel constructed"), TargetPanel)
        || !TestNotNull(TEXT("badge image constructed"), Badge))
    {
        return false;
    }

    Root->AddChild(SourcePanel);
    Root->AddChild(TargetPanel);
    UCanvasPanelSlot* BadgeSlot = Cast<UCanvasPanelSlot>(SourcePanel->AddChild(Badge));
    if (!TestNotNull(TEXT("badge has source canvas slot"), BadgeSlot))
    {
        return false;
    }

    const FAnchors ExpectedAnchors(0.25f, 0.5f, 0.25f, 0.5f);
    const FMargin ExpectedOffsets(32.0f, 48.0f, 160.0f, 36.0f);
    const FVector2D ExpectedAlignment(0.5f, 0.25f);
    const int32 ExpectedZOrder = 7;
    BadgeSlot->SetAnchors(ExpectedAnchors);
    BadgeSlot->SetOffsets(ExpectedOffsets);
    BadgeSlot->SetAlignment(ExpectedAlignment);
    BadgeSlot->SetAutoSize(false);
    BadgeSlot->SetZOrder(ExpectedZOrder);
    RegisterWidgetGuid(WBP, SourcePanel);
    RegisterWidgetGuid(WBP, TargetPanel);
    RegisterWidgetGuid(WBP, Badge);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Badge"));
    Params->SetStringField(TEXT("new_parent_name"), TEXT("TargetPanel"));
    Params->SetBoolField(TEXT("compile"), true);

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleMoveWidget(Params);
    if (!TestTrue(TEXT("move_widget succeeds"), Result.bSuccess) || !TestTrue(TEXT("move_widget has result payload"), Result.Result.IsValid()))
    {
        if (!Result.ErrorMessage.IsEmpty())
        {
            AddError(Result.ErrorMessage);
        }
        return false;
    }

    TestEqual(TEXT("operation source"), Result.Result->GetStringField(TEXT("operation_source")), TEXT("monolith_equivalent"));
    const TSharedPtr<FJsonObject>* Preservation = nullptr;
    if (!TestTrue(TEXT("slot preservation object present"), Result.Result->TryGetObjectField(TEXT("slot_preservation"), Preservation) && Preservation && Preservation->IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("slot preservation status"), (*Preservation)->GetStringField(TEXT("status")), TEXT("preserved"));
    TestEqual(TEXT("old slot type"), (*Preservation)->GetStringField(TEXT("old_slot_type")), TEXT("CanvasPanelSlot"));
    TestEqual(TEXT("new slot type"), (*Preservation)->GetStringField(TEXT("new_slot_type")), TEXT("CanvasPanelSlot"));

    int32 ParentIndex = -1;
    UPanelWidget* ActualParent = UWidgetTree::FindWidgetParent(Badge, ParentIndex);
    TestEqual(TEXT("badge moved to target panel"), ActualParent, static_cast<UPanelWidget*>(TargetPanel));

    UCanvasPanelSlot* NewBadgeSlot = Cast<UCanvasPanelSlot>(Badge->Slot);
    if (!TestNotNull(TEXT("badge keeps canvas slot after move"), NewBadgeSlot))
    {
        return false;
    }

    const FAnchors ActualAnchors = NewBadgeSlot->GetAnchors();
    const FMargin ActualOffsets = NewBadgeSlot->GetOffsets();
    const FVector2D ActualAlignment = NewBadgeSlot->GetAlignment();
    TestTrue(TEXT("anchor min x preserved"), AlmostEqual(ActualAnchors.Minimum.X, ExpectedAnchors.Minimum.X));
    TestTrue(TEXT("anchor min y preserved"), AlmostEqual(ActualAnchors.Minimum.Y, ExpectedAnchors.Minimum.Y));
    TestTrue(TEXT("anchor max x preserved"), AlmostEqual(ActualAnchors.Maximum.X, ExpectedAnchors.Maximum.X));
    TestTrue(TEXT("anchor max y preserved"), AlmostEqual(ActualAnchors.Maximum.Y, ExpectedAnchors.Maximum.Y));
    TestTrue(TEXT("offset left preserved"), AlmostEqual(ActualOffsets.Left, ExpectedOffsets.Left));
    TestTrue(TEXT("offset top preserved"), AlmostEqual(ActualOffsets.Top, ExpectedOffsets.Top));
    TestTrue(TEXT("offset right preserved"), AlmostEqual(ActualOffsets.Right, ExpectedOffsets.Right));
    TestTrue(TEXT("offset bottom preserved"), AlmostEqual(ActualOffsets.Bottom, ExpectedOffsets.Bottom));
    TestTrue(TEXT("alignment x preserved"), AlmostEqual(ActualAlignment.X, ExpectedAlignment.X));
    TestTrue(TEXT("alignment y preserved"), AlmostEqual(ActualAlignment.Y, ExpectedAlignment.Y));
    TestEqual(TEXT("z order preserved"), NewBadgeSlot->GetZOrder(), ExpectedZOrder);
    TestFalse(TEXT("auto-size preserved"), NewBadgeSlot->GetAutoSize());

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetRejectsFullSingleChildParent,
    "Monolith.UI.MoveWidget.RejectsFullSingleChildParent",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetRejectsFullSingleChildParent::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MoveWidgetRejectsFullSingleChildParent");
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, NAME_None, UImage::StaticClass(), Error))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("test WBP loads"), WBP) || !TestNotNull(TEXT("widget tree exists"), WBP ? WBP->WidgetTree.Get() : nullptr))
    {
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("root canvas exists"), Root))
    {
        return false;
    }

    UButton* ButtonHost = WBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ButtonHost"));
    UTextBlock* ExistingText = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ExistingText"));
    UImage* Badge = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Badge"));
    if (!TestNotNull(TEXT("button host constructed"), ButtonHost)
        || !TestNotNull(TEXT("existing text constructed"), ExistingText)
        || !TestNotNull(TEXT("badge image constructed"), Badge))
    {
        return false;
    }

    Root->AddChild(ButtonHost);
    Root->AddChild(Badge);
    ButtonHost->AddChild(ExistingText);
    RegisterWidgetGuid(WBP, ButtonHost);
    RegisterWidgetGuid(WBP, ExistingText);
    RegisterWidgetGuid(WBP, Badge);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    int32 OriginalIndex = -1;
    UPanelWidget* OriginalParent = UWidgetTree::FindWidgetParent(Badge, OriginalIndex);
    TestEqual(TEXT("badge starts on root canvas"), OriginalParent, static_cast<UPanelWidget*>(Root));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("Badge"));
    Params->SetStringField(TEXT("new_parent_name"), TEXT("ButtonHost"));
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleMoveWidget(Params);
    TestFalse(TEXT("move_widget rejects full single-child parent"), Result.bSuccess);
    TestTrue(TEXT("error explains child capacity"), Result.ErrorMessage.Contains(TEXT("cannot accept another child")));

    int32 ParentIndexAfter = -1;
    UPanelWidget* ParentAfter = UWidgetTree::FindWidgetParent(Badge, ParentIndexAfter);
    TestEqual(TEXT("badge remains on original parent after rejection"), ParentAfter, OriginalParent);
    TestEqual(TEXT("button still has one child"), ButtonHost->GetChildrenCount(), 1);
    TestEqual(TEXT("button keeps existing child"), ButtonHost->GetChildAt(0), static_cast<UWidget*>(ExistingText));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetRejectsInvalidSiblingTargetsBeforeMutation,
    "Monolith.UI.MoveWidget.RejectsInvalidSiblingTargetsBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetRejectsInvalidSiblingTargetsBeforeMutation::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MoveWidgetRejectsInvalidSiblingTargets");
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, NAME_None, UImage::StaticClass(), Error))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("test WBP loads"), WBP) || !TestNotNull(TEXT("widget tree exists"), WBP ? WBP->WidgetTree.Get() : nullptr))
    {
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("root canvas exists"), Root))
    {
        return false;
    }

    UImage* First = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("First"));
    UImage* Second = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Second"));
    UTextBlock* NonPanelTarget = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("NonPanelTarget"));
    if (!TestNotNull(TEXT("first image constructed"), First)
        || !TestNotNull(TEXT("second image constructed"), Second)
        || !TestNotNull(TEXT("non-panel target constructed"), NonPanelTarget))
    {
        return false;
    }

    Root->AddChild(First);
    Root->AddChild(Second);
    Root->AddChild(NonPanelTarget);
    RegisterWidgetGuid(WBP, First);
    RegisterWidgetGuid(WBP, Second);
    RegisterWidgetGuid(WBP, NonPanelTarget);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    UPanelSlot* const OriginalFirstSlot = First->Slot;
    UPanelSlot* const OriginalSecondSlot = Second->Slot;

    TSharedPtr<FJsonObject> InvalidIndexParams = MakeShared<FJsonObject>();
    InvalidIndexParams->SetStringField(TEXT("asset_path"), AssetPath);
    InvalidIndexParams->SetStringField(TEXT("widget_name"), TEXT("First"));
    InvalidIndexParams->SetStringField(TEXT("new_parent_name"), TEXT("RootCanvas"));
    InvalidIndexParams->SetNumberField(TEXT("sibling_index"), 3);
    InvalidIndexParams->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult InvalidIndexResult = FMonolithUISlotActions::HandleMoveWidget(InvalidIndexParams);
    TestFalse(TEXT("out-of-range sibling index fails"), InvalidIndexResult.bSuccess);
    TestTrue(TEXT("out-of-range error names valid range"), InvalidIndexResult.ErrorMessage.Contains(TEXT("valid range")));
    TestEqual(TEXT("invalid index leaves first child in place"), Root->GetChildAt(0), static_cast<UWidget*>(First));
    TestEqual(TEXT("invalid index leaves second child in place"), Root->GetChildAt(1), static_cast<UWidget*>(Second));
    TestEqual(TEXT("invalid index preserves first slot instance"), First->Slot.Get(), OriginalFirstSlot);
    TestEqual(TEXT("invalid index preserves second slot instance"), Second->Slot.Get(), OriginalSecondSlot);

    TSharedPtr<FJsonObject> NonPanelParams = MakeShared<FJsonObject>();
    NonPanelParams->SetStringField(TEXT("asset_path"), AssetPath);
    NonPanelParams->SetStringField(TEXT("widget_name"), TEXT("First"));
    NonPanelParams->SetStringField(TEXT("new_parent_name"), TEXT("NonPanelTarget"));
    NonPanelParams->SetNumberField(TEXT("sibling_index"), 0);
    NonPanelParams->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult NonPanelResult = FMonolithUISlotActions::HandleMoveWidget(NonPanelParams);
    TestFalse(TEXT("non-panel parent fails"), NonPanelResult.bSuccess);
    TestTrue(TEXT("non-panel error identifies panel contract"), NonPanelResult.ErrorMessage.Contains(TEXT("not a UPanelWidget")));
    TestEqual(TEXT("non-panel rejection leaves first child in place"), Root->GetChildAt(0), static_cast<UWidget*>(First));
    TestEqual(TEXT("non-panel rejection leaves second child in place"), Root->GetChildAt(1), static_cast<UWidget*>(Second));
    TestEqual(TEXT("non-panel rejection preserves first slot instance"), First->Slot.Get(), OriginalFirstSlot);
    TestEqual(TEXT("non-panel rejection preserves second slot instance"), Second->Slot.Get(), OriginalSecondSlot);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMoveWidgetRejectsCyclicParentBeforeMutation,
    "Monolith.UI.MoveWidget.RejectsCyclicParentBeforeMutation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMoveWidgetRejectsCyclicParentBeforeMutation::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MoveWidgetRejectsCyclicParent");
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, NAME_None, UImage::StaticClass(), Error))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* WBP = LoadTestWidgetBlueprint(AssetPath);
    if (!TestNotNull(TEXT("test WBP loads"), WBP) || !TestNotNull(TEXT("widget tree exists"), WBP ? WBP->WidgetTree.Get() : nullptr))
    {
        return false;
    }

    UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("root canvas exists"), Root))
    {
        return false;
    }

    UCanvasPanel* ParentPanel = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("ParentPanel"));
    UCanvasPanel* DescendantPanel = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("DescendantPanel"));
    if (!TestNotNull(TEXT("parent panel constructed"), ParentPanel)
        || !TestNotNull(TEXT("descendant panel constructed"), DescendantPanel))
    {
        return false;
    }

    Root->AddChild(ParentPanel);
    ParentPanel->AddChild(DescendantPanel);
    RegisterWidgetGuid(WBP, ParentPanel);
    RegisterWidgetGuid(WBP, DescendantPanel);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    FKismetEditorUtilities::CompileBlueprint(WBP);

    UPanelSlot* const OriginalParentSlot = ParentPanel->Slot;
    UPanelSlot* const OriginalDescendantSlot = DescendantPanel->Slot;
    int32 OriginalRootIndex = INDEX_NONE;
    int32 OriginalDescendantIndex = INDEX_NONE;
    TestEqual(TEXT("parent starts under root"), UWidgetTree::FindWidgetParent(ParentPanel, OriginalRootIndex), static_cast<UPanelWidget*>(Root));
    TestEqual(TEXT("descendant starts under parent"), UWidgetTree::FindWidgetParent(DescendantPanel, OriginalDescendantIndex), static_cast<UPanelWidget*>(ParentPanel));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("ParentPanel"));
    Params->SetStringField(TEXT("new_parent_name"), TEXT("DescendantPanel"));
    Params->SetBoolField(TEXT("compile"), false);

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleMoveWidget(Params);
    TestFalse(TEXT("cyclic parent move fails"), Result.bSuccess);
    TestTrue(TEXT("cycle rejection explains hierarchy"), Result.ErrorMessage.Contains(TEXT("descendant")));

    int32 RootIndexAfter = INDEX_NONE;
    int32 DescendantIndexAfter = INDEX_NONE;
    TestEqual(TEXT("parent remains under root after rejection"), UWidgetTree::FindWidgetParent(ParentPanel, RootIndexAfter), static_cast<UPanelWidget*>(Root));
    TestEqual(TEXT("descendant remains under parent after rejection"), UWidgetTree::FindWidgetParent(DescendantPanel, DescendantIndexAfter), static_cast<UPanelWidget*>(ParentPanel));
    TestEqual(TEXT("parent index unchanged"), RootIndexAfter, OriginalRootIndex);
    TestEqual(TEXT("descendant index unchanged"), DescendantIndexAfter, OriginalDescendantIndex);
    TestEqual(TEXT("parent slot instance preserved"), ParentPanel->Slot.Get(), OriginalParentSlot);
    TestEqual(TEXT("descendant slot instance preserved"), DescendantPanel->Slot.Get(), OriginalDescendantSlot);

    return true;
}
