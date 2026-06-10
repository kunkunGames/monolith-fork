#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithChooserAuthoringActions.h"
#include "MonolithMirrorTableActions.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

// Gated on WITH_CHOOSER: the off-gate stub returns "Chooser plugin not
// available" instead of reaching package path validation.
#if WITH_CHOOSER
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCrashguardChooserTableTest, "Monolith.Crashguard.Animation.CreateChooserTableRejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithCrashguardChooserTableTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("//Game/Malformed/Table"));
	FMonolithActionResult Result = FMonolithChooserAuthoringActions::HandleCreateChooserTable(Payload);

	TestFalse(TEXT("create_chooser_table should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("create_chooser_table error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")) || Result.ErrorMessage.Contains(TEXT("Package path")));

	return true;
}
#endif // WITH_CHOOSER

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithCrashguardMirrorTableTest, "Monolith.Crashguard.Animation.CreateMirrorTableRejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithCrashguardMirrorTableTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("asset_path"), TEXT("//Game/Malformed/Mirror"));
	Payload->SetStringField(TEXT("skeleton_path"), TEXT("/Game/Anims/MySkeleton"));
	FMonolithActionResult Result = FMonolithMirrorTableActions::HandleCreateMirrorDataTable(Payload);

	TestFalse(TEXT("create_mirror_data_table should fail on malformed path"), Result.bSuccess);
	TestTrue(TEXT("create_mirror_data_table error should complain about invalid package path"), Result.ErrorMessage.Contains(TEXT("Invalid package path")) || Result.ErrorMessage.Contains(TEXT("Package path")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
