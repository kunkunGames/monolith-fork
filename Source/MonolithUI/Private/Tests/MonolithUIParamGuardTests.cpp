#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithUISettingsActions.h"
#include "MonolithUISlotActions.h"
#include "MonolithUIStylingActions.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Components/Image.h"

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
        TEXT("]")
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
