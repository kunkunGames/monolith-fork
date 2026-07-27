#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshArchFeatureActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenArchFeatureParamTest, "Monolith.ParamGuard.WorldGen.ArchFeatureRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenArchFeatureParamTest::RunTest(const FString& Parameters)
{
    FMonolithMeshArchFeatureActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_railing action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_railing")));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    // Test missing save_path
    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects missing save_path"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports missing save_path"), Result.ErrorMessage.Contains(TEXT("Missing required param: save_path")));

    Params->SetStringField(TEXT("save_path"), TEXT("/Game/TestRailing"));

    // Test missing points
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects missing points"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports missing points"), Result.ErrorMessage.Contains(TEXT("Missing or invalid 'points' array (need at least 2 points as [x,y,z])")));

    // Test malformed points (less than 2)
    TArray<TSharedPtr<FJsonValue>> Points;
    TArray<TSharedPtr<FJsonValue>> Pt1;
    Pt1.Add(MakeShared<FJsonValueNumber>(0));
    Pt1.Add(MakeShared<FJsonValueNumber>(0));
    Pt1.Add(MakeShared<FJsonValueNumber>(0));
    Points.Add(MakeShared<FJsonValueArray>(Pt1));
    Params->SetArrayField(TEXT("points"), Points);

    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects less than 2 points"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports invalid points array"), Result.ErrorMessage.Contains(TEXT("Missing or invalid 'points' array (need at least 2 points as [x,y,z])")));

    // Test point coordinates invalid (not an array of 3 numbers)
    TArray<TSharedPtr<FJsonValue>> Pt2;
    Pt2.Add(MakeShared<FJsonValueNumber>(100));
    Points.Add(MakeShared<FJsonValueArray>(Pt2)); // Only 1 number
    Params->SetArrayField(TEXT("points"), Points);

    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects invalid point format"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports invalid point array length"), Result.ErrorMessage.Contains(TEXT("must be an array of [x,y,z]")));

    // Fix point format but make coordinate non-numeric
    Pt2.Empty();
    Pt2.Add(MakeShared<FJsonValueNumber>(100));
    Pt2.Add(MakeShared<FJsonValueString>(TEXT("invalid")));
    Pt2.Add(MakeShared<FJsonValueNumber>(0));
    Points[1] = MakeShared<FJsonValueArray>(Pt2);
    Params->SetArrayField(TEXT("points"), Points);

    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects non-numeric point coordinate"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports coordinate error"), Result.ErrorMessage.Contains(TEXT("coordinates must be numbers or numeric strings")));

    // Valid points, test invalid style
    Pt2[1] = MakeShared<FJsonValueNumber>(100);
    Points[1] = MakeShared<FJsonValueArray>(Pt2);
    Params->SetArrayField(TEXT("points"), Points);

    Params->SetStringField(TEXT("style"), TEXT("invalid_style"));
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_railing"), Params);
    TestFalse(TEXT("create_railing rejects invalid style"), Result.bSuccess);
    TestTrue(TEXT("create_railing reports style error"), Result.ErrorMessage.Contains(TEXT("Invalid style 'invalid_style'. Valid: simple, bars, solid")));

    return true;
}

#endif // WITH_GEOMETRYSCRIPT
