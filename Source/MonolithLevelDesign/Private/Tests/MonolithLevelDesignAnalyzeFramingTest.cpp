#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignQualityActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAnalyzeFramingQualityAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignQualityActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzeFramingTest, "Monolith.Sentinel.LevelDesign.AnalyzeFramingParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzeFramingTest::RunTest(const FString& Parameters)
{
	// 1. Missing camera_location
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidRot;
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidRot.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("camera_rotation"), ValidRot);

		FMonolithActionResult Result = ExecuteAnalyzeFramingQualityAction(TEXT("analyze_framing"), Params);
		TestFalse(TEXT("Missing camera_location should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention camera_location"), Result.ErrorMessage.Contains(TEXT("camera_location")));
	}

	// 2. Missing camera_rotation
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("camera_location"), ValidLoc);

		FMonolithActionResult Result = ExecuteAnalyzeFramingQualityAction(TEXT("analyze_framing"), Params);
		TestFalse(TEXT("Missing camera_rotation should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention camera_rotation"), Result.ErrorMessage.Contains(TEXT("camera_rotation")));
	}

	// 3. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLocRot;
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		ValidLocRot.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("camera_location"), ValidLocRot);
		Params->SetArrayField(TEXT("camera_rotation"), ValidLocRot);

		FMonolithActionResult Result = ExecuteAnalyzeFramingQualityAction(TEXT("analyze_framing"), Params);
		bool bIsExpectedResult =
			Result.bSuccess ||
			Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
