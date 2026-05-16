#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithGASAbilityActions.h"
#include "MonolithGASCueActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    FMonolithActionResult ExecuteGASAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
    {
        FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
        if (!Registry.HasAction(TEXT("gas"), Action))
        {
            FMonolithGASAbilityActions::RegisterActions(Registry);
            FMonolithGASCueActions::RegisterActions(Registry);
        }

        return Registry.ExecuteAction(TEXT("gas"), Action, Params);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASSecurityPathTest, "Monolith.Security.GAS.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASSecurityPathTest::RunTest(const FString& Parameters)
{
    TArray<FString> MalformedPaths = {
        TEXT(""), // Empty path
        TEXT("//Game/MalformedPath/TestAbility"), // Double leading slash
        TEXT("Game/MalformedPath/TestAbility"), // Missing leading slash
        TEXT("/Game/MalformedPath/TestAbility/"), // Trailing slash
        TEXT("/Game/MalformedPath/TestAbility#Invalid") // Illegal characters
    };

    for (const FString& Path : MalformedPaths)
    {
        // Setup payload to simulate malformed path
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("save_path"), Path);

        // Call the action
        FMonolithActionResult Result = ExecuteGASAction(TEXT("create_ability"), Payload);

        // Verify it failed gracefully and returned the validation error
        TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
        if (!Path.IsEmpty())
        {
            TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path for: %s"), *Path),
                Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
                Result.ErrorMessage.Contains(TEXT("Package path")) ||
                Result.ErrorMessage.Contains(Path));
        }
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASCueSecurityPathTest, "Monolith.Security.GAS.ValidatePackagePath.Cue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASCueSecurityPathTest::RunTest(const FString& Parameters)
{
    TArray<FString> MalformedPaths = {
        TEXT(""), // Empty path
        TEXT("//Game/MalformedPath/GC_Test"), // Double leading slash
        TEXT("Game/MalformedPath/GC_Test"), // Missing leading slash
        TEXT("/Game/MalformedPath/GC_Test/"), // Trailing slash
        TEXT("/Game/MalformedPath/GC_Test#Invalid") // Illegal characters
    };

    for (const FString& Path : MalformedPaths)
    {
        // Setup payload to simulate malformed path
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("save_path"), Path);
        Payload->SetStringField(TEXT("cue_tag"), TEXT("GameplayCue.Monolith.Test"));

        // Call the registered cue action so the test reaches ValidatePackagePath.
        FMonolithActionResult Result = ExecuteGASAction(TEXT("create_gameplay_cue_notify"), Payload);

        // Verify it failed gracefully and returned the validation error
        TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
        TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
        if (!Path.IsEmpty())
        {
            TestTrue(*FString::Printf(TEXT("Error should complain about invalid cue save_path for: %s"), *Path),
                Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
                Result.ErrorMessage.Contains(TEXT("Package path")) ||
                Result.ErrorMessage.Contains(TEXT("save_path")) ||
                Result.ErrorMessage.Contains(Path));
        }
    }

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
