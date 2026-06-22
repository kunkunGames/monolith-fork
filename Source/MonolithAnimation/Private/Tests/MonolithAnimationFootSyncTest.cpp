#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithAnimationActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithDeriveFootSyncMarkersParamGuardTest, "Monolith.ParamGuard.Animation.DeriveFootSyncMarkersRejectsMalformedThresholds", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithDeriveFootSyncMarkersParamGuardTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/DummyAnim"));

	TSharedPtr<FJsonObject> ThresholdsObj = MakeShared<FJsonObject>();
	// Provide malformed string instead of expected number
	ThresholdsObj->SetStringField(TEXT("contact_mid"), TEXT("fast"));
	ThresholdsObj->SetStringField(TEXT("sample_rate"), TEXT("very_fast"));
	Params->SetObjectField(TEXT("thresholds"), ThresholdsObj);

	FMonolithActionResult Result = FMonolithAnimationActions::HandleDeriveFootSyncMarkers(Params);
	TestFalse(TEXT("HandleDeriveFootSyncMarkers should reject malformed thresholds"), Result.bSuccess);
	TestTrue(TEXT("Error message should mention number type mismatch"),
		Result.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
