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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignClassifyZoneTensionParamGuardTest, "Monolith.Sentinel.LevelDesign.ClassifyZoneTensionParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignClassifyZoneTensionParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Negative radius
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> LocationArray;
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("location"), LocationArray);

		Params->SetNumberField(TEXT("radius"), -10.0);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("classify_zone_tension"), Params);
		TestFalse(TEXT("Negative radius should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention radius"), Result.ErrorMessage.Contains(TEXT("radius must be >=")));
	}

	// 2. Excessively large radius
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> LocationArray;
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		LocationArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("location"), LocationArray);

		Params->SetNumberField(TEXT("radius"), 100000.0);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("classify_zone_tension"), Params);
		TestFalse(TEXT("Excessively large radius should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention radius"), Result.ErrorMessage.Contains(TEXT("radius must be <=")));
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignPredictPlayerPathsParamGuardTest, "Monolith.ParamGuard.LevelDesign.PredictPlayerPathsParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignPredictPlayerPathsParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing start
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> EndArray;
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndArray);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("predict_player_paths"), Params);
		TestFalse(TEXT("Missing start should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention start"), Result.ErrorMessage.Contains(TEXT("start")));
	}

	// 2. Missing end
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> StartArray;
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("start"), StartArray);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("predict_player_paths"), Params);
		TestFalse(TEXT("Missing end should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention end"), Result.ErrorMessage.Contains(TEXT("end")));
	}

	// 3. Invalid start (wrong type)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("start"), TEXT("NotAnArray"));

		TArray<TSharedPtr<FJsonValue>> EndArray;
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndArray);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("predict_player_paths"), Params);
		TestFalse(TEXT("Wrong type start should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention start"), Result.ErrorMessage.Contains(TEXT("start")));
	}

	// 4. Valid params
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> StartArray;
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		StartArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("start"), StartArray);

		TArray<TSharedPtr<FJsonValue>> EndArray;
		EndArray.Add(MakeShared<FJsonValueNumber>(100.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(100.0));
		EndArray.Add(MakeShared<FJsonValueNumber>(0.0));
		Params->SetArrayField(TEXT("end"), EndArray);

		FMonolithActionResult Result = ExecuteHorrorAction(TEXT("predict_player_paths"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
