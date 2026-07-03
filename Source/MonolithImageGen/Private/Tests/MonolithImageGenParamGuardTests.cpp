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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithImageGenGenerateImageViaIma2RateLimitCooldownTest, "Monolith.ParamGuard.MonolithImageGen.GenerateImageViaIma2RateLimitCooldown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithImageGenGenerateImageViaIma2RateLimitCooldownTest::RunTest(const FString& Parameters)
{
	if (!FMonolithToolRegistry::Get().HasAction(TEXT("imagegen"), TEXT("generate_image_via_ima2")))
	{
		FMonolithImageGenActions::RegisterActions(FMonolithToolRegistry::Get());
	}
	FMonolithImageGenActions::TestResetIma2RateLimitCooldowns();

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("prompt"), TEXT("rate limit cooldown smoke"));
	Params->SetStringField(TEXT("server_url"), TEXT("http://127.0.0.1:9"));
	Params->SetStringField(TEXT("format"), TEXT("png"));
	Params->SetBoolField(TEXT("save"), false);
	Params->SetNumberField(TEXT("timeout_seconds"), 1.0);

	const FString RetrySignature = FMonolithImageGenActions::TestBuildIma2RetrySignature(Params);
	TestTrue(TEXT("retry signature computed"), RetrySignature.StartsWith(TEXT("sha256:")));
	FMonolithImageGenActions::TestRecordIma2RateLimitCooldown(RetrySignature, 120.0);

	const FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("imagegen"), TEXT("generate_image_via_ima2"), Params);
	TestFalse(TEXT("identical request inside cooldown fails"), Result.bSuccess);
	TestEqual(TEXT("cooldown error code"), Result.ErrorCode, -32603);
	TestTrue(TEXT("cooldown message indicates skipped provider call"), Result.ErrorMessage.Contains(TEXT("provider call skipped")));
	TestTrue(TEXT("cooldown error data present"), Result.ErrorData.IsValid());
	if (Result.ErrorData.IsValid())
	{
		FString ErrorClass;
		Result.ErrorData->TryGetStringField(TEXT("error_class"), ErrorClass);
		TestEqual(TEXT("cooldown error class"), ErrorClass, FString(TEXT("provider_rate_limited")));
		FString ResultSignature;
		Result.ErrorData->TryGetStringField(TEXT("retry_signature"), ResultSignature);
		TestEqual(TEXT("cooldown preserves retry signature"), ResultSignature, RetrySignature);
		TestTrue(TEXT("provider call skipped flag"), Result.ErrorData->GetBoolField(TEXT("provider_call_skipped")));
		TestTrue(TEXT("cooldown active flag"), Result.ErrorData->GetBoolField(TEXT("rate_limit_cooldown_active")));
		double RetryAfterSeconds = 0.0;
		Result.ErrorData->TryGetNumberField(TEXT("retry_after_seconds"), RetryAfterSeconds);
		TestTrue(TEXT("retry_after_seconds remains positive"), RetryAfterSeconds > 0.0);
	}

	FMonolithImageGenActions::TestResetIma2RateLimitCooldowns();
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
