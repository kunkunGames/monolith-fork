#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignHorrorActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteHorrorAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignHorrorActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzePacingCurveParamGuardTest, "Monolith.Sentinel.LevelDesign.AnalyzePacingCurveParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzePacingCurveParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("analyze_pacing_curve"), Params);
		TestFalse(TEXT("Missing path_points should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 2. Empty path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Params->SetArrayField(TEXT("path_points"), EmptyArray);
		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("analyze_pacing_curve"), Params);
		TestFalse(TEXT("Empty path_points should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 3. Not enough path_points (only 1)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OnePointArray;

		TArray<TSharedPtr<FJsonValue>> Pt1;
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));
		OnePointArray.Add(MakeShared<FJsonValueArray>(Pt1));

		Params->SetArrayField(TEXT("path_points"), OnePointArray);
		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("analyze_pacing_curve"), Params);
		TestFalse(TEXT("Only 1 path_point should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 4. Wrong type path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_points"), TEXT("NotAnArray"));
		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("analyze_pacing_curve"), Params);
		TestFalse(TEXT("Wrong type path_points should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 5. Valid path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidArray;

		TArray<TSharedPtr<FJsonValue>> Pt1;
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));
		Pt1.Add(MakeShared<FJsonValueNumber>(0.0));

		TArray<TSharedPtr<FJsonValue>> Pt2;
		Pt2.Add(MakeShared<FJsonValueNumber>(100.0));
		Pt2.Add(MakeShared<FJsonValueNumber>(100.0));
		Pt2.Add(MakeShared<FJsonValueNumber>(0.0));

		ValidArray.Add(MakeShared<FJsonValueArray>(Pt1));
		ValidArray.Add(MakeShared<FJsonValueArray>(Pt2));

		Params->SetArrayField(TEXT("path_points"), ValidArray);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("analyze_pacing_curve"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}



IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLimitGuardLevelDesignAnalyzeSightlinesRayCountTest, "Monolith.LimitGuard.LevelDesign.AnalyzeSightlinesRejectsLargeRayCount", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLimitGuardLevelDesignAnalyzeSightlinesRayCountTest::RunTest(const FString& Parameters)
{
	FMonolithLevelDesignHorrorActions::RegisterActions(FMonolithToolRegistry::Get());
	TestTrue(TEXT("analyze_sightlines action is registered"), FMonolithToolRegistry::Get().HasAction(TEXT("leveldesign"), TEXT("analyze_sightlines")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> LocArr;
	LocArr.Add(MakeShared<FJsonValueNumber>(0.0));
	LocArr.Add(MakeShared<FJsonValueNumber>(0.0));
	LocArr.Add(MakeShared<FJsonValueNumber>(0.0));
	Params->SetArrayField(TEXT("location"), LocArr);
	Params->SetNumberField(TEXT("ray_count"), 1000.0);

	FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("leveldesign"), TEXT("analyze_sightlines"), Params);
	TestFalse(TEXT("analyze_sightlines rejects excessively large ray_count"), Result.bSuccess);
	TestTrue(TEXT("analyze_sightlines reports the validation error for ray_count"), Result.ErrorMessage.Contains(TEXT("ray_count must be between 4 and 128")));

	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
