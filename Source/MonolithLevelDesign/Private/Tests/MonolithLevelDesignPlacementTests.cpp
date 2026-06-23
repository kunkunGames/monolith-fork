#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignPlacementActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecutePlacementAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("scene"), Action))
		{
			FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("scene"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignRandomizeTransformsTest, "Monolith.Sentinel.LevelDesign.RandomizeTransformsParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignRandomizeTransformsTest::RunTest(const FString& Parameters)
{
	// 1. Missing actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Missing actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 2. Empty actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Params->SetArrayField(TEXT("actor_names"), EmptyArray);
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Empty actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 3. Wrong type actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("actor_names"), TEXT("NotAnArray"));
		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		TestFalse(TEXT("Wrong type actor_names should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention actor_names"), Result.ErrorMessage.Contains(TEXT("actor_names")));
	}

	// 4. Valid actor_names
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidArray;
		ValidArray.Add(MakeShared<FJsonValueString>(TEXT("DummyActor")));
		Params->SetArrayField(TEXT("actor_names"), ValidArray);

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("randomize_transforms"), Params);
		// It might succeed with 0 modified, or fail with "No editor world available" if there's no world
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignGetLevelActorsParamGuardTest, "Monolith.ParamGuard.LevelDesign.GetLevelActors", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignGetLevelActorsParamGuardTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("radius"), TEXT("NotANumber"));

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestFalse(TEXT("get_level_actors should reject string radius"), Result.bSuccess);
		TestTrue(TEXT("radius type error should mention radius"), Result.ErrorMessage.Contains(TEXT("radius")));
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("radius"), -1.0);

		FMonolithActionResult Result = ExecutePlacementAction(TEXT("get_level_actors"), Params);
		TestFalse(TEXT("get_level_actors should reject negative radius"), Result.bSuccess);
		TestTrue(TEXT("negative radius error should mention lower bound"), Result.ErrorMessage.Contains(TEXT(">= 0.0")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
