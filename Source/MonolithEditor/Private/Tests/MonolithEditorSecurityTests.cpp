#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithEditorMapActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteEditorAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("editor"), Action))
		{
			FMonolithEditorMapActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("editor"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorSecurityPathTest, "Monolith.Security.MonolithEditor.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorSecurityPathTest::RunTest(const FString& Parameters)
{
	// Setup payload with double slash to simulate malformed path
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("path"), TEXT("//Game/MalformedPath/TestMap"));

	// Call the action
	FMonolithActionResult Result = ExecuteEditorAction(TEXT("create_empty_map"), Payload);

	// Verify it failed gracefully and returned the validation error
	TestFalse(TEXT("Action should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
