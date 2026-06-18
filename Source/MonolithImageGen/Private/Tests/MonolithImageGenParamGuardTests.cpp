// Copyright tumourlove. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithImageGenActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardImageGenGenerateImageMalformedParamsTest, "Monolith.ParamGuard.MonolithImageGen.GenerateImageRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardImageGenGenerateImageMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithImageGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("generate_image action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("imagegen"), TEXT("generate_image")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image"), EmptyParams);
		TestFalse(TEXT("GenerateImage with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
