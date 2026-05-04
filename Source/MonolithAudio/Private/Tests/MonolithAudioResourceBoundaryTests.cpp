#include "Misc/AutomationTest.h"
#include "MonolithAudioQueryActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioFindUnattenuatedSoundsLimitTest, "Monolith.LimitGuard.Audio.FindUnattenuatedSoundsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioFindUnattenuatedSoundsLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (100)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = FMonolithAudioQueryActions::FindUnattenuatedSounds(Params);
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			AddError(TEXT("Action failed without limit"));
			return false;
		}

		double CountVal = 0.0;
		if (!Result.Result->TryGetNumberField(TEXT("count"), CountVal))
		{
			AddError(TEXT("Missing count in result"));
		}

		if (CountVal > 100.0)
		{
			AddError(FString::Printf(TEXT("Default limit of 100 was not respected. Count was %f"), CountVal));
		}
	}

	// Test explicit limit
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5.0);
		FMonolithActionResult Result = FMonolithAudioQueryActions::FindUnattenuatedSounds(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("count"), CountVal))
		{
			if (CountVal > 5.0)
			{
				AddError(FString::Printf(TEXT("Explicit limit of 5 was not respected. Count was %f"), CountVal));
			}
		}
	}

	// Test zero/negative limit clamping
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), -10.0);
		FMonolithActionResult Result = FMonolithAudioQueryActions::FindUnattenuatedSounds(Params);

		double CountVal = -1.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("count"), CountVal))
		{
			if (CountVal != 0.0)
			{
				AddError(FString::Printf(TEXT("Negative limit was not clamped to 0. Count was %f"), CountVal));
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS