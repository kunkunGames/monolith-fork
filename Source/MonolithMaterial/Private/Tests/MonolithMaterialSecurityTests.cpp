#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityPathTest, "Monolith.Security.Material.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityPathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestMaterial"), // Double leading slash
		TEXT("Game/MalformedPath/TestMaterial"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestMaterial/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestMaterial#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), Path);

		// Call the action
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create"), Payload);

		// Verify it failed gracefully and returned the validation error
		TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path or empty asset name for: %s"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Invalid asset path")) ||
				Result.ErrorMessage.Contains(TEXT("Asset name is empty")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialPreviewTexturesLimitTest, "Monolith.LimitGuard.Material.PreviewTexturesLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialPreviewTexturesLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (int32 i = 0; i < 101; ++i)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("/Game/Textures/Tex_%d"), i)));
	}
	Payload->SetArrayField(TEXT("asset_paths"), PathsArray);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("preview_textures"), Payload);

	TestFalse(TEXT("Action should fail on oversized input array"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialBatchRecompileLimitTest, "Monolith.LimitGuard.Material.BatchRecompileLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialBatchRecompileLimitTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> PathsArray;
	for (int32 i = 0; i < 201; ++i)
	{
		PathsArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("/Game/Materials/Mat_%d"), i)));
	}
	Payload->SetArrayField(TEXT("asset_paths"), PathsArray);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("batch_recompile"), Payload);

	TestFalse(TEXT("Action should fail on oversized input array"), Result.bSuccess);
	TestTrue(TEXT("Error should complain about maximum allowed size"), Result.ErrorMessage.Contains(TEXT("exceeds maximum allowed size")));

	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSecurityCreateFunctionInstancePathTest, "Monolith.Security.Material.CreateFunctionInstance.ValidatePackagePath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSecurityCreateFunctionInstancePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/MalformedPath/TestFunctionInstance"), // Double leading slash
		TEXT("Game/MalformedPath/TestFunctionInstance"), // Missing leading slash
		TEXT("/Game/MalformedPath/TestFunctionInstance/"), // Trailing slash
		TEXT("/Game/MalformedPath/TestFunctionInstance#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		// Setup payload to simulate malformed path
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("asset_path"), Path);
		Payload->SetStringField(TEXT("parent"), TEXT("/Game/ParentFunction"));

		// Call the action
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("material"), TEXT("create_function_instance"), Payload);

		// Verify it failed gracefully and returned the validation error
		TestFalse(*FString::Printf(TEXT("Action should fail on malformed path: %s"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path: %s"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should complain about invalid package path or empty asset name for: %s"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Invalid asset path")) ||
				Result.ErrorMessage.Contains(TEXT("Asset name is empty")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
