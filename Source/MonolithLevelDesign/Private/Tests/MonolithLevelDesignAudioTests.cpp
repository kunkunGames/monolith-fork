#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"
#include "MonolithLevelDesignAudioActions.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAudioAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("leveldesign"), Action))
		{
			FMonolithLevelDesignAudioActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("leveldesign"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignEstimateFootstepSoundTest, "Monolith.Sentinel.LevelDesign.EstimateFootstepSoundParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignEstimateFootstepSoundTest::RunTest(const FString& Parameters)
{
	// 1. Missing location
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteAudioAction(TEXT("estimate_footstep_sound"), Params);
		TestFalse(TEXT("Missing location should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention location"), Result.ErrorMessage.Contains(TEXT("location")));
	}

	// 2. Wrong type location
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("location"), TEXT("NotAnArray"));
		FMonolithActionResult Result = ExecuteAudioAction(TEXT("estimate_footstep_sound"), Params);
		TestFalse(TEXT("Wrong type location should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention location"), Result.ErrorMessage.Contains(TEXT("location")));
	}

	// 3. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("location"), ValidLoc);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("estimate_footstep_sound"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzeRoomAcousticsTest, "Monolith.Sentinel.LevelDesign.AnalyzeRoomAcousticsParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzeRoomAcousticsTest::RunTest(const FString& Parameters)
{
	// 1. Missing volume_name
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteAudioAction(TEXT("analyze_room_acoustics"), Params);
		TestFalse(TEXT("Missing volume_name should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention volume_name"), Result.ErrorMessage.Contains(TEXT("volume_name")));
	}

	// 2. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("volume_name"), TEXT("TestVolume"));

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("analyze_room_acoustics"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLevelDesignAnalyzeSoundPropagationTest, "Monolith.Sentinel.LevelDesign.AnalyzeSoundPropagationParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelDesignAnalyzeSoundPropagationTest::RunTest(const FString& Parameters)
{
	// 1. Missing from
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("to"), ValidLoc);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("analyze_sound_propagation"), Params);
		TestFalse(TEXT("Missing from should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention from"), Result.ErrorMessage.Contains(TEXT("from")));
	}

	// 2. Missing to
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("from"), ValidLoc);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("analyze_sound_propagation"), Params);
		TestFalse(TEXT("Missing to should fail"), Result.bSuccess);
		TestTrue(TEXT("Error message should mention to"), Result.ErrorMessage.Contains(TEXT("to")));
	}

	// 3. Valid parameters
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ValidLoc;
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		ValidLoc.Add(MakeShared<FJsonValueNumber>(0));
		Params->SetArrayField(TEXT("from"), ValidLoc);
		Params->SetArrayField(TEXT("to"), ValidLoc);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("analyze_sound_propagation"), Params);
		bool bIsExpectedResult = Result.bSuccess || Result.ErrorMessage.Contains(TEXT("No editor world available"));
		TestTrue(TEXT("Valid params should succeed or fail cleanly on missing world"), bIsExpectedResult);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
