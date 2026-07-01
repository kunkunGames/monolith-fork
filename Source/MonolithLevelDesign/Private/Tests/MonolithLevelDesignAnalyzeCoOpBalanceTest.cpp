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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzeCoOpBalanceTest, "Monolith.Sentinel.LevelDesign.AnalyzeCoOpBalanceParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzeCoOpBalanceTest::RunTest(const FString& Parameters)
{
	// 1. Missing player_positions
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("analyze_co_op_balance"), Params);
		TestFalse(TEXT("Missing player_positions should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention player_positions"), Result.ErrorMessage.Contains(TEXT("player_positions")));
	}

	// 2. Too few player_positions (1 player)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PositionsArr;

		TArray<TSharedPtr<FJsonValue>> Pos1;
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		PositionsArr.Add(MakeShared<FJsonValueArray>(Pos1));

		Params->SetArrayField(TEXT("player_positions"), PositionsArr);

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("analyze_co_op_balance"), Params);
		TestFalse(TEXT("1 player should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention player_positions"), Result.ErrorMessage.Contains(TEXT("player_positions")));
	}

	// 3. Too many player_positions (9 players)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PositionsArr;

		TArray<TSharedPtr<FJsonValue>> Pos;
		Pos.Add(MakeShared<FJsonValueNumber>(0));
		Pos.Add(MakeShared<FJsonValueNumber>(0));
		Pos.Add(MakeShared<FJsonValueNumber>(0));

        for (int32 i = 0; i < 9; ++i)
        {
            PositionsArr.Add(MakeShared<FJsonValueArray>(Pos));
        }

		Params->SetArrayField(TEXT("player_positions"), PositionsArr);

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("analyze_co_op_balance"), Params);
		TestFalse(TEXT("9 players should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention maximum 8"), Result.ErrorMessage.Contains(TEXT("Maximum 8 player positions")));
	}

    // 4. Invalid position format
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PositionsArr;

		TArray<TSharedPtr<FJsonValue>> Pos1;
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		PositionsArr.Add(MakeShared<FJsonValueArray>(Pos1));

        TArray<TSharedPtr<FJsonValue>> Pos2;
		Pos2.Add(MakeShared<FJsonValueNumber>(0));
		Pos2.Add(MakeShared<FJsonValueNumber>(0));
		PositionsArr.Add(MakeShared<FJsonValueArray>(Pos2));

		Params->SetArrayField(TEXT("player_positions"), PositionsArr);

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("analyze_co_op_balance"), Params);
		TestFalse(TEXT("Invalid position format should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention [x, y, z]"), Result.ErrorMessage.Contains(TEXT("[x, y, z]")));
	}

	// 5. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> PositionsArr;

		TArray<TSharedPtr<FJsonValue>> Pos1;
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		Pos1.Add(MakeShared<FJsonValueNumber>(0));
		PositionsArr.Add(MakeShared<FJsonValueArray>(Pos1));

        TArray<TSharedPtr<FJsonValue>> Pos2;
		Pos2.Add(MakeShared<FJsonValueNumber>(100));
		Pos2.Add(MakeShared<FJsonValueNumber>(100));
		Pos2.Add(MakeShared<FJsonValueNumber>(0));
		PositionsArr.Add(MakeShared<FJsonValueArray>(Pos2));

		Params->SetArrayField(TEXT("player_positions"), PositionsArr);

		FMonolithActionResult Result = ExecuteQualityAction(TEXT("analyze_co_op_balance"), Params);
		bool bIsExpectedResult =
			Result.bSuccess ||
			Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
