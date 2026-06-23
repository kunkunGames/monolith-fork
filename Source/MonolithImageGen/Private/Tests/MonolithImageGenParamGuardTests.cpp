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

	// Test 2: Missing prompt
	{
		TSharedPtr<FJsonObject> MissingPromptParams = MakeShared<FJsonObject>();
		MissingPromptParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image"), MissingPromptParams);
		TestFalse(TEXT("GenerateImage missing prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty prompt
	{
		TSharedPtr<FJsonObject> EmptyPromptParams = MakeShared<FJsonObject>();
		EmptyPromptParams->SetStringField(TEXT("prompt"), TEXT("   "));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image"), EmptyPromptParams);
		TestFalse(TEXT("GenerateImage empty prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardImageGenGenerateImageViaIma2MalformedParamsTest, "Monolith.ParamGuard.MonolithImageGen.GenerateImageViaIma2RejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardImageGenGenerateImageViaIma2MalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithImageGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("generate_image_via_ima2 action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("imagegen"), TEXT("generate_image_via_ima2")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image_via_ima2"), EmptyParams);
		TestFalse(TEXT("GenerateImageViaIma2 with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing prompt
	{
		TSharedPtr<FJsonObject> MissingPromptParams = MakeShared<FJsonObject>();
		MissingPromptParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image_via_ima2"), MissingPromptParams);
		TestFalse(TEXT("GenerateImageViaIma2 missing prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty prompt
	{
		TSharedPtr<FJsonObject> EmptyPromptParams = MakeShared<FJsonObject>();
		EmptyPromptParams->SetStringField(TEXT("prompt"), TEXT("   "));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image_via_ima2"), EmptyPromptParams);
		TestFalse(TEXT("GenerateImageViaIma2 empty prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardImageGenImportGeneratedImageMalformedParamsTest, "Monolith.ParamGuard.MonolithImageGen.ImportGeneratedImageRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardImageGenImportGeneratedImageMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithImageGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("import_generated_image action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("imagegen"), TEXT("import_generated_image")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("import_generated_image"), EmptyParams);
		TestFalse(TEXT("ImportGeneratedImage with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing bytes_b64 and file_path
	{
		TSharedPtr<FJsonObject> MissingInputParams = MakeShared<FJsonObject>();
		MissingInputParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("import_generated_image"), MissingInputParams);
		TestFalse(TEXT("ImportGeneratedImage missing input params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
