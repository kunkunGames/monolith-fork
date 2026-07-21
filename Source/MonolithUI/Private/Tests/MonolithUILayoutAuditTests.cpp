// Copyright tumourlove. All Rights Reserved.
// Structural contract tests for ui::audit_widget_layout.

#if WITH_DEV_AUTOMATION_TESTS

#include "Actions/MonolithUISpecActions.h"
#include "MonolithUICommon.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
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

	struct FTestLayoutBounds
	{
		double X = 0.0;
		double Y = 0.0;
		double W = 0.0;
		double H = 0.0;
		bool bValid = false;
	};

	bool TryGetMeasuredBounds(
		const TSharedPtr<FJsonObject>& Result,
		const FString& WidgetName,
		FTestLayoutBounds& OutBounds)
	{
		const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
		if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("profiles"), Profiles) || !Profiles || Profiles->Num() != 1)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> Profile = (*Profiles)[0].IsValid() ? (*Profiles)[0]->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
		if (!Profile.IsValid() || !Profile->TryGetArrayField(TEXT("widgets"), Widgets) || !Widgets)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& WidgetValue : *Widgets)
		{
			const TSharedPtr<FJsonObject> Widget = WidgetValue.IsValid() ? WidgetValue->AsObject() : nullptr;
			FString FoundName;
			if (!Widget.IsValid() || !Widget->TryGetStringField(TEXT("widget_name"), FoundName) || FoundName != WidgetName)
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* Bounds = nullptr;
			if (!Widget->TryGetObjectField(TEXT("layout_bounds"), Bounds) || !Bounds || !Bounds->IsValid())
			{
				return false;
			}

			return (*Bounds)->TryGetNumberField(TEXT("x"), OutBounds.X)
				&& (*Bounds)->TryGetNumberField(TEXT("y"), OutBounds.Y)
				&& (*Bounds)->TryGetNumberField(TEXT("w"), OutBounds.W)
				&& (*Bounds)->TryGetNumberField(TEXT("h"), OutBounds.H)
				&& (*Bounds)->TryGetBoolField(TEXT("valid"), OutBounds.bValid);
		}

		return false;
	}

	UWidgetBlueprint* ResetMeasureFixtureToBox(
		const FString& AssetPath,
		UClass* BoxClass,
		FString& OutError,
		UPanelWidget*& OutRoot)
	{
		OutRoot = nullptr;
		if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
			AssetPath,
			NAME_None,
			nullptr,
			OutError))
		{
			return nullptr;
		}

		UWidgetBlueprint* WidgetBlueprint = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !BoxClass || !BoxClass->IsChildOf(UPanelWidget::StaticClass()))
		{
			OutError = TEXT("Failed to load the measure fixture or resolve its requested box panel class.");
			return nullptr;
		}

		MonolithUI::TestUtils::CleanupWidgetTree(WidgetBlueprint);
		OutRoot = WidgetBlueprint->WidgetTree->ConstructWidget<UPanelWidget>(BoxClass, TEXT("RootBox"));
		if (!OutRoot)
		{
			OutError = TEXT("Failed to construct the requested box panel root.");
			return nullptr;
		}
		WidgetBlueprint->WidgetTree->RootWidget = OutRoot;
		return WidgetBlueprint;
	}

	USizeBox* AddMeasureSizeBox(
		UWidgetBlueprint* WidgetBlueprint,
		UPanelWidget* Parent,
		const FName WidgetName)
	{
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !Parent)
		{
			return nullptr;
		}

		USizeBox* Child = WidgetBlueprint->WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), WidgetName);
		return Child && Parent->AddChild(Child) ? Child : nullptr;
	}

	void CompileMeasureFixture(UWidgetBlueprint* WidgetBlueprint)
	{
		MonolithUI::ReconcileWidgetVariableGuids(WidgetBlueprint);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
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
	FMonolithUIMeasureWidgetLayoutVerticalBoxExactFitTest,
	"Monolith.UI.MeasureWidgetLayout.VerticalBoxExactFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutVerticalBoxExactFitTest::RunTest(const FString& /*Parameters*/)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutVerticalBoxExactFit");
	FString Error;
	UPanelWidget* Root = nullptr;
	UWidgetBlueprint* WidgetBlueprint = ResetMeasureFixtureToBox(AssetPath, UVerticalBox::StaticClass(), Error, Root);
	if (!WidgetBlueprint)
	{
		AddError(Error);
		return false;
	}

	USizeBox* First = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("First"));
	USizeBox* Second = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Second"));
	USizeBox* Third = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Third"));
	if (!TestNotNull(TEXT("first vertical child"), First)
		|| !TestNotNull(TEXT("second vertical child"), Second)
		|| !TestNotNull(TEXT("third vertical child"), Third))
	{
		return false;
	}

	First->SetHeightOverride(40.f);
	Second->SetHeightOverride(30.f);
	Third->SetHeightOverride(115.f);
	UVerticalBoxSlot* FirstSlot = Cast<UVerticalBoxSlot>(First->Slot);
	UVerticalBoxSlot* SecondSlot = Cast<UVerticalBoxSlot>(Second->Slot);
	UVerticalBoxSlot* ThirdSlot = Cast<UVerticalBoxSlot>(Third->Slot);
	if (!TestNotNull(TEXT("first vertical slot"), FirstSlot)
		|| !TestNotNull(TEXT("second vertical slot"), SecondSlot)
		|| !TestNotNull(TEXT("third vertical slot"), ThirdSlot))
	{
		return false;
	}

	const FSlateChildSize Automatic(ESlateSizeRule::Automatic);
	FirstSlot->SetSize(Automatic);
	FirstSlot->SetPadding(FMargin(0.f, 2.f, 0.f, 3.f));
	SecondSlot->SetSize(Automatic);
	SecondSlot->SetPadding(FMargin(0.f, 5.f, 0.f, 5.f));
	ThirdSlot->SetSize(Automatic);
	CompileMeasureFixture(WidgetBlueprint);

	const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(320.f, 200.f));
	if (!TestTrue(TEXT("vertical exact-fit measure returns JSON"), Out.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("vertical exact-fit has no findings"), Out->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("vertical exact-fit has no overlaps"), CountNestedFindings(Out, TEXT("overlaps"), TEXT("WidgetOverlap")), 0);

	FTestLayoutBounds FirstBounds;
	FTestLayoutBounds SecondBounds;
	FTestLayoutBounds ThirdBounds;
	TestTrue(TEXT("first bounds available"), TryGetMeasuredBounds(Out, TEXT("First"), FirstBounds));
	TestTrue(TEXT("second bounds available"), TryGetMeasuredBounds(Out, TEXT("Second"), SecondBounds));
	TestTrue(TEXT("third bounds available"), TryGetMeasuredBounds(Out, TEXT("Third"), ThirdBounds));
	TestTrue(TEXT("first vertical bounds exact"), FirstBounds.bValid
		&& FMath::IsNearlyEqual(FirstBounds.Y, 2.0) && FMath::IsNearlyEqual(FirstBounds.H, 40.0));
	TestTrue(TEXT("second vertical bounds use cumulative prior extent"), SecondBounds.bValid
		&& FMath::IsNearlyEqual(SecondBounds.Y, 50.0) && FMath::IsNearlyEqual(SecondBounds.H, 30.0));
	TestTrue(TEXT("third vertical bounds use unequal cumulative extents"), ThirdBounds.bValid
		&& FMath::IsNearlyEqual(ThirdBounds.Y, 85.0) && FMath::IsNearlyEqual(ThirdBounds.H, 115.0));
	WidgetBlueprint->GetOutermost()->ClearDirtyFlag();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMeasureWidgetLayoutHorizontalBoxExactFitTest,
	"Monolith.UI.MeasureWidgetLayout.HorizontalBoxExactFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutHorizontalBoxExactFitTest::RunTest(const FString& /*Parameters*/)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutHorizontalBoxExactFit");
	FString Error;
	UPanelWidget* Root = nullptr;
	UWidgetBlueprint* WidgetBlueprint = ResetMeasureFixtureToBox(AssetPath, UHorizontalBox::StaticClass(), Error, Root);
	if (!WidgetBlueprint)
	{
		AddError(Error);
		return false;
	}

	USizeBox* First = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("First"));
	USizeBox* Second = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Second"));
	USizeBox* Third = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Third"));
	if (!TestNotNull(TEXT("first horizontal child"), First)
		|| !TestNotNull(TEXT("second horizontal child"), Second)
		|| !TestNotNull(TEXT("third horizontal child"), Third))
	{
		return false;
	}

	First->SetWidthOverride(50.f);
	Second->SetWidthOverride(80.f);
	Third->SetWidthOverride(150.f);
	UHorizontalBoxSlot* FirstSlot = Cast<UHorizontalBoxSlot>(First->Slot);
	UHorizontalBoxSlot* SecondSlot = Cast<UHorizontalBoxSlot>(Second->Slot);
	UHorizontalBoxSlot* ThirdSlot = Cast<UHorizontalBoxSlot>(Third->Slot);
	if (!TestNotNull(TEXT("first horizontal slot"), FirstSlot)
		|| !TestNotNull(TEXT("second horizontal slot"), SecondSlot)
		|| !TestNotNull(TEXT("third horizontal slot"), ThirdSlot))
	{
		return false;
	}

	const FSlateChildSize Automatic(ESlateSizeRule::Automatic);
	FirstSlot->SetSize(Automatic);
	FirstSlot->SetPadding(FMargin(4.f, 0.f, 6.f, 0.f));
	SecondSlot->SetSize(Automatic);
	SecondSlot->SetPadding(FMargin(10.f, 0.f, 0.f, 0.f));
	ThirdSlot->SetSize(Automatic);
	CompileMeasureFixture(WidgetBlueprint);

	const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(300.f, 120.f));
	if (!TestTrue(TEXT("horizontal exact-fit measure returns JSON"), Out.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("horizontal exact-fit has no findings"), Out->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("horizontal exact-fit has no overlaps"), CountNestedFindings(Out, TEXT("overlaps"), TEXT("WidgetOverlap")), 0);

	FTestLayoutBounds FirstBounds;
	FTestLayoutBounds SecondBounds;
	FTestLayoutBounds ThirdBounds;
	TestTrue(TEXT("first bounds available"), TryGetMeasuredBounds(Out, TEXT("First"), FirstBounds));
	TestTrue(TEXT("second bounds available"), TryGetMeasuredBounds(Out, TEXT("Second"), SecondBounds));
	TestTrue(TEXT("third bounds available"), TryGetMeasuredBounds(Out, TEXT("Third"), ThirdBounds));
	TestTrue(TEXT("first horizontal bounds exact"), FirstBounds.bValid
		&& FMath::IsNearlyEqual(FirstBounds.X, 4.0) && FMath::IsNearlyEqual(FirstBounds.W, 50.0));
	TestTrue(TEXT("second horizontal bounds use cumulative prior extent"), SecondBounds.bValid
		&& FMath::IsNearlyEqual(SecondBounds.X, 70.0) && FMath::IsNearlyEqual(SecondBounds.W, 80.0));
	TestTrue(TEXT("third horizontal bounds use unequal cumulative extents"), ThirdBounds.bValid
		&& FMath::IsNearlyEqual(ThirdBounds.X, 150.0) && FMath::IsNearlyEqual(ThirdBounds.W, 150.0));
	WidgetBlueprint->GetOutermost()->ClearDirtyFlag();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMeasureWidgetLayoutBoxMixedAllocationTest,
	"Monolith.UI.MeasureWidgetLayout.BoxMixedAutoFillCollapsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutBoxMixedAllocationTest::RunTest(const FString& /*Parameters*/)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutBoxMixedAllocation");
	FString Error;
	UPanelWidget* Root = nullptr;
	UWidgetBlueprint* WidgetBlueprint = ResetMeasureFixtureToBox(AssetPath, UVerticalBox::StaticClass(), Error, Root);
	if (!WidgetBlueprint)
	{
		AddError(Error);
		return false;
	}

	USizeBox* AutomaticChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Automatic"));
	USizeBox* CollapsedChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Collapsed"));
	USizeBox* FillOneChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("FillOne"));
	USizeBox* FillThreeChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("FillThree"));
	if (!TestNotNull(TEXT("automatic child"), AutomaticChild)
		|| !TestNotNull(TEXT("collapsed child"), CollapsedChild)
		|| !TestNotNull(TEXT("fill-one child"), FillOneChild)
		|| !TestNotNull(TEXT("fill-three child"), FillThreeChild))
	{
		return false;
	}

	AutomaticChild->SetHeightOverride(40.f);
	CollapsedChild->SetHeightOverride(100.f);
	CollapsedChild->SetVisibility(ESlateVisibility::Collapsed);
	UVerticalBoxSlot* AutomaticSlot = Cast<UVerticalBoxSlot>(AutomaticChild->Slot);
	UVerticalBoxSlot* CollapsedSlot = Cast<UVerticalBoxSlot>(CollapsedChild->Slot);
	UVerticalBoxSlot* FillOneSlot = Cast<UVerticalBoxSlot>(FillOneChild->Slot);
	UVerticalBoxSlot* FillThreeSlot = Cast<UVerticalBoxSlot>(FillThreeChild->Slot);
	if (!TestNotNull(TEXT("automatic slot"), AutomaticSlot)
		|| !TestNotNull(TEXT("collapsed slot"), CollapsedSlot)
		|| !TestNotNull(TEXT("fill-one slot"), FillOneSlot)
		|| !TestNotNull(TEXT("fill-three slot"), FillThreeSlot))
	{
		return false;
	}

	const FSlateChildSize Automatic(ESlateSizeRule::Automatic);
	FSlateChildSize FillOne(ESlateSizeRule::Fill);
	FillOne.Value = 1.f;
	FSlateChildSize FillThree(ESlateSizeRule::Fill);
	FillThree.Value = 3.f;
	AutomaticSlot->SetSize(Automatic);
	AutomaticSlot->SetPadding(FMargin(0.f, 5.f, 0.f, 5.f));
	CollapsedSlot->SetSize(Automatic);
	CollapsedSlot->SetPadding(FMargin(0.f, 20.f, 0.f, 20.f));
	FillOneSlot->SetSize(FillOne);
	FillOneSlot->SetPadding(FMargin(0.f, 2.f, 0.f, 3.f));
	FillThreeSlot->SetSize(FillThree);
	FillThreeSlot->SetPadding(FMargin(0.f, 5.f, 0.f, 5.f));
	CompileMeasureFixture(WidgetBlueprint);

	const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(200.f, 300.f));
	if (!TestTrue(TEXT("mixed allocation measure returns JSON"), Out.IsValid()))
	{
		return false;
	}
	TestTrue(TEXT("mixed allocation has no findings"), Out->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("mixed allocation has no overlaps"), CountNestedFindings(Out, TEXT("overlaps"), TEXT("WidgetOverlap")), 0);

	FTestLayoutBounds AutomaticBounds;
	FTestLayoutBounds CollapsedBounds;
	FTestLayoutBounds FillOneBounds;
	FTestLayoutBounds FillThreeBounds;
	TestTrue(TEXT("automatic bounds available"), TryGetMeasuredBounds(Out, TEXT("Automatic"), AutomaticBounds));
	TestTrue(TEXT("collapsed bounds available"), TryGetMeasuredBounds(Out, TEXT("Collapsed"), CollapsedBounds));
	TestTrue(TEXT("fill-one bounds available"), TryGetMeasuredBounds(Out, TEXT("FillOne"), FillOneBounds));
	TestTrue(TEXT("fill-three bounds available"), TryGetMeasuredBounds(Out, TEXT("FillThree"), FillThreeBounds));
	TestTrue(TEXT("automatic child reserves desired size plus padding"), AutomaticBounds.bValid
		&& FMath::IsNearlyEqual(AutomaticBounds.Y, 5.0) && FMath::IsNearlyEqual(AutomaticBounds.H, 40.0));
	TestFalse(TEXT("collapsed child does not expose valid layout bounds"), CollapsedBounds.bValid);
	TestTrue(TEXT("fill-one receives one quarter of remaining content after all visible padding"), FillOneBounds.bValid
		&& FMath::IsNearlyEqual(FillOneBounds.Y, 52.0) && FMath::IsNearlyEqual(FillOneBounds.H, 58.75));
	TestTrue(TEXT("fill-three receives three quarters of remaining content after all visible padding"), FillThreeBounds.bValid
		&& FMath::IsNearlyEqual(FillThreeBounds.Y, 118.75) && FMath::IsNearlyEqual(FillThreeBounds.H, 176.25));
	WidgetBlueprint->GetOutermost()->ClearDirtyFlag();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithUIMeasureWidgetLayoutBoxUnknownAutomaticExtentTest,
	"Monolith.UI.MeasureWidgetLayout.BoxUnknownAutomaticExtentFailsClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMeasureWidgetLayoutBoxUnknownAutomaticExtentTest::RunTest(const FString& /*Parameters*/)
{
	const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_MeasureLayoutBoxUnknownAutomaticExtent");
	FString Error;
	UPanelWidget* Root = nullptr;
	UWidgetBlueprint* WidgetBlueprint = ResetMeasureFixtureToBox(AssetPath, UVerticalBox::StaticClass(), Error, Root);
	if (!WidgetBlueprint)
	{
		AddError(Error);
		return false;
	}

	USizeBox* KnownChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("Known"));
	USizeBox* UnknownDesiredChild = AddMeasureSizeBox(WidgetBlueprint, Root, TEXT("UnknownDesired"));
	if (!TestNotNull(TEXT("known child"), KnownChild)
		|| !TestNotNull(TEXT("unknown desired child"), UnknownDesiredChild))
	{
		return false;
	}

	KnownChild->SetHeightOverride(40.f);
	UVerticalBoxSlot* KnownSlot = Cast<UVerticalBoxSlot>(KnownChild->Slot);
	UVerticalBoxSlot* UnknownDesiredSlot = Cast<UVerticalBoxSlot>(UnknownDesiredChild->Slot);
	if (!TestNotNull(TEXT("known slot"), KnownSlot)
		|| !TestNotNull(TEXT("unknown desired slot"), UnknownDesiredSlot))
	{
		return false;
	}

	const FSlateChildSize Automatic(ESlateSizeRule::Automatic);
	KnownSlot->SetSize(Automatic);
	KnownSlot->SetPadding(FMargin(0.f, 5.f, 0.f, 5.f));
	UnknownDesiredSlot->SetSize(Automatic);
	CompileMeasureFixture(WidgetBlueprint);

	const TSharedPtr<FJsonObject> Out = ExecuteLayoutMeasure(AssetPath, FVector2D(200.f, 100.f));
	if (!TestTrue(TEXT("unknown automatic measure returns JSON"), Out.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("unknown automatic extent fails closed"), Out->GetBoolField(TEXT("bSuccess")));
	TestEqual(TEXT("unknown automatic status"), Out->GetStringField(TEXT("status")), TEXT("measurement_unavailable"));
	TestEqual(TEXT("unknown automatic produces no fabricated overlap"), CountNestedFindings(Out, TEXT("overlaps"), TEXT("WidgetOverlap")), 0);

	FTestLayoutBounds KnownBounds;
	FTestLayoutBounds UnknownBounds;
	TestTrue(TEXT("known bounds available"), TryGetMeasuredBounds(Out, TEXT("Known"), KnownBounds));
	TestTrue(TEXT("unknown row available"), TryGetMeasuredBounds(Out, TEXT("UnknownDesired"), UnknownBounds));
	TestTrue(TEXT("known child remains measured"), KnownBounds.bValid
		&& FMath::IsNearlyEqual(KnownBounds.Y, 5.0) && FMath::IsNearlyEqual(KnownBounds.H, 40.0));
	TestFalse(TEXT("unknown desired size is not reported as valid geometry"), UnknownBounds.bValid);
	WidgetBlueprint->GetOutermost()->ClearDirtyFlag();
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


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIParamGuardMeasureWidgetLayoutTest,
    "Monolith.ParamGuard.UI.MeasureWidgetLayout.MalformedParams",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardMeasureWidgetLayoutTest::RunTest(const FString& /*Parameters*/)
{
    EnsureLayoutAuditActionRegistered();

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), TEXT("/Game/UI/WBP_DummyTest"));
    Params->SetStringField(TEXT("check_overlap"), TEXT("wrong_type_string")); // Malformed

    const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("measure_widget_layout"),
        Params);

    TestFalse(TEXT("Malformed check_overlap should fail"), Result.bSuccess);
    TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    TestTrue(TEXT("Error identifies check_overlap"), Result.ErrorMessage.Contains(TEXT("check_overlap")));

    Params->RemoveField(TEXT("check_overlap"));

    TSharedPtr<FJsonObject> MaxOverlapArray = MakeShared<FJsonObject>(); // Malformed
    Params->SetObjectField(TEXT("max_allowed_overlap_ratio"), MaxOverlapArray);

    const FMonolithActionResult Result2 = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"),
        TEXT("measure_widget_layout"),
        Params);
    TestFalse(TEXT("Malformed max_allowed_overlap_ratio should fail"), Result2.bSuccess);
    TestEqual(TEXT("Error code is invalid params for ratio"), Result2.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
    TestTrue(TEXT("Error identifies max_allowed_overlap_ratio"), Result2.ErrorMessage.Contains(TEXT("max_allowed_overlap_ratio")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
