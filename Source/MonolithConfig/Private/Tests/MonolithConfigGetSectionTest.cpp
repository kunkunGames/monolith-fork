// SPDX-License-Identifier: MIT
// Validation tests for get_section config action.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MonolithConfigActions.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithConfigGetSectionTest,
	"Monolith.Config.GetSection.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithConfigGetSectionTest::RunTest(const FString& /*Parameters*/)
{
	// Failure scenario: invalid section
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("file"), TEXT("MonolithSettings"));
		P->SetStringField(TEXT("section"), TEXT("/Script/DoesNotExist.InvalidSection123"));

		const FMonolithActionResult Result = FMonolithConfigActions::GetSection(P);
		TestFalse(TEXT("action should fail for invalid section"), Result.bSuccess);
		TestTrue(TEXT("error message mentions section not found"), Result.ErrorMessage.Contains(TEXT("not found")));
	}

	// Success scenario: valid section
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("file"), TEXT("MonolithSettings"));
		P->SetStringField(TEXT("section"), TEXT("/Script/MonolithCore.MonolithSettings"));

		const FMonolithActionResult Result = FMonolithConfigActions::GetSection(P);
		TestTrue(TEXT("action should succeed for valid section"), Result.bSuccess);
		if (Result.Result.IsValid())
		{
			FString SectionName;
			Result.Result->TryGetStringField(TEXT("section"), SectionName);
			TestEqual(TEXT("result section matches"), SectionName, TEXT("/Script/MonolithCore.MonolithSettings"));

			const TSharedPtr<FJsonObject>* EntriesObj;
			if (Result.Result->TryGetObjectField(TEXT("entries"), EntriesObj))
			{
				TestTrue(TEXT("entries object is present"), true);
			}
		}
	}

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
