#include "Misc/AutomationTest.h"
#include "MonolithAudioQueryActions.h"
#include "MonolithAudioMetaSoundActions.h"
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

FMonolithActionResult ExecuteFindSoundsWithoutClass(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioQueryActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("find_sounds_without_class"), Params);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioFindSoundsWithoutClassLimitTest, "Monolith.LimitGuard.Audio.FindSoundsWithoutClassClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioFindSoundsWithoutClassLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (100)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteFindSoundsWithoutClass(Params);
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
		FMonolithActionResult Result = ExecuteFindSoundsWithoutClass(Params);

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
		FMonolithActionResult Result = ExecuteFindSoundsWithoutClass(Params);

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
		FMonolithActionResult Result = ExecuteFindSoundsWithoutClass(Params);

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
		FMonolithActionResult Result = ExecuteFindSoundsWithoutClass(Params);
		TestFalse(TEXT("String limit should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention limit"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	return true;
}


namespace
{
FMonolithActionResult ExecuteListAudioAssets(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioQueryActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("list_audio_assets"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioListAudioAssetsLimitTest, "Monolith.LimitGuard.Audio.ListAudioAssetsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioListAudioAssetsLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (100)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("type"), TEXT("SoundWave"));
		FMonolithActionResult Result = ExecuteListAudioAssets(Params);
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
		Params->SetStringField(TEXT("type"), TEXT("SoundWave"));
		Params->SetNumberField(TEXT("limit"), 5.0);
		FMonolithActionResult Result = ExecuteListAudioAssets(Params);

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
		Params->SetStringField(TEXT("type"), TEXT("SoundWave"));
		Params->SetNumberField(TEXT("limit"), -10.0);
		FMonolithActionResult Result = ExecuteListAudioAssets(Params);

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
		Params->SetStringField(TEXT("type"), TEXT("SoundWave"));
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteListAudioAssets(Params);

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
		Params->SetStringField(TEXT("type"), TEXT("SoundWave"));
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = ExecuteListAudioAssets(Params);
		TestFalse(TEXT("String limit should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention limit"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	return true;
}




namespace
{
FMonolithActionResult ExecuteListAvailableMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("list_available_metasound_nodes"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioListAvailableMetaSoundNodesLimitTest, "Monolith.LimitGuard.Audio.ListAvailableMetaSoundNodesClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioListAvailableMetaSoundNodesLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (200)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteListAvailableMetaSoundNodes(Params);
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			AddError(TEXT("Action failed without limit"));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (Result.Result->TryGetArrayField(TEXT("nodes"), NodesArray))
		{
			if (NodesArray->Num() > 200)
			{
				AddError(FString::Printf(TEXT("Default limit of 200 was not respected. Count was %d"), NodesArray->Num()));
			}
		}
		else
		{
			AddError(TEXT("Missing nodes array in result"));
		}
	}

	// Test explicit limit
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5.0);
		FMonolithActionResult Result = ExecuteListAvailableMetaSoundNodes(Params);

		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("nodes"), NodesArray))
			{
				if (NodesArray->Num() > 5)
				{
					AddError(FString::Printf(TEXT("Explicit limit of 5 was not respected. Count was %d"), NodesArray->Num()));
				}
			}
		}
	}

	// Test zero/negative limit clamping
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), -10.0);
		FMonolithActionResult Result = ExecuteListAvailableMetaSoundNodes(Params);

		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("nodes"), NodesArray))
			{
				if (NodesArray->Num() != 1 && NodesArray->Num() != 0) // clamped to 1, but maybe no nodes found
				{
					// the test is just ensuring we clamped correctly
					// note: some nodes might not exist so Num could be 0, but it should not be > 1
					if (NodesArray->Num() > 1)
					{
						AddError(FString::Printf(TEXT("Negative limit was not clamped to 1. Count was %d"), NodesArray->Num()));
					}
				}
			}
		}
	}

	// Test very large limit upper bound
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteListAvailableMetaSoundNodes(Params);

		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("nodes"), NodesArray))
			{
				if (NodesArray->Num() > 1000)
				{
					AddError(FString::Printf(TEXT("Huge limit was not clamped to 1000. Count was %d"), NodesArray->Num()));
				}
			}
		}
	}

	// Test malformed limit type (TryGetNumberField fails and leaves default intact)
	// (or returns error depending on how we handle it - the current code just ignores and uses default)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = ExecuteListAvailableMetaSoundNodes(Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("nodes"), NodesArray))
			{
				if (NodesArray->Num() > 200)
				{
					AddError(FString::Printf(TEXT("Malformed limit was not ignored/defaulted to 200. Count was %d"), NodesArray->Num()));
				}
			}
		}
	}

	return true;
}


namespace
{
FMonolithActionResult ExecuteCreateRandomSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("create_random_sound_cue"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioCreateWavePlayerNodesLimitTest, "Monolith.LimitGuard.Audio.CreateWavePlayerNodesClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioCreateWavePlayerNodesLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitCue"));

		TArray<TSharedPtr<FJsonValue>> OversizedWavesArray;
		for (int32 i = 0; i < 101; ++i)
		{
			OversizedWavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Temp/SomeWave")));
		}
		Params->SetArrayField(TEXT("sound_waves"), OversizedWavesArray);

		FMonolithActionResult Result = ExecuteCreateRandomSoundCue(Params);

		TestFalse(TEXT("Oversized wave array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
