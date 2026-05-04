#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithAIBlackboardActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAIAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("ai"), Action))
		{
			FMonolithAIBlackboardActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("ai"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAISecurityPathTest, "Monolith.Security.AI.ValidatePackagePath", EAutomationTestFlags::ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FMonolithAISecurityPathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestBlackboard"), // Double leading slash
		TEXT("Game/MalformedPath/TestBlackboard"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestBlackboard/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestBlackboard#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("save_path"), Path);
		Payload->SetStringField(TEXT("name"), TEXT("TestBlackboard"));

		// Call the action
		FMonolithActionResult Result = ExecuteAIAction(TEXT("create_blackboard"), Payload);

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

#endif // WITH_DEV_AUTOMATION_TESTS
