#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithEditorActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorGetBuildErrorsMalformedTest, "Monolith.ParamGuard.Editor.GetBuildErrorsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorGetBuildErrorsMalformedTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("clear_baseline"), 12345);

		FMonolithActionResult Result = FMonolithEditorActions::HandleGetBuildErrors(Payload);

		TestFalse(TEXT("Malformed clear_baseline should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should name the parameter clear_baseline"), Result.ErrorMessage.Contains(TEXT("clear_baseline")));
	}

	return true;
}
