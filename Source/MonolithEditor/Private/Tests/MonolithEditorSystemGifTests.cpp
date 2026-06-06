#include "Misc/AutomationTest.h"
#include "MonolithEditorActions.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithEditorCaptureSystemGifMalformedDurationTest,
	"Monolith.ParamGuard.EditorPreview.CaptureSystemGifMalformedDuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithEditorCaptureSystemGifMalformedDurationTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Engine/BasicShapes/Cube"));
	Params->SetStringField(TEXT("duration_seconds"), TEXT("not_a_number"));

	const FMonolithActionResult Result = FMonolithEditorActions::HandleCaptureSystemGif(Params);
	TestFalse(TEXT("Malformed duration_seconds returns an error"), Result.bSuccess);
	TestTrue(TEXT("Error message mentions duration_seconds"),
		Result.ErrorMessage.Contains(TEXT("duration_seconds")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
