#include "Misc/AutomationTest.h"
#include "MonolithAudioQueryActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FMonolithActionResult ExecuteFindUnattenuatedSounds(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioQueryActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("find_unattenuated_sounds"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioFindUnattenuatedSoundsLimitTest, "Monolith.LimitGuard.Audio.FindUnattenuatedSoundsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioFindUnattenuatedSoundsLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (100)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteFindUnattenuatedSounds(Params);
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
		FMonolithActionResult Result = ExecuteFindUnattenuatedSounds(Params);

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
		FMonolithActionResult Result = ExecuteFindUnattenuatedSounds(Params);

		double CountVal = -1.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("count"), CountVal))
		{
			if (CountVal != 0.0)
			{
				AddError(FString::Printf(TEXT("Negative limit was not clamped to 0. Count was %f"), CountVal));
			}
		}
	}

	// Test very large limit upper bound
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteFindUnattenuatedSounds(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("count"), CountVal))
		{
			if (CountVal > 1000.0)
			{
				AddError(FString::Printf(TEXT("Huge limit was not clamped to 1000. Count was %f"), CountVal));
			}
		}
	}

	// Test malformed limit type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = ExecuteFindUnattenuatedSounds(Params);
		TestFalse(TEXT("String limit should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention limit"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
