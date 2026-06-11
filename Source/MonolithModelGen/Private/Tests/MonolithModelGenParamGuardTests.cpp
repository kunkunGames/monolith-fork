#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#include "MonolithModelGenActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenSubmitJobMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.SubmitJobRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenSubmitJobMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("submit_generated_model_job action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("submit_generated_model_job")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("submit_generated_model_job"), EmptyParams);
		TestFalse(TEXT("SubmitJob with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing prompt
	{
		TSharedPtr<FJsonObject> MissingPromptParams = MakeShared<FJsonObject>();
		MissingPromptParams->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		MissingPromptParams->SetStringField(TEXT("model"), TEXT("monolith/local-obj-v1"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("submit_generated_model_job"), MissingPromptParams);
		TestFalse(TEXT("SubmitJob missing prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty prompt
	{
		TSharedPtr<FJsonObject> EmptyPromptParams = MakeShared<FJsonObject>();
		EmptyPromptParams->SetStringField(TEXT("prompt"), TEXT("   "));
		EmptyPromptParams->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		EmptyPromptParams->SetStringField(TEXT("model"), TEXT("monolith/local-obj-v1"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("submit_generated_model_job"), EmptyPromptParams);
		TestFalse(TEXT("SubmitJob empty prompt should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 4: Invalid provider
	{
		TSharedPtr<FJsonObject> InvalidProviderParams = MakeShared<FJsonObject>();
		InvalidProviderParams->SetStringField(TEXT("prompt"), TEXT("A wooden chair"));
		InvalidProviderParams->SetStringField(TEXT("provider"), TEXT("unsupported_provider"));
		InvalidProviderParams->SetStringField(TEXT("model"), TEXT("monolith/local-obj-v1"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("submit_generated_model_job"), InvalidProviderParams);
		TestFalse(TEXT("SubmitJob unsupported provider should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 5: Valid params
	{
		TSharedPtr<FJsonObject> ValidParams = MakeShared<FJsonObject>();
		ValidParams->SetStringField(TEXT("prompt"), TEXT("A wooden chair"));
		ValidParams->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		ValidParams->SetStringField(TEXT("model"), TEXT("monolith/local-obj-v1"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("submit_generated_model_job"), ValidParams);
		TestTrue(TEXT("SubmitJob valid params should succeed"), Result.bSuccess);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			FString LocalFile;
			if (Result.Result->TryGetStringField(TEXT("local_file"), LocalFile) && !LocalFile.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*FPaths::GetPath(LocalFile), false, true);
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenGetJobMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.GetJobRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenGetJobMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("get_generated_model_job action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("get_generated_model_job")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_job"), EmptyParams);
		TestFalse(TEXT("GetJob with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing job_id
	{
		TSharedPtr<FJsonObject> MissingJobIdParams = MakeShared<FJsonObject>();
		MissingJobIdParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_job"), MissingJobIdParams);
		TestFalse(TEXT("GetJob missing job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty job_id
	{
		TSharedPtr<FJsonObject> EmptyJobIdParams = MakeShared<FJsonObject>();
		EmptyJobIdParams->SetStringField(TEXT("job_id"), TEXT(""));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_job"), EmptyJobIdParams);
		TestFalse(TEXT("GetJob empty job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenDownloadJobMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.DownloadJobRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenDownloadJobMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("download_generated_model_result action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("download_generated_model_result")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("download_generated_model_result"), EmptyParams);
		TestFalse(TEXT("DownloadJob with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing job_id
	{
		TSharedPtr<FJsonObject> MissingJobIdParams = MakeShared<FJsonObject>();
		MissingJobIdParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("download_generated_model_result"), MissingJobIdParams);
		TestFalse(TEXT("DownloadJob missing job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty job_id
	{
		TSharedPtr<FJsonObject> EmptyJobIdParams = MakeShared<FJsonObject>();
		EmptyJobIdParams->SetStringField(TEXT("job_id"), TEXT(""));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("download_generated_model_result"), EmptyJobIdParams);
		TestFalse(TEXT("DownloadJob empty job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenCancelJobMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.CancelJobRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenCancelJobMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("cancel_generated_model_job action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("cancel_generated_model_job")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("cancel_generated_model_job"), EmptyParams);
		TestFalse(TEXT("CancelJob with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing job_id
	{
		TSharedPtr<FJsonObject> MissingJobIdParams = MakeShared<FJsonObject>();
		MissingJobIdParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("cancel_generated_model_job"), MissingJobIdParams);
		TestFalse(TEXT("CancelJob missing job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty job_id
	{
		TSharedPtr<FJsonObject> EmptyJobIdParams = MakeShared<FJsonObject>();
		EmptyJobIdParams->SetStringField(TEXT("job_id"), TEXT(""));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("cancel_generated_model_job"), EmptyJobIdParams);
		TestFalse(TEXT("CancelJob empty job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenImportJobMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.ImportJobRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenImportJobMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("import_generated_model action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("import_generated_model")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), EmptyParams);
		TestFalse(TEXT("ImportJob with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenGetProvenanceMalformedParamsTest, "Monolith.ParamGuard.MonolithModelGen.GetProvenanceRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenGetProvenanceMalformedParamsTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("get_generated_model_provenance action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("get_generated_model_provenance")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_provenance"), EmptyParams);
		TestFalse(TEXT("GetProvenance with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 2: Missing asset_path
	{
		TSharedPtr<FJsonObject> MissingAssetPathParams = MakeShared<FJsonObject>();
		MissingAssetPathParams->SetStringField(TEXT("other_param"), TEXT("value"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_provenance"), MissingAssetPathParams);
		TestFalse(TEXT("GetProvenance missing asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 3: Empty asset_path
	{
		TSharedPtr<FJsonObject> EmptyAssetPathParams = MakeShared<FJsonObject>();
		EmptyAssetPathParams->SetStringField(TEXT("asset_path"), TEXT(""));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("get_generated_model_provenance"), EmptyAssetPathParams);
		TestFalse(TEXT("GetProvenance empty asset_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}
