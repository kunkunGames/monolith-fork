#include "Misc/AutomationTest.h"
#include "MonolithAudioQueryActions.h"
#if WITH_METASOUND
#include "MonolithAudioMetaSoundActions.h"
#endif
#include "MonolithAudioSoundCueActions.h"
#include "MonolithAudioBatchActions.h"
#include "MonolithAudioPerceptionActions.h"
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

FMonolithActionResult ExecuteFindUnusedAudio(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioQueryActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("find_unused_audio"), Params);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioFindUnusedAudioLimitTest, "Monolith.LimitGuard.Audio.FindUnusedAudioClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioFindUnusedAudioLimitTest::RunTest(const FString& Parameters)
{
	// Test default limit (100)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteFindUnusedAudio(Params);
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			AddError(TEXT("Action failed without limit"));
			return false;
		}

		double CountVal = 0.0;
		if (!Result.Result->TryGetNumberField(TEXT("unused_count"), CountVal))
		{
			AddError(TEXT("Missing unused_count in result"));
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
		FMonolithActionResult Result = ExecuteFindUnusedAudio(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("unused_count"), CountVal))
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
		FMonolithActionResult Result = ExecuteFindUnusedAudio(Params);

		double CountVal = -1.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("unused_count"), CountVal))
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
		FMonolithActionResult Result = ExecuteFindUnusedAudio(Params);

		double CountVal = 0.0;
		if (Result.Result.IsValid() && Result.Result->TryGetNumberField(TEXT("unused_count"), CountVal))
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
		FMonolithActionResult Result = ExecuteFindUnusedAudio(Params);
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




#if WITH_METASOUND
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
#endif // WITH_METASOUND


namespace
{
FMonolithActionResult ExecuteBatchAssignSoundClass(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioBatchActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("batch_assign_sound_class"), Params);
}

