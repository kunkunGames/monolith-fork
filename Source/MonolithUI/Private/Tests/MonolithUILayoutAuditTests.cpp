// Copyright tumourlove. All Rights Reserved.
// Structural contract tests for ui::audit_widget_layout.

#if WITH_DEV_AUTOMATION_TESTS

#include "Actions/MonolithUISpecActions.h"
#include "MonolithUICommon.h"
#include "MonolithToolRegistry.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "WidgetBlueprint.h"

namespace
{
    void EnsureLayoutAuditActionRegistered()
    {
        FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
        if (!Registry.HasAction(TEXT("ui"), TEXT("audit_widget_layout")))
        {
            MonolithUI::FSpecActions::Register(Registry);
        }
    }

    TSharedPtr<FJsonObject> ExecuteLayoutAudit(const FString& AssetPath, const bool bTreatWarningsAsErrors = false)
    {
        EnsureLayoutAuditActionRegistered();

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> AssetPaths;
        AssetPaths.Add(MakeShared<FJsonValueString>(AssetPath));
        Params->SetArrayField(TEXT("asset_paths"), AssetPaths);
        Params->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"),
            TEXT("audit_widget_layout"),
            Params);
        return Result.Result;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditCanvasAnchorMismatchTest,
    "Monolith.UI.AuditWidgetLayout.CanvasAnchorMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditCanvasAnchorMismatchTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditCanvasAnchorMismatch");
    UWidget* ChildWidget = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        AssetPath,
        TEXT("StageNameText"),
        UTextBlock::StaticClass(),
        Error,
        &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    UTextBlock* StageText = Cast<UTextBlock>(ChildWidget);
    TestNotNull(TEXT("test fixture child is a TextBlock"), StageText);
    if (!StageText)
    {
        return false;
    }

    StageText->SetText(FText::FromString(TEXT("Stage Name")));
    StageText->SetAutoWrapText(false);

    UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(StageText->Slot);
    TestNotNull(TEXT("test fixture child has a CanvasPanelSlot"), CanvasSlot);
    if (!CanvasSlot)
    {
        return false;
    }

    CanvasSlot->SetAnchors(FAnchors(1.f, 0.f, 1.f, 0.f));
    CanvasSlot->SetAlignment(FVector2D(0.f, 0.f));
    CanvasSlot->SetPosition(FVector2D(-128.f, 16.f));
    CanvasSlot->SetSize(FVector2D(180.f, 32.f));
    CanvasSlot->SetAutoSize(false);

    UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("test fixture WBP reloads"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns a JSON payload"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    bool bSuccess = true;
    FString Status;
    TestTrue(TEXT("payload exposes bSuccess"), Out->TryGetBoolField(TEXT("bSuccess"), bSuccess));
    TestTrue(TEXT("payload exposes status"), Out->TryGetStringField(TEXT("status"), Status));
    TestFalse(TEXT("anchor mismatch makes the audit fail"), bSuccess);
    TestEqual(TEXT("failing audit status"), Status, FString(TEXT("findings_failed")));

    const TSharedPtr<FJsonObject>* Summary = nullptr;
    TestTrue(TEXT("payload exposes summary"), Out->TryGetObjectField(TEXT("summary"), Summary));
    if (Summary && Summary->IsValid())
    {
        int32 CanvasSlots = 0;
        int32 ErrorCount = 0;
        (*Summary)->TryGetNumberField(TEXT("canvas_slots"), CanvasSlots);
        (*Summary)->TryGetNumberField(TEXT("error_count"), ErrorCount);
        TestEqual(TEXT("one Canvas slot was scanned"), CanvasSlots, 1);
        TestTrue(TEXT("at least one error was reported"), ErrorCount >= 1);
    }

    const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
    TestTrue(TEXT("payload exposes findings"), Out->TryGetArrayField(TEXT("findings"), Findings));
    bool bFoundAnchorMismatch = false;
    if (Findings)
    {
        for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
        {
            const TSharedPtr<FJsonObject>* Finding = nullptr;
            if (!FindingValue.IsValid() || !FindingValue->TryGetObject(Finding) || !Finding || !Finding->IsValid())
            {
                continue;
            }

            FString Category;
            if ((*Finding)->TryGetStringField(TEXT("category"), Category)
                && Category == TEXT("CanvasAnchorMismatch"))
            {
                bFoundAnchorMismatch = true;
                break;
            }
        }
    }
    TestTrue(TEXT("CanvasAnchorMismatch finding is present"), bFoundAnchorMismatch);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditMinDesiredWidthDoesNotBoundTextTest,
    "Monolith.UI.AuditWidgetLayout.MinDesiredWidthDoesNotBoundText",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditMinDesiredWidthDoesNotBoundTextTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditMinDesiredWidthText");
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        AssetPath,
        NAME_None,
        nullptr,
        Error))
    {
        AddError(Error);
        return false;
    }

    UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("test fixture WBP reloads"), WidgetBlueprint);
    if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
    {
        return false;
    }

    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
    TestNotNull(TEXT("test fixture root is a CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }

    USizeBox* WidthFloor = WidgetBlueprint->WidgetTree->ConstructWidget<USizeBox>(
        USizeBox::StaticClass(),
        TEXT("PromptWidthFloor"));
    UTextBlock* PromptText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(),
        TEXT("PromptText"));
    TestNotNull(TEXT("fixture SizeBox is constructed"), WidthFloor);
    TestNotNull(TEXT("fixture dynamic TextBlock is constructed"), PromptText);
    if (!WidthFloor || !PromptText)
    {
        return false;
    }

    WidthFloor->SetMinDesiredWidth(480.0f);
    PromptText->SetText(FText::FromString(TEXT("Prompt text intentionally has only a minimum width floor and no wrap or max width.")));
    PromptText->SetAutoWrapText(false);
    WidthFloor->AddChild(PromptText);
    RootCanvas->AddChild(WidthFloor);
    if (UCanvasPanelSlot* WrapperSlot = Cast<UCanvasPanelSlot>(WidthFloor->Slot))
    {
        WrapperSlot->SetAutoSize(true);
        WrapperSlot->SetSize(FVector2D::ZeroVector);
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath, true);
    TestTrue(TEXT("audit returns a JSON payload"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
    TestTrue(TEXT("payload exposes findings"), Out->TryGetArrayField(TEXT("findings"), Findings));
    bool bFoundUnboundedDynamicText = false;
    if (Findings)
    {
        for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
        {
            const TSharedPtr<FJsonObject>* Finding = nullptr;
            if (!FindingValue.IsValid() || !FindingValue->TryGetObject(Finding) || !Finding || !Finding->IsValid())
            {
                continue;
            }

            FString Category;
            if ((*Finding)->TryGetStringField(TEXT("category"), Category)
                && Category == TEXT("UnboundedDynamicText"))
            {
                bFoundUnboundedDynamicText = true;
                break;
            }
        }
    }
    TestTrue(TEXT("MinDesiredWidth alone does not suppress UnboundedDynamicText"), bFoundUnboundedDynamicText);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
