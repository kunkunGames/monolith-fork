#include "CoreTypes.h"
#include "Misc/AutomationTest.h"
#include "MonolithSourceActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReviewContextParamValidationTest, "Monolith.SourceIndexer.Source.ReviewContextParamValidation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSourceReviewContextParamValidationTest::RunTest(const FString& Parameters)
{
	// 1. Missing required 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = FMonolithSourceActions::HandleReviewContext(Params);
		TestTrue(TEXT("Missing symbol returns error"), !Result.bSuccess);
		// Note: The actual implementation in MonolithSourceActions::HandleReviewContext returns -32602
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	// 2. Empty 'symbol' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("symbol"), TEXT(""));

		FMonolithActionResult Result = FMonolithSourceActions::HandleReviewContext(Params);
		TestTrue(TEXT("Empty symbol returns error"), !Result.bSuccess);
		TestEqual(TEXT("Error code is invalid params"), Result.ErrorCode, -32602);
	}

	return true;
}
