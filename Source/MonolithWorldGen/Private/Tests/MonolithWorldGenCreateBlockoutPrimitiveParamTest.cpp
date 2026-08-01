#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_GEOMETRYSCRIPT

#include "MonolithMeshBlockoutActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithWorldGenCreateBlockoutPrimitiveParamTest, "Monolith.ParamGuard.WorldGen.CreateBlockoutPrimitiveRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithWorldGenCreateBlockoutPrimitiveParamTest::RunTest(const FString& Parameters)
{
    FMonolithMeshBlockoutActions::RegisterActions(FMonolithToolRegistry::Get());
    TestTrue(TEXT("create_blockout_primitive action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("worldgen"), TEXT("create_blockout_primitive")));

    TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

    // Test missing shape
    FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects missing shape"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports missing shape"), Result.ErrorMessage.Contains(TEXT("Missing required param: shape")));

    // Test invalid shape
    Params->SetStringField(TEXT("shape"), TEXT("invalid_shape"));
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects invalid shape"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports invalid shape"), Result.ErrorMessage.Contains(TEXT("Invalid shape: 'invalid_shape'. Valid: box, cylinder, sphere, cone, wedge")));

    // Test missing location
    Params->SetStringField(TEXT("shape"), TEXT("box"));
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects missing location"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports missing location"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param: location (array of 3 numbers)")));

    // Test invalid location (not enough elements)
    TArray<TSharedPtr<FJsonValue>> BadLocation;
    BadLocation.Add(MakeShared<FJsonValueNumber>(0));
    Params->SetArrayField(TEXT("location"), BadLocation);
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects invalid location"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports missing location"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param: location (array of 3 numbers)")));

    // Set valid location, test missing scale
    TArray<TSharedPtr<FJsonValue>> ValidLocation;
    ValidLocation.Add(MakeShared<FJsonValueNumber>(0));
    ValidLocation.Add(MakeShared<FJsonValueNumber>(0));
    ValidLocation.Add(MakeShared<FJsonValueNumber>(0));
    Params->SetArrayField(TEXT("location"), ValidLocation);
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects missing scale"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports missing scale"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param: scale (array of 3 numbers)")));

    // Test invalid scale (not enough elements)
    TArray<TSharedPtr<FJsonValue>> BadScale;
    BadScale.Add(MakeShared<FJsonValueNumber>(1));
    Params->SetArrayField(TEXT("scale"), BadScale);
    Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("worldgen"), TEXT("create_blockout_primitive"), Params);
    TestFalse(TEXT("create_blockout_primitive rejects invalid scale"), Result.bSuccess);
    TestTrue(TEXT("create_blockout_primitive reports missing scale"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param: scale (array of 3 numbers)")));

    return true;
}

#endif // WITH_GEOMETRYSCRIPT
