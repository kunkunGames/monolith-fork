#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithUISettingsActions.h"
#include "MonolithUISlotActions.h"
#include "MonolithUIStylingActions.h"
#include "Spec/UISpecSerializer.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "WidgetBlueprint.h"

#include <limits>

namespace
{
    bool CreateVerticalBoxSlotFixture(
        const FString& AssetPath,
        FString& OutError,
        UWidgetBlueprint*& OutWBP,
        UImage*& OutChild,
        UVerticalBoxSlot*& OutSlot)
    {
        OutWBP = nullptr;
        OutChild = nullptr;
        OutSlot = nullptr;

        if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
                AssetPath,
                NAME_None,
                UImage::StaticClass(),
                OutError))
        {
            return false;
        }

        OutWBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
        if (!OutWBP || !OutWBP->WidgetTree)
        {
            OutError = TEXT("Failed to load the vertical-box slot test WBP.");
            return false;
        }

        MonolithUI::TestUtils::CleanupWidgetTree(OutWBP);
        UVerticalBox* Root = OutWBP->WidgetTree->ConstructWidget<UVerticalBox>(
            UVerticalBox::StaticClass(),
            TEXT("RootBox"));
        OutChild = OutWBP->WidgetTree->ConstructWidget<UImage>(
            UImage::StaticClass(),
            TEXT("ActionTile"));
        if (!Root || !OutChild)
        {
            OutError = TEXT("Failed to construct the vertical-box slot test widgets.");
            return false;
        }

        OutWBP->WidgetTree->RootWidget = Root;
        OutSlot = Cast<UVerticalBoxSlot>(Root->AddChild(OutChild));
        if (!OutSlot)
        {
            OutError = TEXT("VerticalBox did not create a UVerticalBoxSlot.");
            return false;
        }

        FSlateChildSize InitialSize(ESlateSizeRule::Automatic);
        InitialSize.Value = 1.0f;
        OutSlot->SetSize(InitialSize);
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardScaffoldSaveGame, "Monolith.ParamGuard.MonolithUI.ScaffoldSaveGameRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardScaffoldSaveGame::RunTest(const FString& Parameters)
{
    // Build payload with malformed 'properties' array
    FString JsonPayload = TEXT("{")
        TEXT("\"class_name\": \"UMySaveGame\",")
        TEXT("\"module_name\": \"MyModule\",")
        TEXT("\"properties\": [")
        TEXT("    \"not_an_object\",")
        TEXT("    null,")
        TEXT("    123,")
        TEXT("    { \"name\": \"ValidProp\", \"type\": \"int32\", \"default_value\": \"42\" }")
        TEXT("],")
        TEXT("\"dry_run\": true")
        TEXT("}");

    TSharedPtr<FJsonObject> ParamsObj;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
    if (!FJsonSerializer::Deserialize(Reader, ParamsObj) || !ParamsObj.IsValid())
    {
        AddError(TEXT("Failed to deserialize test JSON payload"));
        return false;
    }

    FMonolithActionResult Result = FMonolithUISettingsActions::HandleScaffoldSaveGame(ParamsObj);

    TestTrue(TEXT("scaffold_save_game did not crash and processed the valid prop"), Result.bSuccess);
    if (Result.Result.IsValid())
    {
        TestEqual(TEXT("Only valid property object is counted"), static_cast<int32>(Result.Result->GetNumberField(TEXT("property_count"))), 1);
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardSetSlotPropertyAnchorsMalformed, "Monolith.ParamGuard.MonolithUI.SetSlotPropertyAnchorsMalformed", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardSetSlotPropertyAnchorsMalformed::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_ParamGuardSetSlotPropertyAnchorsMalformed");
    FString Error;
    UWidget* ChildWidget = nullptr;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, TEXT("PreviewImage"), UImage::StaticClass(), Error, &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    TSharedPtr<FJsonObject> Anchors = MakeShared<FJsonObject>();
    // Missing min_x, max_x, min_y, but has malformed max_y
    Anchors->SetStringField(TEXT("max_y"), TEXT("not_a_number"));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("PreviewImage"));
    Params->SetObjectField(TEXT("anchors"), Anchors);

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleSetSlotProperty(Params);
    TestFalse(TEXT("set_slot_property fails gracefully on malformed anchors"), Result.bSuccess);
    TestTrue(TEXT("set_slot_property returns clear error message"), Result.ErrorMessage.Contains(TEXT("Invalid param")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetSlotPropertyBoxSizeRule,
    "Monolith.UI.SetSlotProperty.BoxSizeRule",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetSlotPropertyBoxSizeRule::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SetSlotPropertyBoxSizeRule");
    FString Error;
    UWidgetBlueprint* WBP = nullptr;
    UImage* Child = nullptr;
    UVerticalBoxSlot* Slot = nullptr;
    if (!CreateVerticalBoxSlotFixture(AssetPath, Error, WBP, Child, Slot))
    {
        AddError(Error);
        return false;
    }
    (void)WBP;

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    Params->SetStringField(TEXT("size_rule"), TEXT("Fill"));
    Params->SetNumberField(TEXT("fill_weight"), 2.5);

    const FMonolithActionResult FillResult = FMonolithUISlotActions::HandleSetSlotProperty(Params);
    if (!TestTrue(TEXT("set_slot_property accepts box size fields"), FillResult.bSuccess && FillResult.Result.IsValid()))
    {
        AddError(FillResult.ErrorMessage);
        return false;
    }

    TestEqual(TEXT("two box size properties applied"), static_cast<int32>(FillResult.Result->GetNumberField(TEXT("properties_set"))), 2);
    TestEqual(TEXT("result reports Fill"), FillResult.Result->GetStringField(TEXT("size_rule")), TEXT("Fill"));
    TestTrue(TEXT("result reports fill weight"), FMath::IsNearlyEqual(FillResult.Result->GetNumberField(TEXT("fill_weight")), 2.5));

    const FSlateChildSize FillSize = Slot->GetSize();
    TestEqual(TEXT("slot uses Fill"), FillSize.SizeRule.GetValue(), ESlateSizeRule::Fill);
    TestTrue(TEXT("slot stores fill weight"), FMath::IsNearlyEqual(FillSize.Value, 2.5f));

    const FUISpecSerializerResult DumpResult = FUISpecSerializer::DumpFromWBP(WBP, AssetPath);
    if (!TestTrue(TEXT("UISpec serializer succeeds after owner action"), DumpResult.bSuccess && DumpResult.Document.Root.IsValid()))
    {
        return false;
    }
    if (!TestEqual(TEXT("UISpec root has ActionTile"), DumpResult.Document.Root->Children.Num(), 1))
    {
        return false;
    }
    const TSharedPtr<FUISpecNode> DumpedChild = DumpResult.Document.Root->Children[0];
    if (!TestTrue(TEXT("serialized ActionTile is valid"), DumpedChild.IsValid()))
    {
        return false;
    }
    TestEqual(TEXT("UISpec readback reports Fill"), DumpedChild->Slot.SizeRule, FName(TEXT("Fill")));
    TestTrue(TEXT("UISpec readback reports fill weight"), FMath::IsNearlyEqual(DumpedChild->Slot.FillWeight, 2.5f));

    TSharedPtr<FJsonObject> AutomaticParams = MakeShared<FJsonObject>();
    AutomaticParams->SetStringField(TEXT("asset_path"), AssetPath);
    AutomaticParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    AutomaticParams->SetStringField(TEXT("size_rule"), TEXT("Automatic"));

    const FMonolithActionResult AutomaticResult = FMonolithUISlotActions::HandleSetSlotProperty(AutomaticParams);
    TestTrue(TEXT("set_slot_property accepts Automatic"), AutomaticResult.bSuccess);
    const FSlateChildSize RuleOnlySize = Slot->GetSize();
    TestEqual(TEXT("rule-only write uses Automatic"), RuleOnlySize.SizeRule.GetValue(), ESlateSizeRule::Automatic);
    TestTrue(TEXT("rule-only write preserves fill weight"), FMath::IsNearlyEqual(RuleOnlySize.Value, 2.5f));

    TSharedPtr<FJsonObject> WeightOnlyParams = MakeShared<FJsonObject>();
    WeightOnlyParams->SetStringField(TEXT("asset_path"), AssetPath);
    WeightOnlyParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    WeightOnlyParams->SetNumberField(TEXT("fill_weight"), 0.75);
    const FMonolithActionResult WeightOnlyResult = FMonolithUISlotActions::HandleSetSlotProperty(WeightOnlyParams);
    TestTrue(TEXT("set_slot_property accepts fill_weight alone"), WeightOnlyResult.bSuccess);
    const FSlateChildSize WeightOnlySize = Slot->GetSize();
    TestEqual(TEXT("weight-only write preserves Automatic"), WeightOnlySize.SizeRule.GetValue(), ESlateSizeRule::Automatic);
    TestTrue(TEXT("weight-only write stores coefficient"), FMath::IsNearlyEqual(WeightOnlySize.Value, 0.75f));

    const FString HorizontalAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SetSlotPropertyHorizontalBoxSizeRule");
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
            HorizontalAssetPath,
            NAME_None,
            UImage::StaticClass(),
            Error))
    {
        AddError(Error);
        return false;
    }
    UWidgetBlueprint* HorizontalWBP = LoadObject<UWidgetBlueprint>(nullptr, *HorizontalAssetPath);
    if (!TestNotNull(TEXT("horizontal fixture WBP loads"), HorizontalWBP))
    {
        return false;
    }
    MonolithUI::TestUtils::CleanupWidgetTree(HorizontalWBP);
    UHorizontalBox* HorizontalRoot = HorizontalWBP->WidgetTree->ConstructWidget<UHorizontalBox>(
        UHorizontalBox::StaticClass(),
        TEXT("HorizontalRoot"));
    UImage* HorizontalChild = HorizontalWBP->WidgetTree->ConstructWidget<UImage>(
        UImage::StaticClass(),
        TEXT("HorizontalActionTile"));
    if (!TestNotNull(TEXT("horizontal root constructed"), HorizontalRoot)
        || !TestNotNull(TEXT("horizontal child constructed"), HorizontalChild))
    {
        return false;
    }
    HorizontalWBP->WidgetTree->RootWidget = HorizontalRoot;
    UHorizontalBoxSlot* HorizontalSlot = Cast<UHorizontalBoxSlot>(HorizontalRoot->AddChild(HorizontalChild));
    if (!TestNotNull(TEXT("horizontal child gets HorizontalBoxSlot"), HorizontalSlot))
    {
        return false;
    }

    TSharedPtr<FJsonObject> HorizontalParams = MakeShared<FJsonObject>();
    HorizontalParams->SetStringField(TEXT("asset_path"), HorizontalAssetPath);
    HorizontalParams->SetStringField(TEXT("widget_name"), TEXT("HorizontalActionTile"));
    HorizontalParams->SetStringField(TEXT("size_rule"), TEXT("Fill"));
    HorizontalParams->SetNumberField(TEXT("fill_weight"), 3.0);
    const FMonolithActionResult HorizontalResult = FMonolithUISlotActions::HandleSetSlotProperty(HorizontalParams);
    TestTrue(TEXT("horizontal box size fields apply"), HorizontalResult.bSuccess);
    const FSlateChildSize HorizontalSize = HorizontalSlot->GetSize();
    TestEqual(TEXT("horizontal slot uses Fill"), HorizontalSize.SizeRule.GetValue(), ESlateSizeRule::Fill);
    TestTrue(TEXT("horizontal slot stores fill weight"), FMath::IsNearlyEqual(HorizontalSize.Value, 3.0f));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUISetSlotPropertyBoxSizeValidation,
    "Monolith.UI.SetSlotProperty.BoxSizeValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUISetSlotPropertyBoxSizeValidation::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SetSlotPropertyBoxSizeValidation");
    FString Error;
    UWidgetBlueprint* WBP = nullptr;
    UImage* Child = nullptr;
    UVerticalBoxSlot* Slot = nullptr;
    if (!CreateVerticalBoxSlotFixture(AssetPath, Error, WBP, Child, Slot))
    {
        AddError(Error);
        return false;
    }
    (void)WBP;
    (void)Child;

    TSharedPtr<FJsonObject> InvalidRuleParams = MakeShared<FJsonObject>();
    InvalidRuleParams->SetStringField(TEXT("asset_path"), AssetPath);
    InvalidRuleParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    InvalidRuleParams->SetStringField(TEXT("size_rule"), TEXT("Stretch"));
    const FMonolithActionResult InvalidRuleResult = FMonolithUISlotActions::HandleSetSlotProperty(InvalidRuleParams);
    TestFalse(TEXT("unknown size rule is rejected"), InvalidRuleResult.bSuccess);
    TestTrue(TEXT("size-rule error lists canonical tokens"), InvalidRuleResult.ErrorMessage.Contains(TEXT("Automatic")) && InvalidRuleResult.ErrorMessage.Contains(TEXT("Fill")));

    TSharedPtr<FJsonObject> NegativeWeightParams = MakeShared<FJsonObject>();
    NegativeWeightParams->SetStringField(TEXT("asset_path"), AssetPath);
    NegativeWeightParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    NegativeWeightParams->SetNumberField(TEXT("fill_weight"), -1.0);
    const FMonolithActionResult NegativeWeightResult = FMonolithUISlotActions::HandleSetSlotProperty(NegativeWeightParams);
    TestFalse(TEXT("negative fill weight is rejected"), NegativeWeightResult.bSuccess);
    TestTrue(TEXT("fill-weight error explains range"), NegativeWeightResult.ErrorMessage.Contains(TEXT("greater than or equal to zero")));

    TSharedPtr<FJsonObject> WrongTypeParams = MakeShared<FJsonObject>();
    WrongTypeParams->SetStringField(TEXT("asset_path"), AssetPath);
    WrongTypeParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    WrongTypeParams->SetStringField(TEXT("fill_weight"), TEXT("heavy"));
    const FMonolithActionResult WrongTypeResult = FMonolithUISlotActions::HandleSetSlotProperty(WrongTypeParams);
    TestFalse(TEXT("non-numeric fill weight is rejected"), WrongTypeResult.bSuccess);

    TSharedPtr<FJsonObject> NonFiniteParams = MakeShared<FJsonObject>();
    NonFiniteParams->SetStringField(TEXT("asset_path"), AssetPath);
    NonFiniteParams->SetStringField(TEXT("widget_name"), TEXT("ActionTile"));
    NonFiniteParams->SetNumberField(TEXT("fill_weight"), std::numeric_limits<double>::infinity());
    const FMonolithActionResult NonFiniteResult = FMonolithUISlotActions::HandleSetSlotProperty(NonFiniteParams);
    TestFalse(TEXT("non-finite fill weight is rejected"), NonFiniteResult.bSuccess);

    const FSlateChildSize UnchangedSize = Slot->GetSize();
    TestEqual(TEXT("failed writes preserve size rule"), UnchangedSize.SizeRule.GetValue(), ESlateSizeRule::Automatic);
    TestTrue(TEXT("failed writes preserve fill weight"), FMath::IsNearlyEqual(UnchangedSize.Value, 1.0f));

    const FString CanvasAssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SetSlotPropertyBoxSizeRejectsCanvas");
    UWidget* CanvasChild = nullptr;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
            CanvasAssetPath,
            TEXT("CanvasChild"),
            UImage::StaticClass(),
            Error,
            &CanvasChild))
    {
        AddError(Error);
        return false;
    }
    (void)CanvasChild;

    TSharedPtr<FJsonObject> CanvasParams = MakeShared<FJsonObject>();
    CanvasParams->SetStringField(TEXT("asset_path"), CanvasAssetPath);
    CanvasParams->SetStringField(TEXT("widget_name"), TEXT("CanvasChild"));
    CanvasParams->SetNumberField(TEXT("fill_weight"), 1.0);
    const FMonolithActionResult CanvasResult = FMonolithUISlotActions::HandleSetSlotProperty(CanvasParams);
    TestFalse(TEXT("CanvasPanelSlot rejects box size fields"), CanvasResult.bSuccess);
    TestTrue(TEXT("slot-class error names compatible slots"), CanvasResult.ErrorMessage.Contains(TEXT("VerticalBoxSlot")) && CanvasResult.ErrorMessage.Contains(TEXT("HorizontalBoxSlot")));
    TestTrue(TEXT("fill-weight-only slot error reports fill_weight path"), CanvasResult.ErrorMessage.Contains(TEXT("json_path: /fill_weight")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardSetImageMissingNumericField, "Monolith.ParamGuard.MonolithUI.SetImageMissingNumericField", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardSetImageMissingNumericField::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_ParamGuardSetImageMissingNumericField");
    FString Error;
    UWidget* ChildWidget = nullptr;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, TEXT("PreviewImage"), UImage::StaticClass(), Error, &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    TSharedPtr<FJsonObject> Size = MakeShared<FJsonObject>();
    Size->SetNumberField(TEXT("x"), 64.0);

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("asset_path"), AssetPath);
    Params->SetStringField(TEXT("widget_name"), TEXT("PreviewImage"));
    Params->SetObjectField(TEXT("size"), Size);

    const FMonolithActionResult Result = FMonolithUIStylingActions::HandleSetImage(Params);
    TestTrue(TEXT("set_image tolerates missing size.y numeric field"), Result.bSuccess);
    int32 PropertiesSet = 0;
    const bool bHasPropertiesSet = Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("properties_set"), PropertiesSet);
    TestTrue(TEXT("properties_set field is present"), bHasPropertiesSet);
    TestEqual(TEXT("set_image applied one property"), PropertiesSet, 1);

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardSetRetainerEffectMaterialMissingParams, "Monolith.ParamGuard.MonolithUI.SetRetainerEffectMaterialMissingParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardSetRetainerEffectMaterialMissingParams::RunTest(const FString& Parameters)
{
    const FMonolithActionResult Result = FMonolithUIStylingActions::HandleSetRetainerEffectMaterial(MakeShared<FJsonObject>());
    TestFalse(TEXT("set_retainer_effect_material rejects missing params"), Result.bSuccess);
    TestTrue(TEXT("set_retainer_effect_material reports a required param"), Result.ErrorMessage.Contains(TEXT("asset_path")) || Result.ErrorMessage.Contains(TEXT("required")));
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardSetSlotGridNumericFields, "Monolith.ParamGuard.MonolithUI.SetSlotGridNumericFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardSetSlotGridNumericFields::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_ParamGuardGridSlot");
    FString Error;
    UWidget* ChildWidget = nullptr;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(AssetPath, TEXT("GridChild"), UImage::StaticClass(), Error, &ChildWidget))
    {
        AddError(Error);
        return false;
    }

    TSharedPtr<FJsonObject> BadRowParams = MakeShared<FJsonObject>();
    BadRowParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadRowParams->SetStringField(TEXT("widget_name"), TEXT("GridChild"));
    BadRowParams->SetStringField(TEXT("row"), TEXT("not_a_number"));

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleSetSlotProperty(BadRowParams);

    TestFalse(TEXT("set_slot_property correctly rejects malformed string for 'row'"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("error message indicates parameter 'row' must be a number"), Result.ErrorMessage.Contains(TEXT("Parameter 'row' must be a number")));
    }

    TSharedPtr<FJsonObject> BadPaddingParams = MakeShared<FJsonObject>();
    BadPaddingParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadPaddingParams->SetStringField(TEXT("widget_name"), TEXT("GridChild"));
    TSharedPtr<FJsonObject> PaddingObj = MakeShared<FJsonObject>();
    PaddingObj->SetStringField(TEXT("left"), TEXT("10px"));
    BadPaddingParams->SetObjectField(TEXT("padding"), PaddingObj);

    const FMonolithActionResult PadResult = FMonolithUISlotActions::HandleSetSlotProperty(BadPaddingParams);

    TestFalse(TEXT("set_slot_property correctly rejects malformed string for padding 'left'"), PadResult.bSuccess);
    if (!PadResult.bSuccess)
    {
        TestTrue(TEXT("error message indicates padding 'left' must be a number"), PadResult.ErrorMessage.Contains(TEXT("Padding 'left' must be a number")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIParamGuardSetSlotMissingNumericField, "Monolith.ParamGuard.MonolithUI.SetSlotMissingNumericField", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIParamGuardSetSlotMissingNumericField::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/WBP_SetSlotMissingNumericField");
    UWidget* Child = nullptr;
    FString Error;
    if (!MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
            AssetPath,
            TEXT("CanvasChild"),
            UImage::StaticClass(),
            Error,
            &Child))
    {
        AddError(Error);
        return false;
    }

    UCanvasPanelSlot* CanvasSlot = Child ? Cast<UCanvasPanelSlot>(Child->Slot) : nullptr;
    if (!TestNotNull(TEXT("Canvas child has a CanvasPanelSlot"), CanvasSlot))
    {
        return false;
    }
    const bool InitialAutoSize = CanvasSlot->GetAutoSize();
    const int32 InitialZOrder = CanvasSlot->GetZOrder();

    TSharedPtr<FJsonObject> BadOffsetsParams = MakeShared<FJsonObject>();
    BadOffsetsParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadOffsetsParams->SetStringField(TEXT("widget_name"), TEXT("CanvasChild"));
    TSharedPtr<FJsonObject> OffsetsObj = MakeShared<FJsonObject>();
    OffsetsObj->SetStringField(TEXT("left"), TEXT("10px"));
    BadOffsetsParams->SetObjectField(TEXT("offsets"), OffsetsObj);

    const FMonolithActionResult Result = FMonolithUISlotActions::HandleSetSlotProperty(BadOffsetsParams);

    TestFalse(TEXT("set_slot_property correctly rejects malformed string for offsets 'left'"), Result.bSuccess);
    if (!Result.bSuccess)
    {
        TestTrue(TEXT("error message indicates offsets.left must be a number"), Result.ErrorMessage.Contains(TEXT("offsets.left must be a number")));
    }

    TSharedPtr<FJsonObject> BadZOrderParams = MakeShared<FJsonObject>();
    BadZOrderParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadZOrderParams->SetStringField(TEXT("widget_name"), TEXT("CanvasChild"));
    BadZOrderParams->SetStringField(TEXT("z_order"), TEXT("10px"));

    const FMonolithActionResult ZOrderResult = FMonolithUISlotActions::HandleSetSlotProperty(BadZOrderParams);

    TestFalse(TEXT("set_slot_property correctly rejects malformed string for z_order"), ZOrderResult.bSuccess);
    if (!ZOrderResult.bSuccess)
    {
        TestTrue(TEXT("error message indicates z_order must be a number"), ZOrderResult.ErrorMessage.Contains(TEXT("z_order must be a number")));
    }

    TSharedPtr<FJsonObject> BadAutoSizeParams = MakeShared<FJsonObject>();
    BadAutoSizeParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadAutoSizeParams->SetStringField(TEXT("widget_name"), TEXT("CanvasChild"));
    BadAutoSizeParams->SetStringField(TEXT("auto_size"), TEXT("yes"));

    const FMonolithActionResult AutoSizeResult = FMonolithUISlotActions::HandleSetSlotProperty(BadAutoSizeParams);

    TestFalse(TEXT("set_slot_property correctly rejects malformed string for auto_size"), AutoSizeResult.bSuccess);
    if (!AutoSizeResult.bSuccess)
    {
        TestTrue(
            TEXT("error message indicates auto_size must be a boolean"),
            AutoSizeResult.ErrorMessage.Contains(TEXT("Parameter 'auto_size' must be a boolean")));
    }
    TestEqual(
        TEXT("malformed auto_size is rejected before slot mutation"),
        CanvasSlot->GetAutoSize(),
        InitialAutoSize);

    TSharedPtr<FJsonObject> BadCompileParams = MakeShared<FJsonObject>();
    BadCompileParams->SetStringField(TEXT("asset_path"), AssetPath);
    BadCompileParams->SetStringField(TEXT("widget_name"), TEXT("CanvasChild"));
    BadCompileParams->SetNumberField(TEXT("z_order"), InitialZOrder + 1);
    BadCompileParams->SetStringField(TEXT("compile"), TEXT("yes"));

    const FMonolithActionResult CompileResult =
        FMonolithUISlotActions::HandleSetSlotProperty(BadCompileParams);
    TestFalse(TEXT("set_slot_property rejects malformed string for compile"), CompileResult.bSuccess);
    if (!CompileResult.bSuccess)
    {
        TestTrue(
            TEXT("error message indicates compile must be a boolean"),
            CompileResult.ErrorMessage.Contains(TEXT("Parameter 'compile' must be a boolean")));
    }
    TestEqual(
        TEXT("malformed compile is rejected before any requested slot mutation"),
        CanvasSlot->GetZOrder(),
        InitialZOrder);

    return true;
}