FMonolithActionResult ExecuteCreateRandomSoundCue(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("create_random_sound_cue"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioBatchActionsLimitTest, "Monolith.LimitGuard.Audio.BatchActionsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioBatchActionsLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sound_class"), TEXT("/Game/Temp/SomeClass"));

		TArray<TSharedPtr<FJsonValue>> OversizedWavesArray;
		for (int32 i = 0; i < 201; ++i)
		{
			OversizedWavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Temp/SomeWave")));
		}
		Params->SetArrayField(TEXT("asset_paths"), OversizedWavesArray);

		FMonolithActionResult Result = ExecuteBatchAssignSoundClass(Params);

		TestFalse(TEXT("Oversized wave array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioCreateDistanceCrossfadeCueLimitTest, "Monolith.LimitGuard.Audio.CreateDistanceCrossfadeCueClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioCreateDistanceCrossfadeCueLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitDistanceCue"));

		TArray<TSharedPtr<FJsonValue>> OversizedBandsArray;
		for (int32 i = 0; i < 101; ++i)
		{
			TSharedPtr<FJsonObject> BandObj = MakeShared<FJsonObject>();
			BandObj->SetStringField(TEXT("sound_wave"), TEXT("/Game/Temp/SomeWave"));
			OversizedBandsArray.Add(MakeShared<FJsonValueObject>(BandObj));
		}
		Params->SetArrayField(TEXT("bands"), OversizedBandsArray);

		FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("create_distance_crossfade_cue"), Params);

		TestFalse(TEXT("Oversized bands array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
}

#if WITH_METASOUND
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioBuildMetaSoundFromSpecLimitTest, "Monolith.LimitGuard.Audio.BuildMetaSoundFromSpecClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioBuildMetaSoundFromSpecLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized nodes array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitSpecMetaSound"));

		TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OversizedNodesArray;
		for (int32 i = 0; i < 501; ++i)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			OversizedNodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		SpecObj->SetArrayField(TEXT("nodes"), OversizedNodesArray);
		Params->SetObjectField(TEXT("spec"), SpecObj);

		FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("build_metasound_from_spec"), Params);

		TestFalse(TEXT("Oversized nodes array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	// Test oversized connections array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitSpecMetaSound"));

		TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OversizedConnsArray;
		for (int32 i = 0; i < 1001; ++i)
		{
			TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
			OversizedConnsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
		}
		SpecObj->SetArrayField(TEXT("connections"), OversizedConnsArray);
		Params->SetObjectField(TEXT("spec"), SpecObj);

		FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("build_metasound_from_spec"), Params);

		TestFalse(TEXT("Oversized connections array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	// Test oversized graph_input_connections array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitSpecMetaSound"));

		TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OversizedGraphInputConnsArray;
		for (int32 i = 0; i < 1001; ++i)
		{
			TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
			OversizedGraphInputConnsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
		}
		SpecObj->SetArrayField(TEXT("graph_input_connections"), OversizedGraphInputConnsArray);
		Params->SetObjectField(TEXT("spec"), SpecObj);

		FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("build_metasound_from_spec"), Params);

		TestFalse(TEXT("Oversized graph_input_connections array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioCreateInteractiveMetaSoundLimitTest, "Monolith.LimitGuard.Audio.CreateInteractiveMetaSoundClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioCreateInteractiveMetaSoundLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitMetaSound"));

		TArray<TSharedPtr<FJsonValue>> OversizedWavesArray;
		for (int32 i = 0; i < 101; ++i)
		{
			OversizedWavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Temp/SomeWave")));
		}
		Params->SetArrayField(TEXT("sound_waves"), OversizedWavesArray);

		FMonolithAudioMetaSoundActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("create_interactive_metasound"), Params);

		TestFalse(TEXT("Oversized wave array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
}
#endif // WITH_METASOUND

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioBuildSoundCueFromSpecLimitTest, "Monolith.LimitGuard.Audio.BuildSoundCueFromSpecClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioBuildSoundCueFromSpecLimitTest::RunTest(const FString& Parameters)
{
	// Test oversized nodes array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitSpecCue"));

		TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OversizedNodesArray;
		for (int32 i = 0; i < 501; ++i)
		{
			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			OversizedNodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
		SpecObj->SetArrayField(TEXT("nodes"), OversizedNodesArray);
		Params->SetObjectField(TEXT("spec"), SpecObj);

		FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("build_sound_cue_from_spec"), Params);

		TestFalse(TEXT("Oversized nodes array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	// Test oversized connections array is rejected
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Temp/TestLimitSpecCue"));

		TSharedPtr<FJsonObject> SpecObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> OversizedConnsArray;
		for (int32 i = 0; i < 1001; ++i)
		{
			TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
			OversizedConnsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
		}
		SpecObj->SetArrayField(TEXT("connections"), OversizedConnsArray);
		Params->SetObjectField(TEXT("spec"), SpecObj);

		FMonolithAudioSoundCueActions::RegisterActions(FMonolithToolRegistry::Get());
		FMonolithActionResult Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("build_sound_cue_from_spec"), Params);

		TestFalse(TEXT("Oversized connections array should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention maximum allowed"), Result.ErrorMessage.Contains(TEXT("exceeds the maximum allowed")));
	}

	return true;
}

namespace
{
FMonolithActionResult ExecuteListPerceptionBoundSounds(const TSharedPtr<FJsonObject>& Params)
{
	FMonolithAudioPerceptionActions::RegisterActions(FMonolithToolRegistry::Get());
	return FMonolithToolRegistry::Get().ExecuteAction(TEXT("audio"), TEXT("list_perception_bound_sounds"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioListPerceptionBoundSoundsLimitTest, "Monolith.LimitGuard.Audio.ListPerceptionBoundSoundsClampsLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioListPerceptionBoundSoundsLimitTest::RunTest(const FString& Parameters)
{
	// Test malformed limit type
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("limit"), TEXT("not_a_number"));
		FMonolithActionResult Result = ExecuteListPerceptionBoundSounds(Params);
		TestFalse(TEXT("String limit should return an error"), Result.bSuccess);
		TestTrue(TEXT("Error should mention limit"), Result.ErrorMessage.Contains(TEXT("limit")));
	}

	// Test omitted value uses existing default
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		FMonolithActionResult Result = ExecuteListPerceptionBoundSounds(Params);
		if (!Result.bSuccess || !Result.Result.IsValid())
		{
			AddError(TEXT("Action failed without limit"));
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("bindings"), BindingsArray))
			{
				if (BindingsArray->Num() > 1000)
				{
					AddError(FString::Printf(TEXT("Default limit of 1000 was not respected. Count was %d"), BindingsArray->Num()));
				}
			}
		}
	}

	// Test normal value is preserved
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 5.0);
		FMonolithActionResult Result = ExecuteListPerceptionBoundSounds(Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("bindings"), BindingsArray))
			{
				if (BindingsArray->Num() > 5)
				{
					AddError(FString::Printf(TEXT("Explicit limit of 5 was not respected. Count was %d"), BindingsArray->Num()));
				}
			}
		}
	}

	// Test negative value is handled (clamped to 0)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), -10.0);
		FMonolithActionResult Result = ExecuteListPerceptionBoundSounds(Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("bindings"), BindingsArray))
			{
				if (BindingsArray->Num() > 0)
				{
					AddError(FString::Printf(TEXT("Negative limit was not clamped to 0. Count was %d"), BindingsArray->Num()));
				}
			}
		}
	}

	// Test extreme value is clamped (to 1000)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), 1000000.0);
		FMonolithActionResult Result = ExecuteListPerceptionBoundSounds(Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* BindingsArray = nullptr;
			if (Result.Result->TryGetArrayField(TEXT("bindings"), BindingsArray))
			{
				if (BindingsArray->Num() > 1000)
				{
					AddError(FString::Printf(TEXT("Huge limit was not clamped to 1000. Count was %d"), BindingsArray->Num()));
				}
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
