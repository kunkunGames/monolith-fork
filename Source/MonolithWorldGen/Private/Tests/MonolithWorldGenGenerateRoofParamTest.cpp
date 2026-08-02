#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshRoofActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenGenerateRoofParamTest, "Monolith.ParamGuard.WorldGen.GenerateRoofRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenGenerateRoofParamTest::RunTest(const FString& Parameters)
{
    FMonolithMeshRoofActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("generate_roof action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("generate_roof")));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    // Test footprint_polygon checks
    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
    TestFalse(TEXT("generate_roof rejects missing footprint_polygon"), Result.bSuccess);
    TestTrue(TEXT("generate_roof reports footprint_polygon requires at least 3 points"), Result.ErrorMessage.Contains(TEXT("footprint_polygon requires at least 3 [x,y] points")));

    TArray<TSharedPtr<FJsonValue>> FootprintArr;
    Params->SetArrayField(TEXT("footprint_polygon"), FootprintArr);
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
    TestFalse(TEXT("generate_roof rejects empty footprint_polygon"), Result.bSuccess);
    TestTrue(TEXT("generate_roof reports footprint_polygon requires at least 3 points"), Result.ErrorMessage.Contains(TEXT("footprint_polygon requires at least 3 [x,y] points")));

    // Build a valid footprint
    for (int32 i = 0; i < 3; ++i)
    {
        TArray<TSharedPtr<FJsonValue>> Pair;
        Pair.Add(MakeShared<FJsonValueNumber>(i));
        Pair.Add(MakeShared<FJsonValueNumber>(i));
        FootprintArr.Add(MakeShared<FJsonValueArray>(Pair));
    }
    Params->SetArrayField(TEXT("footprint_polygon"), FootprintArr);

    // Test missing save_path
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
    TestFalse(TEXT("generate_roof rejects missing save_path"), Result.bSuccess);
    TestTrue(TEXT("generate_roof reports save_path is required"), Result.ErrorMessage.Contains(TEXT("save_path is required")));

    // Test pitch_degrees wrong type
    Params->SetStringField(TEXT("save_path"), TEXT("/Game/Test/SM_TestRoof"));
    Params->SetStringField(TEXT("pitch_degrees"), TEXT("thirty")); // String instead of number
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("generate_roof"), Params);
    TestFalse(TEXT("generate_roof rejects string pitch_degrees"), Result.bSuccess);
    TestTrue(TEXT("generate_roof reports pitch_degrees must be a number"), Result.ErrorMessage.Contains(TEXT("pitch_degrees must be a number")));

    return true;
}

#endif
