// Copyright tumourlove. All Rights Reserved.
// Structural contract tests for ui::audit_widget_layout.

#if WITH_DEV_AUTOMATION_TESTS

#include "Actions/MonolithUISpecActions.h"
#include "MonolithUICommon.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithToolRegistry.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetMaterialLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

namespace
{
    void EnsureLayoutAuditActionRegistered()
    {
        FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
        if (!Registry.HasAction(TEXT("ui"), TEXT("audit_widget_layout"))
            || !Registry.HasAction(TEXT("ui"), TEXT("measure_widget_layout"))
            || !Registry.HasAction(TEXT("ui"), TEXT("audit_widget_material_lifecycle")))
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

    TSharedPtr<FJsonObject> ExecuteMaterialLifecycleAudit(
        const FString& AssetPath,
        const bool bIncludeAdvisory = true,
        const bool bTreatWarningsAsErrors = false)
    {
        EnsureLayoutAuditActionRegistered();

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetBoolField(TEXT("include_advisory"), bIncludeAdvisory);
        Params->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);

        const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"),
            TEXT("audit_widget_material_lifecycle"),
            Params);
        return Result.Result;
    }

    UEdGraphPin* FindExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin
                && Pin->Direction == Direction
                && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    bool CreateUnsavedMaterialLifecycleFixture(const FString& AssetPath, FString& OutError, UWidgetBlueprint*& OutWBP)
    {
        OutWBP = nullptr;

        FString PackagePath, AssetName;
        if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName,
            ESearchCase::IgnoreCase, ESearchDir::FromEnd) || AssetName.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Cannot split asset_path '%s'"), *AssetPath);
            return false;
        }

        if (const FString ValidationError = MonolithCore::ValidatePackagePath(AssetPath); !ValidationError.IsEmpty())
        {
            OutError = ValidationError;
            return false;
        }

        UPackage* Package = CreatePackage(*AssetPath);
        if (!Package)
        {
            OutError = FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath);
            return false;
        }

        UWidgetBlueprint* WBP = FindObject<UWidgetBlueprint>(Package, *AssetName);
        if (!WBP)
        {
            UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
            Factory->BlueprintType = BPTYPE_Normal;
            Factory->ParentClass = UUserWidget::StaticClass();
            WBP = Cast<UWidgetBlueprint>(Factory->FactoryCreateNew(
                UWidgetBlueprint::StaticClass(),
                Package,
                FName(*AssetName),
                RF_Public | RF_Standalone,
                nullptr,
                GWarn));
        }

        if (!WBP || !WBP->WidgetTree)
        {
            OutError = TEXT("Failed to construct unsaved test WBP");
            return false;
        }

        MonolithUI::TestUtils::CleanupWidgetTree(WBP);

        UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
        WBP->WidgetTree->RootWidget = Root;
        UImage* Image = WBP->WidgetTree->ConstructWidget<UImage>(
            UImage::StaticClass(), TEXT("DynamicBrushImage"));
        if (!Root || !Image)
        {
            OutError = TEXT("Failed to construct fixture root/Image widgets");
            return false;
        }
        Root->AddChild(Image);

        if (WBP->UbergraphPages.Num() == 0)
        {
            UEdGraph* EventGraph = FBlueprintEditorUtils::CreateNewGraph(
                WBP,
                TEXT("EventGraph"),
                UEdGraph::StaticClass(),
                UEdGraphSchema_K2::StaticClass());
            FBlueprintEditorUtils::AddUbergraphPage(WBP, EventGraph);
        }

        for (UEdGraph* Graph : WBP->UbergraphPages)
        {
            if (!Graph)
            {
                continue;
            }
            TArray<UEdGraphNode*> Nodes = Graph->Nodes;
            for (UEdGraphNode* Node : Nodes)
            {
                if (Node)
                {
                    Graph->RemoveNode(Node);
                }
            }
        }

        MonolithUI::ReconcileWidgetVariableGuids(WBP);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        Package->MarkPackageDirty();
        OutWBP = WBP;
        return true;
    }

    bool AddTickDynamicMaterialCreationNode(UWidgetBlueprint* WBP, FString& OutError)
    {
        if (!WBP || WBP->UbergraphPages.Num() == 0 || !WBP->UbergraphPages[0])
        {
            OutError = TEXT("Fixture WBP has no EventGraph");
            return false;
        }

        UEdGraph* Graph = WBP->UbergraphPages[0];
        UK2Node_Event* TickEvent = NewObject<UK2Node_Event>(Graph);
        TickEvent->EventReference.SetExternalMember(TEXT("Tick"), UUserWidget::StaticClass());
        TickEvent->bOverrideFunction = true;
        TickEvent->NodePosX = 0;
        TickEvent->NodePosY = 0;
        Graph->AddNode(TickEvent, true, false);
        TickEvent->AllocateDefaultPins();
        TickEvent->CreateNewGuid();

        UFunction* CreateDynamicMaterialFn = UKismetMaterialLibrary::StaticClass()->FindFunctionByName(
            GET_FUNCTION_NAME_CHECKED(UKismetMaterialLibrary, CreateDynamicMaterialInstance));
        if (!CreateDynamicMaterialFn)
        {
            OutError = TEXT("UKismetMaterialLibrary::CreateDynamicMaterialInstance function not found");
            return false;
        }

        UK2Node_CallFunction* CreateMidCall = NewObject<UK2Node_CallFunction>(Graph);
        CreateMidCall->SetFromFunction(CreateDynamicMaterialFn);
        CreateMidCall->NodePosX = 260;
        CreateMidCall->NodePosY = 0;
        Graph->AddNode(CreateMidCall, true, false);
        CreateMidCall->AllocateDefaultPins();
        CreateMidCall->CreateNewGuid();

        UEdGraphPin* EventExec = FindExecPin(TickEvent, EGPD_Output);
        UEdGraphPin* CallExec = FindExecPin(CreateMidCall, EGPD_Input);
        if (!EventExec || !CallExec)
        {
            OutError = TEXT("Failed to find exec pins for Tick -> CreateDynamicMaterialInstance test graph");
            return false;
        }
        EventExec->MakeLinkTo(CallExec);

        FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
        return true;
    }

    TSharedPtr<FJsonObject> ExecuteLayoutMeasure(
        const FString& AssetPath,
        const FVector2D& Resolution,
        const TSharedPtr<FJsonObject>& SafeZone = nullptr)
    {
        EnsureLayoutAuditActionRegistered();

        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("asset_path"), AssetPath);
        Params->SetBoolField(TEXT("check_overlap"), true);
        Params->SetBoolField(TEXT("check_safe_zone"), SafeZone.IsValid());

        TSharedPtr<FJsonObject> Profile = MakeShared<FJsonObject>();
        Profile->SetStringField(TEXT("name"), TEXT("test"));
        TArray<TSharedPtr<FJsonValue>> ResolutionArray;
        ResolutionArray.Add(MakeShared<FJsonValueNumber>(Resolution.X));
        ResolutionArray.Add(MakeShared<FJsonValueNumber>(Resolution.Y));
        Profile->SetArrayField(TEXT("resolution"), ResolutionArray);
        if (SafeZone.IsValid())
        {
            Profile->SetObjectField(TEXT("safe_zone"), SafeZone);
        }

        TArray<TSharedPtr<FJsonValue>> Profiles;
        Profiles.Add(MakeShared<FJsonValueObject>(Profile));
        Params->SetArrayField(TEXT("profiles"), Profiles);

        const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
            TEXT("ui"),
            TEXT("measure_widget_layout"),
            Params);
        return Result.Result;
    }

    int32 CountNestedFindings(
        const TSharedPtr<FJsonObject>& Result,
        const FString& FieldName,
        const FString& Category)
    {
        if (!Result.IsValid())
        {
            return 0;
        }

        const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
        if (!Result->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles)
        {
            return 0;
        }

        int32 Count = 0;
        for (const TSharedPtr<FJsonValue>& ProfileValue : *Profiles)
        {
            const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
            if (!Profile.IsValid())
            {
                continue;
            }

            const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
            if (!Profile->TryGetArrayField(FieldName, Findings) || !Findings)
            {
                continue;
            }

            for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
            {
                const TSharedPtr<FJsonObject> Finding = FindingValue.IsValid() ? FindingValue->AsObject() : nullptr;
                FString FoundCategory;
                if (Finding.IsValid()
                    && Finding->TryGetStringField(TEXT("category"), FoundCategory)
                    && FoundCategory == Category)
                {
                    ++Count;
                }
            }
        }
        return Count;
    }

    int32 CountTopLevelFindings(const TSharedPtr<FJsonObject>& Result, const FString& Category)
    {
        if (!Result.IsValid())
        {
            return 0;
        }

        const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
        if (!Result->TryGetArrayField(TEXT("findings"), Findings) || !Findings)
        {
            return 0;
        }

        int32 Count = 0;
        for (const TSharedPtr<FJsonValue>& FindingValue : *Findings)
        {
            const TSharedPtr<FJsonObject> Finding = FindingValue.IsValid() ? FindingValue->AsObject() : nullptr;
            FString FoundCategory;
            if (Finding.IsValid()
                && Finding->TryGetStringField(TEXT("category"), FoundCategory)
                && FoundCategory == Category)
            {
                ++Count;
            }
        }
        return Count;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditOneChildCanvasWrapperTest,
    "Monolith.UI.AuditWidgetLayout.OneChildCanvasWrapper",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditOneChildCanvasWrapperTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditOneChildCanvasWrapper");
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
    TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }
    RootCanvas->ClearChildren();

    UCanvasPanel* NestedCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(
        UCanvasPanel::StaticClass(),
        TEXT("NestedCanvas"));
    UTextBlock* PromptText = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(
        UTextBlock::StaticClass(),
        TEXT("NestedPrompt"));
    TestNotNull(TEXT("nested CanvasPanel"), NestedCanvas);
    TestNotNull(TEXT("nested prompt"), PromptText);
    if (!NestedCanvas || !PromptText)
    {
        return false;
    }

    NestedCanvas->AddChild(PromptText);
    RootCanvas->AddChild(NestedCanvas);

    if (UCanvasPanelSlot* NestedSlot = Cast<UCanvasPanelSlot>(NestedCanvas->Slot))
    {
        NestedSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
        NestedSlot->SetPosition(FVector2D(120.f, 80.f));
        NestedSlot->SetSize(FVector2D(240.f, 96.f));
        NestedSlot->SetZOrder(0);
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("nested one-child Canvas wrapper is reported"),
        CountTopLevelFindings(Out, TEXT("OneChildCanvasWrapper")) >= 1);
    TestTrue(TEXT("nested Canvas overuse is reported"),
        CountTopLevelFindings(Out, TEXT("CanvasOveruse")) >= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditDecorativeHitTestBlockerTest,
    "Monolith.UI.AuditWidgetLayout.DecorativeHitTestBlocker",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditDecorativeHitTestBlockerTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditDecorativeHitTestBlocker");
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
    TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }
    RootCanvas->ClearChildren();

    UButton* ConfirmButton = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(
        UButton::StaticClass(),
        TEXT("ConfirmButton"));
    UImage* BlockingPlate = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(),
        TEXT("BlockingPlate"));
    TestNotNull(TEXT("interactive button"), ConfirmButton);
    TestNotNull(TEXT("blocking image"), BlockingPlate);
    if (!ConfirmButton || !BlockingPlate)
    {
        return false;
    }

    RootCanvas->AddChild(ConfirmButton);
    RootCanvas->AddChild(BlockingPlate);

    if (UCanvasPanelSlot* ButtonSlot = Cast<UCanvasPanelSlot>(ConfirmButton->Slot))
    {
        ButtonSlot->SetPosition(FVector2D(100.f, 100.f));
        ButtonSlot->SetSize(FVector2D(220.f, 64.f));
        ButtonSlot->SetZOrder(0);
    }
    if (UCanvasPanelSlot* PlateSlot = Cast<UCanvasPanelSlot>(BlockingPlate->Slot))
    {
        PlateSlot->SetPosition(FVector2D(96.f, 96.f));
        PlateSlot->SetSize(FVector2D(240.f, 80.f));
        PlateSlot->SetZOrder(1);
    }
    BlockingPlate->SetVisibility(ESlateVisibility::Visible);

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("decorative hit-test blocker is reported"),
        CountTopLevelFindings(Out, TEXT("DecorativeHitTestBlocker")) >= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditHiddenInteractiveSpaceTest,
    "Monolith.UI.AuditWidgetLayout.HiddenInteractiveSpace",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditHiddenInteractiveSpaceTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditHiddenInteractiveSpace");
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
    TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }
    RootCanvas->ClearChildren();

    UButton* HiddenButton = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(
        UButton::StaticClass(),
        TEXT("HiddenActionButton"));
    TestNotNull(TEXT("hidden button"), HiddenButton);
    if (!HiddenButton)
    {
        return false;
    }

    RootCanvas->AddChild(HiddenButton);
    HiddenButton->SetVisibility(ESlateVisibility::Hidden);
    if (UCanvasPanelSlot* ButtonSlot = Cast<UCanvasPanelSlot>(HiddenButton->Slot))
    {
        ButtonSlot->SetPosition(FVector2D(180.f, 140.f));
        ButtonSlot->SetSize(FVector2D(180.f, 48.f));
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("hidden interactive layout space is reported"),
        CountTopLevelFindings(Out, TEXT("HiddenInteractiveSpace")) >= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditEdgeUiMissingSafeZoneTest,
    "Monolith.UI.AuditWidgetLayout.EdgeUiMissingSafeZone",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditEdgeUiMissingSafeZoneTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditEdgeUiMissingSafeZone");
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
    TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }
    RootCanvas->ClearChildren();

    UImage* EdgePrompt = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(),
        TEXT("EdgePrompt"));
    TestNotNull(TEXT("edge prompt"), EdgePrompt);
    if (!EdgePrompt)
    {
        return false;
    }

    RootCanvas->AddChild(EdgePrompt);
    if (UCanvasPanelSlot* EdgeSlot = Cast<UCanvasPanelSlot>(EdgePrompt->Slot))
    {
        EdgeSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
        EdgeSlot->SetPosition(FVector2D(0.f, 0.f));
        EdgeSlot->SetSize(FVector2D(96.f, 48.f));
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("edge UI without SafeZone ancestry is reported"),
        CountTopLevelFindings(Out, TEXT("EdgeUiMissingSafeZone")) >= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditUnstyledInteractiveStateTest,
    "Monolith.UI.AuditWidgetLayout.UnstyledInteractiveState",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUILayoutAuditMaterialDomainMismatchTest,
    "Monolith.UI.AuditWidgetLayout.MaterialDomainMismatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUILayoutAuditUnstyledInteractiveStateTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditUnstyledInteractiveState");
    UWidget* ChildWidget = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        AssetPath,
        TEXT("PrimaryActionButton"),
        UButton::StaticClass(),
        Error,
        &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    UButton* PrimaryActionButton = Cast<UButton>(ChildWidget);
    TestNotNull(TEXT("test fixture child is a Button"), PrimaryActionButton);
    if (!PrimaryActionButton)
    {
        return false;
    }

    if (UCanvasPanelSlot* ButtonSlot = Cast<UCanvasPanelSlot>(PrimaryActionButton->Slot))
    {
        ButtonSlot->SetPosition(FVector2D(220.f, 160.f));
        ButtonSlot->SetSize(FVector2D(220.f, 64.f));
    }

    UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("test fixture WBP reloads"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("unstyled button-like interactive state is reported"),
        CountTopLevelFindings(Out, TEXT("UnstyledInteractiveState")) >= 1);
    return true;
}

