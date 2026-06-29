#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignAccessibilityActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAccessibilityAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignAccessibilityActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignValidatePathWidthParamGuardTest, "Monolith.Sentinel.LevelDesign.ValidatePathWidthParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignValidatePathWidthParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing start
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EndPt;
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndPt);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		TestFalse(TEXT("Missing start should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention start"), Result.ErrorMessage.Contains(TEXT("start")));
	}

	// 2. Missing end
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StartPt;
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("start"), StartPt);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		TestFalse(TEXT("Missing end should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention end"), Result.ErrorMessage.Contains(TEXT("end")));
	}

	// 3. Wrong type start
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("start"), TEXT("NotAnArray"));
		TArray<TSharedPtr<FJsonValue>> EndPt;
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndPt);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		TestFalse(TEXT("Wrong type start should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention start"), Result.ErrorMessage.Contains(TEXT("start")));
	}

	// 4. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StartPt;
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		StartPt.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("start"), StartPt);

		TArray<TSharedPtr<FJsonValue>> EndPt;
		EndPt.Add(MakeShared<FJsonValueNumber>(100.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(100.0));
		EndPt.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndPt);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available")) || Result.ErrorMessage.Contains(TEXT("No navmesh path found"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world/navmesh"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
