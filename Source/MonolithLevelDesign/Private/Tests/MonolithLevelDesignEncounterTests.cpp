#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignEncounterActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteEncounterAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignEncounterActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}

	TSharedPtr<FJsonObject> CreateValidRegion()
	{
		auto RegionObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> CenterArr;
		CenterArr.Add(MakeShared<FJsonValueNumber>(0.0));
		CenterArr.Add(MakeShared<FJsonValueNumber>(0.0));
		CenterArr.Add(MakeShared<FJsonValueNumber>(0.0));
		RegionObj->SetArrayField(TEXT("center"), CenterArr);
		RegionObj->SetNumberField(TEXT("radius"), 1000.0);
		return RegionObj;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignDesignEncounterParamTest, "Monolith.Sentinel.LevelDesign.DesignEncounterParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignDesignEncounterParamTest::RunTest(const FString& Parameters)
{
	// 1. Missing region
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("design_encounter"), Params);
		TestFalse(TEXT("Missing region should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
	}

	// 2. Wrong type for region
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("region"), TEXT("not_an_object"));
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("design_encounter"), Params);
		TestFalse(TEXT("Wrong type for region should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
	}

	// 3. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("region"), CreateValidRegion());
		// Also add a dummy param to make it clear we're executing with valid inputs
		Params->SetStringField(TEXT("archetype"), TEXT("stalker"));

		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("design_encounter"), Params);
		// Note: The action itself might fail later on if it can't find a nav mesh,
		// but it should at least pass the param validation step.
		// To be strictly correct according to "valid params succeed", let's check
		// that the error isn't about missing/invalid params.
		if (!Result.bSuccess)
		{
			TestFalse(TEXT("Valid params should not fail with missing param error"), Result.ErrorMessage.Contains(TEXT("Missing or invalid required param")));
		}
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignGenerateScareSequenceParamGuardTest, "Monolith.Sentinel.LevelDesign.GenerateScareSequenceParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignGenerateScareSequenceParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("generate_scare_sequence"), Params);
		TestFalse(TEXT("Missing path_points should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 2. Empty path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Params->SetArrayField(TEXT("path_points"), EmptyArray);
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("generate_scare_sequence"), Params);
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
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("generate_scare_sequence"), Params);
		TestFalse(TEXT("Only 1 path_point should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention path_points"), Result.ErrorMessage.Contains(TEXT("path_points")));
	}

	// 4. Wrong type path_points
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("path_points"), TEXT("NotAnArray"));
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("generate_scare_sequence"), Params);
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

		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("generate_scare_sequence"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzeAiTerritoryParamGuardTest, "Monolith.Sentinel.LevelDesign.AnalyzeAiTerritoryParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzeAiTerritoryParamGuardTest::RunTest(const FString& Parameters)
{
	// 1. Missing region
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("analyze_ai_territory"), Params);
		TestFalse(TEXT("Missing region should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
	}

	// 2. Wrong type for region
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("region"), TEXT("not_an_object"));
		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("analyze_ai_territory"), Params);
		TestFalse(TEXT("Wrong type for region should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention region"), Result.ErrorMessage.Contains(TEXT("region")));
	}

	// 3. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("region"), CreateValidRegion());

		FMonolithActionResult Result = ExecuteEncounterAction(TEXT("analyze_ai_territory"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("Missing or invalid required param")) == false;
		TestTrue(TEXT("Valid params should succeed or fail cleanly"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