bool FMonolithUILayoutAuditMaterialDomainMismatchTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_LayoutAuditMaterialDomainMismatch");
    UWidget* ChildWidget = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        AssetPath,
        TEXT("SurfaceMaterialImage"),
        UImage::StaticClass(),
        Error,
        &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    UImage* SurfaceMaterialImage = Cast<UImage>(ChildWidget);
    TestNotNull(TEXT("test fixture child is an Image"), SurfaceMaterialImage);
    if (!SurfaceMaterialImage)
    {
        return false;
    }

    const FString SurfaceMaterialPath = TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial");
    UMaterialInterface* SurfaceMaterial = LoadObject<UMaterialInterface>(nullptr, *SurfaceMaterialPath);
    TestNotNull(TEXT("surface-domain material fixture loads"), SurfaceMaterial);
    if (!SurfaceMaterial)
    {
        return false;
    }

    SurfaceMaterialImage->SetBrushFromMaterial(SurfaceMaterial);
    if (UCanvasPanelSlot* ImageSlot = Cast<UCanvasPanelSlot>(SurfaceMaterialImage->Slot))
    {
        ImageSlot->SetPosition(FVector2D(64.f, 96.f));
        ImageSlot->SetSize(FVector2D(256.f, 128.f));
    }

    UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("test fixture WBP reloads"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutAudit(AssetPath);
    TestTrue(TEXT("audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("surface-domain UMG brush material is reported"),
        CountTopLevelFindings(Out, TEXT("MaterialDomainMismatch")) >= 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMeasureWidgetLayoutCanvasOverlapTest,
    "Monolith.UI.MeasureWidgetLayout.CanvasOverlap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutCanvasOverlapTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutCanvasOverlap");
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
    TestNotNull(TEXT("fixture root is CanvasPanel"), RootCanvas);
    if (!RootCanvas)
    {
        return false;
    }

    UImage* Back = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("BackPlate"));
    UImage* Front = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("FrontPlate"));
    TestNotNull(TEXT("back image"), Back);
    TestNotNull(TEXT("front image"), Front);
    if (!Back || !Front)
    {
        return false;
    }
    RootCanvas->AddChild(Back);
    RootCanvas->AddChild(Front);

    if (UCanvasPanelSlot* BackSlot = Cast<UCanvasPanelSlot>(Back->Slot))
    {
        BackSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
        BackSlot->SetPosition(FVector2D(100.f, 100.f));
        BackSlot->SetSize(FVector2D(200.f, 100.f));
    }
    if (UCanvasPanelSlot* FrontSlot = Cast<UCanvasPanelSlot>(Front->Slot))
    {
        FrontSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
        FrontSlot->SetPosition(FVector2D(150.f, 120.f));
        FrontSlot->SetSize(FVector2D(200.f, 100.f));
    }

    MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(800.f, 600.f));
    TestTrue(TEXT("measure returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestFalse(TEXT("overlap makes measure fail"), Out->GetBoolField(TEXT("bSuccess")));
    TestEqual(TEXT("one overlap finding"), CountNestedFindings(Out, TEXT("overlaps"), TEXT("WidgetOverlap")), 1);
    TestEqual(TEXT("schema version"), Out->GetStringField(TEXT("schema_version")), TEXT("ui_layout_measure.v1"));
    TestFalse(TEXT("does not claim render geometry proof"), Out->GetBoolField(TEXT("render_geometry_proof")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMeasureWidgetLayoutSafeZoneTest,
    "Monolith.UI.MeasureWidgetLayout.SafeZoneViolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutSafeZoneTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutSafeZone");
    UWidget* ChildWidget = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
        AssetPath,
        TEXT("EdgePrompt"),
        UImage::StaticClass(),
        Error,
        &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    UImage* EdgePrompt = Cast<UImage>(ChildWidget);
    TestNotNull(TEXT("fixture child is image"), EdgePrompt);
    if (!EdgePrompt)
    {
        return false;
    }

    if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(EdgePrompt->Slot))
    {
        CanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 0.f));
        CanvasSlot->SetPosition(FVector2D(0.f, 0.f));
        CanvasSlot->SetSize(FVector2D(96.f, 48.f));
    }

    UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
    TestNotNull(TEXT("test fixture WBP reloads"), WidgetBlueprint);
    if (!WidgetBlueprint)
    {
        return false;
    }
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
    FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

    TSharedPtr<FJsonObject> SafeZone = MakeShared<FJsonObject>();
    SafeZone->SetNumberField(TEXT("left"), 32.0);
    SafeZone->SetNumberField(TEXT("top"), 24.0);
    SafeZone->SetNumberField(TEXT("right"), 32.0);
    SafeZone->SetNumberField(TEXT("bottom"), 24.0);

    const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(1280.f, 720.f), SafeZone);
    TestTrue(TEXT("measure returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestFalse(TEXT("safe-zone violation makes measure fail"), Out->GetBoolField(TEXT("bSuccess")));
    TestEqual(TEXT("one safe-zone violation"), CountNestedFindings(Out, TEXT("safe_zone_violations"), TEXT("SafeZoneViolation")), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMaterialLifecycleAuditCleanWidgetTest,
    "Monolith.UI.AuditWidgetMaterialLifecycle.CleanWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMaterialLifecycleAuditCleanWidgetTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MaterialLifecycleAuditClean");
    FString Error;
    UWidgetBlueprint* WidgetBlueprint = nullptr;
    if (!CreateUnsavedMaterialLifecycleFixture(AssetPath, Error, WidgetBlueprint))
    {
        AddError(Error);
        return false;
    }

    TestNotNull(TEXT("unsaved clean fixture WBP exists"), WidgetBlueprint);
    const TSharedPtr<FJsonObject> Out = ExecuteMaterialLifecycleAudit(AssetPath, /*bIncludeAdvisory=*/true);
    TestTrue(TEXT("material lifecycle audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestTrue(TEXT("clean widget passes material lifecycle audit"), Out->GetBoolField(TEXT("bSuccess")));
    TestEqual(TEXT("material lifecycle schema"), Out->GetStringField(TEXT("schema_version")), TEXT("ui_material_lifecycle_audit.v1"));
    const TSharedPtr<FJsonObject>* Summary = nullptr;
    TestTrue(TEXT("material lifecycle summary exists"), Out->TryGetObjectField(TEXT("summary"), Summary));
    if (Summary && Summary->IsValid())
    {
        int32 DmiCount = -1;
        int32 ErrorCount = -1;
        (*Summary)->TryGetNumberField(TEXT("dynamic_material_creation_count"), DmiCount);
        (*Summary)->TryGetNumberField(TEXT("error_count"), ErrorCount);
        TestEqual(TEXT("clean widget has no DMI creation calls"), DmiCount, 0);
        TestEqual(TEXT("clean widget has no lifecycle errors"), ErrorCount, 0);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIMaterialLifecycleAuditTickDynamicMaterialTest,
    "Monolith.UI.AuditWidgetMaterialLifecycle.TickDynamicMaterial",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMaterialLifecycleAuditTickDynamicMaterialTest::RunTest(const FString& /*Parameters*/)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MaterialLifecycleAuditTickDmi");
    FString Error;
    UWidgetBlueprint* WidgetBlueprint = nullptr;
    if (!CreateUnsavedMaterialLifecycleFixture(AssetPath, Error, WidgetBlueprint))
    {
        AddError(Error);
        return false;
    }
    if (!AddTickDynamicMaterialCreationNode(WidgetBlueprint, Error))
    {
        AddError(Error);
        return false;
    }

    const TSharedPtr<FJsonObject> Out = ExecuteMaterialLifecycleAudit(AssetPath, /*bIncludeAdvisory=*/false);
    TestTrue(TEXT("material lifecycle audit returns JSON"), Out.IsValid());
    if (!Out.IsValid())
    {
        return false;
    }

    TestFalse(TEXT("Tick-created MID fails material lifecycle audit"), Out->GetBoolField(TEXT("bSuccess")));
    TestEqual(TEXT("failing material lifecycle status"), Out->GetStringField(TEXT("status")), TEXT("findings_failed"));
    TestTrue(TEXT("repeated lifecycle DMI finding exists"),
        CountTopLevelFindings(Out, TEXT("DynamicMaterialCreatedInRepeatedLifecycle")) >= 1);

    const TSharedPtr<FJsonObject>* Summary = nullptr;
    TestTrue(TEXT("material lifecycle summary exists"), Out->TryGetObjectField(TEXT("summary"), Summary));
    if (Summary && Summary->IsValid())
    {
        int32 DmiCount = 0;
        int32 ErrorCount = 0;
        (*Summary)->TryGetNumberField(TEXT("dynamic_material_creation_count"), DmiCount);
        (*Summary)->TryGetNumberField(TEXT("error_count"), ErrorCount);
        TestTrue(TEXT("at least one DMI creation call scanned"), DmiCount >= 1);
        TestTrue(TEXT("at least one lifecycle error reported"), ErrorCount >= 1);
    }
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
