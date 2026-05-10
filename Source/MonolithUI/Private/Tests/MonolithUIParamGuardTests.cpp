#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithUISettingsActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

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
