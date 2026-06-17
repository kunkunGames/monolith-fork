#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignQualityActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteQualityAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignQualityActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignEvaluateMonsterRevealTest, "Monolith.Sentinel.LevelDesign.EvaluateMonsterRevealParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignEvaluateMonsterRevealTest::RunTest(const FString& Parameters)
{
	// 1. Missing player_location
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidRot;
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("player_rotation"), ValidRot);
		Params->SetStringField(TEXT("monster_actor"), TEXT("TestMonster"));

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("evaluate_monster_reveal"), Params);
		TestFalse(TEXT("Missing player_location should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention player_location"), Result.ErrorMessage.Contains(TEXT("player_location")));
	}

	// 2. Missing player_rotation
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("player_location"), ValidLoc);
		Params->SetStringField(TEXT("monster_actor"), TEXT("TestMonster"));

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("evaluate_monster_reveal"), Params);
		TestFalse(TEXT("Missing player_rotation should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention player_rotation"), Result.ErrorMessage.Contains(TEXT("player_rotation")));
	}

	// 3. Missing monster_actor
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLocRot;
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("player_location"), ValidLocRot);
		Params->SetArrayField(TEXT("player_rotation"), ValidLocRot);

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("evaluate_monster_reveal"), Params);
		TestFalse(TEXT("Missing monster_actor should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention monster_actor"), Result.ErrorMessage.Contains(TEXT("monster_actor")));
	}

	// 4. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLocRot;
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("player_location"), ValidLocRot);
		Params->SetArrayField(TEXT("player_rotation"), ValidLocRot);
		Params->SetStringField(TEXT("monster_actor"), TEXT("TestMonster"));

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("evaluate_monster_reveal"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
