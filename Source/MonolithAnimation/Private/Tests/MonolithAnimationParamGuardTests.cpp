#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationParamGuardSetSectionNextTest, "Monolith.ParamGuard.Animation.SetSectionNext", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationParamGuardSetSectionNextTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// Test SetSectionNext with wrong types for section_name and next_section_name
	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesntExist.DoesntExist"));
		BadParams->SetNumberField(TEXT("section_name"), 123); // Invalid type
		BadParams->SetNumberField(TEXT("next_section_name"), 456); // Invalid type

		FMonolithActionResult Result = Registry.ExecuteAction(TEXT("animation"), TEXT("set_section_next"), BadParams);

		TestFalse(TEXT("SetSectionNext should fail if section_name is not a string"), Result.bSuccess);
		TestTrue(TEXT("SetSectionNext error should mention parameter invalidity"), Result.Error.Contains(TEXT("Parameter")));
	}

	return true;
}
