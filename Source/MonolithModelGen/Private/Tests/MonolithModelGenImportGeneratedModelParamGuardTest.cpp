#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#include "MonolithModelGenActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardModelGenImportGeneratedModelTest, "Monolith.ParamGuard.MonolithModelGen.ImportGeneratedModelRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithParamGuardModelGenImportGeneratedModelTest::RunTest(const FString& Parameters)
{
	FMonolithModelGenActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("import_generated_model action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("modelgen"), TEXT("import_generated_model")));

	// Test 1: Empty parameters
	{
		TSharedPtr<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), EmptyParams);
		TestFalse(TEXT("ImportGeneratedModel with empty params should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
		TestTrue(TEXT("Error message should mention job_id or file_path"), Result.Error.Contains(TEXT("Either job_id or file_path is required")));
	}

	// Test 2: Missing destination defaults to /Game/GeneratedModels
	// But file_path is not found.
	{
		TSharedPtr<FJsonObject> MissingDestParams = MakeShared<FJsonObject>();
		MissingDestParams->SetStringField(TEXT("file_path"), TEXT("/fake/path/that/does/not/exist.fbx"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), MissingDestParams);
		TestFalse(TEXT("ImportGeneratedModel with missing dest and non-existent file should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
		TestTrue(TEXT("Error message should mention model file not found"), Result.Error.Contains(TEXT("Model file not found")));
	}

	// Test 3: Invalid destination folder (malformed path)
	{
		TSharedPtr<FJsonObject> InvalidDestParams = MakeShared<FJsonObject>();
		InvalidDestParams->SetStringField(TEXT("file_path"), TEXT("some_valid_looking_path.fbx")); // File existence not checked yet
		InvalidDestParams->SetStringField(TEXT("destination"), TEXT("NotStartingWithSlashGame"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), InvalidDestParams);
		TestFalse(TEXT("ImportGeneratedModel with malformed destination should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
		// Assuming MonolithModelGen::ValidateDestinationFolder returns an error like "Destination folder must start with /Game/"
	}

	// Test 4: Both job_id and file_path empty string
	{
		TSharedPtr<FJsonObject> EmptyStringsParams = MakeShared<FJsonObject>();
		EmptyStringsParams->SetStringField(TEXT("job_id"), TEXT(""));
		EmptyStringsParams->SetStringField(TEXT("file_path"), TEXT(""));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), EmptyStringsParams);
		TestFalse(TEXT("ImportGeneratedModel with empty string for job_id and file_path should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
		TestTrue(TEXT("Error message should mention job_id or file_path"), Result.Error.Contains(TEXT("Either job_id or file_path is required")));
	}

	// Test 5: Wrong type for job_id (number instead of string)
	// HasField without type check would not catch this, but TryGetStringField does.
	{
		TSharedPtr<FJsonObject> WrongTypeParams = MakeShared<FJsonObject>();
		WrongTypeParams->SetNumberField(TEXT("job_id"), 12345);
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), WrongTypeParams);
		TestFalse(TEXT("ImportGeneratedModel with wrong type job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	// Test 6: Invalid job_id
	{
		TSharedPtr<FJsonObject> InvalidJobIdParams = MakeShared<FJsonObject>();
		InvalidJobIdParams->SetStringField(TEXT("job_id"), TEXT("../../illegal_path"));
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("modelgen"), TEXT("import_generated_model"), InvalidJobIdParams);
		TestFalse(TEXT("ImportGeneratedModel with invalid job_id should fail"), Result.bSuccess);
		TestEqual(TEXT("Error code should be invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}
