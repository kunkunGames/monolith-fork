#include "CoreTypes.h"
#include "Misc/AutomationTest.h"
#include "MonolithSourceActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewContextParamValidationTest, "Monolith.SourceIndexer.Source.ReviewContextParamValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceReviewContextParamValidationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("source"), TEXT("review_context")))
	{
		FMonolithSourceActions::RegisterAll();
	}

	// 1. Missing required 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Missing symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 2. Empty 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT(""));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Empty symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 3. Malformed 'direction' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT("MySymbol"));
		Params->SetNumberField(TEXT("direction"), 123);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Malformed direction returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 4. Malformed 'max_depth' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT("MySymbol"));
		Params->SetStringField(TEXT("max_depth"), TEXT("2"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Malformed max_depth returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 5. Malformed 'max_results' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT("MySymbol"));
		Params->SetStringField(TEXT("max_results"), TEXT("200"));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Malformed max_results returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 6. Malformed 'detail_level' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT("MySymbol"));
		Params->SetNumberField(TEXT("detail_level"), 1);

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("review_context"), Params);
		TestTrue(TEXT("Malformed detail_level returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceRiskScoreParamValidationTest, "Monolith.Source.RiskScoreParamValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceRiskScoreParamValidationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("source"), TEXT("risk_score")))
	{
		FMonolithSourceActions::RegisterAll();
	}

	// 1. Missing required 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("risk_score"), Params);
		TestTrue(TEXT("Missing symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 2. Empty 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT(""));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("risk_score"), Params);
		TestTrue(TEXT("Empty symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceImpactRadiusParamValidationTest, "Monolith.Source.ImpactRadiusParamValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceImpactRadiusParamValidationTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	if (!Registry.HasAction(TEXT("source"), TEXT("impact_radius")))
	{
		FMonolithSourceActions::RegisterAll();
	}

	// 1. Missing required 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("impact_radius"), Params);
		TestTrue(TEXT("Missing symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 2. Empty 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT(""));

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("source"), TEXT("impact_radius"), Params);
		TestTrue(TEXT("Empty symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}
