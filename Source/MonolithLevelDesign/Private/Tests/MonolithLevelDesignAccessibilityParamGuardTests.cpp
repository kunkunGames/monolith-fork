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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignValidatePathWidthParamGuardTest, "Monolith.ParamGuard.LevelDesign.ValidatePathWidthRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignValidatePathWidthParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing start
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("end"), ValidLoc);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		TestFalse(TEXT("Missing start should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention start"), Result.ErrorMessage.Contains(TEXT("start")));
	}

	// 2. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("start"), ValidLoc);
		Params->SetArrayField(TEXT("end"), ValidLoc);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_path_width"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available")) || Result.ErrorMessage.Contains(TEXT("No navmesh path found"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world/navmesh"), bIsExpectedResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignValidateInteractiveReachParamGuardTest, "Monolith.ParamGuard.LevelDesign.ValidateInteractiveReachRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignValidateInteractiveReachParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Valid empty parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_interactive_reach"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid empty params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	// 2. Valid with region parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> MinLoc;
		MinLoc.Add(MakeShared<FJsonValueNumber>(0));
		MinLoc.Add(MakeShared<FJsonValueNumber>(0));
		MinLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("region_min"), MinLoc);

		TArray<TSharedPtr<FJsonValue>> MaxLoc;
		MaxLoc.Add(MakeShared<FJsonValueNumber>(100));
		MaxLoc.Add(MakeShared<FJsonValueNumber>(100));
		MaxLoc.Add(MakeShared<FJsonValueNumber>(100));
		Params->SetArrayField(TEXT("region_max"), MaxLoc);

		FMonolithActionResult Result = ExecuteAccessibilityAction(TEXT("validate_interactive_reach"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid region params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
