#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignPlacementActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteMeasureDistanceAction(const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("scene"), TEXT("measure_distance")))
		{
			FMonolithLevelDesignPlacementActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("scene"), TEXT("measure_distance"), Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignMeasureDistanceTest, "Monolith.Sentinel.LevelDesign.MeasureDistanceParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignMeasureDistanceTest::RunTest(const FString& Parameters)
{
	// 1. Missing 'from' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> ToArr;
		ToArr.Add(MakeShared<FJsonValueNumber>(0));
		ToArr.Add(MakeShared<FJsonValueNumber>(0));
		ToArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("to"), ToArr);

		FMonolithActionResult Result = ExecuteMeasureDistanceAction(Params);
		TestFalse(TEXT("Missing from should fail"), Result.bSuccess);
	}

	// 2. Missing 'to' param
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> FromArr;
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("from"), FromArr);

		FMonolithActionResult Result = ExecuteMeasureDistanceAction(Params);
		TestFalse(TEXT("Missing to should fail"), Result.bSuccess);
	}

    // 3. Valid params: array to array
    {
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> FromArr;
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		FromArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("from"), FromArr);

        TArray<TSharedPtr<FJsonValue>> ToArr;
		ToArr.Add(MakeShared<FJsonValueNumber>(100));
		ToArr.Add(MakeShared<FJsonValueNumber>(0));
		ToArr.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("to"), ToArr);

        FMonolithActionResult Result = ExecuteMeasureDistanceAction(Params);
		TestTrue(TEXT("Valid from/to arrays should succeed"), Result.bSuccess);
        if (Result.bSuccess && Result.ResultData.IsValid())
        {
            double Dist = 0.0;
            Result.ResultData->TryGetNumberField(TEXT("euclidean_distance"), Dist);
            TestEqual(TEXT("Distance should be 100"), Dist, 100.0);
        }
    }

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
