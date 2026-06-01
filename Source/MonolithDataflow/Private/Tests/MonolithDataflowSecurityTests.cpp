#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithDataflowActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// FMonolithDataflowActions::ListAssets
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDataflowSecurityListAssetsPathTest, "Monolith.Security.MonolithDataflow.ListAssets.RejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDataflowSecurityListAssetsPathTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("dataflow"), TEXT("list_assets")))
	{
		FMonolithDataflowActions::RegisterActions(Registry);
	}

	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("Game/Dataflow"), // Missing leading slash
	};

	for (const FString& Path : MalformedPaths)
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("package_path"), Path);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("dataflow"), TEXT("list_assets"), Payload);

		TestFalse(*FString::Printf(TEXT("list_assets with malformed path '%s' should return Error"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path '%s'"), *Path), Result.ErrorMessage.IsEmpty());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
