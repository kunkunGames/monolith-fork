#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithUISlotActions.h"
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
