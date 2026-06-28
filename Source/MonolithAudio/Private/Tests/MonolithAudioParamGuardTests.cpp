#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithAudioAssetActions.h"
#include "MonolithAudioSoundCueActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_METASOUND
#include "MonolithAudioMetaSoundActions.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	void RegisterAudioParamGuardActions(FMonolithToolRegistry& Registry)
	{
		FMonolithAudioAssetActions::RegisterActions(Registry);
		FMonolithAudioSoundCueActions::RegisterActions(Registry);
#if WITH_METASOUND
		FMonolithAudioMetaSoundActions::RegisterActions(Registry);
#endif
	}

	FMonolithActionResult ExecuteAudioAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("audio"), Action))
		{
			RegisterAudioParamGuardActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("audio"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioApplyTemplateRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.ApplyAudioTemplateRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioApplyTemplateRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	AssetsArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Audio/SW_Test")));
	Params->SetArrayField(TEXT("asset_paths"), AssetsArray);

	TSharedPtr<FJsonObject> Template = MakeShared<FJsonObject>();
	Template->SetNumberField(TEXT("sound_class"), 123); // Should be string
	Params->SetObjectField(TEXT("template"), Template);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("apply_audio_template"), Params);

	TestTrue(TEXT("ApplyAudioTemplate with malformed sound_class should return Error"), !Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.ModifySoundSubmixRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSubmix"));

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("output_volume_db"), TEXT("malformed")); // Should be number
	Params->SetObjectField(TEXT("properties"), Props);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("modify_sound_submix"), Params);

	TestTrue(TEXT("ModifySoundSubmix with malformed property should return Error"), !Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateSoundMixRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateSoundMixRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateSoundMixRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSoundMix1"));
	Params->SetStringField(TEXT("initial_delay"), TEXT("malformed")); // Should be number

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_sound_mix"), Params);
	TestTrue(TEXT("CreateSoundMix with malformed initial_delay should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateSoundMix reports malformed initial_delay"), Result.ErrorMessage.Contains(TEXT("initial_delay must be a number")));

	TSharedPtr<FJsonObject> Params2 = MakeShared<FJsonObject>();
	Params2->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSoundMix2"));
	TArray<TSharedPtr<FJsonValue>> ClassEffects;
	TSharedPtr<FJsonObject> EffectObj = MakeShared<FJsonObject>();
	EffectObj->SetStringField(TEXT("SoundClass"), TEXT("None"));
	EffectObj->SetStringField(TEXT("bApplyToChildren"), TEXT("malformed")); // Should be bool
	ClassEffects.Add(MakeShared<FJsonValueObject>(EffectObj));
	Params2->SetArrayField(TEXT("class_effects"), ClassEffects);

	FMonolithActionResult Result2 = ExecuteAudioAction(TEXT("create_sound_mix"), Params2);
	TestTrue(TEXT("CreateSoundMix with malformed bApplyToChildren should return Error"), !Result2.bSuccess);
	TestTrue(TEXT("CreateSoundMix reports malformed class_effects error"), Result2.ErrorMessage.Contains(TEXT("bApplyToChildren must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioSetSoundMixSettingsRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.SetSoundMixSettingsRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioSetSoundMixSettingsRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSoundMixForEdit"));
	ExecuteAudioAction(TEXT("create_sound_mix"), CreateParams);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SM_TestSoundMixForEdit"));
	TArray<TSharedPtr<FJsonValue>> ClassEffects;
	TSharedPtr<FJsonObject> EffectObj = MakeShared<FJsonObject>();
	EffectObj->SetStringField(TEXT("SoundClass"), TEXT("None"));
	EffectObj->SetStringField(TEXT("bApplyToChildren"), TEXT("malformed")); // Should be bool
	ClassEffects.Add(MakeShared<FJsonValueObject>(EffectObj));
	Params->SetArrayField(TEXT("class_effects"), ClassEffects);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("set_sound_mix_settings"), Params);
	TestTrue(TEXT("SetSoundMixSettings with malformed bApplyToChildren should return Error"), !Result.bSuccess);
	TestTrue(TEXT("SetSoundMixSettings reports malformed class_effects error"), Result.ErrorMessage.Contains(TEXT("bApplyToChildren must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateTestWaveRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateTestWaveRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateTestWaveRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SW_TestTestWave"));
	Params->SetStringField(TEXT("frequency_hz"), TEXT("malformed")); // Should be number

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_test_wave"), Params);
	TestTrue(TEXT("CreateTestWave with malformed property should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateTestWave reports malformed property"), Result.ErrorMessage.Contains(TEXT("frequency_hz must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBuildSoundCueRejectsMalformedSpecTest, "Monolith.ParamGuard.Audio.BuildSoundCueRejectsMalformedSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBuildSoundCueRejectsMalformedSpecTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedSpec"));

		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		// Missing "type", "id" is wrong type
		NodeObj->SetNumberField(TEXT("id"), 123);
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		Spec->SetArrayField(TEXT("nodes"), NodesArray);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_sound_cue_from_spec"), Params);
		TestTrue(TEXT("BuildSoundCueFromSpec with malformed nodes should return Error"), !Result.bSuccess);
	}

	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedFirstNode"));

		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("id"), TEXT("root"));
		NodeObj->SetStringField(TEXT("type"), TEXT("Oscillator"));
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		Spec->SetArrayField(TEXT("nodes"), NodesArray);
		Spec->SetNumberField(TEXT("first_node"), 123);
		Params->SetObjectField(TEXT("spec"), Spec);

		FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_sound_cue_from_spec"), Params);
		TestTrue(TEXT("BuildSoundCueFromSpec with malformed first_node should return Error"), !Result.bSuccess);
		TestTrue(TEXT("BuildSoundCueFromSpec reports malformed first_node"), Result.ErrorMessage.Contains(TEXT("first_node must be a non-empty string")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateDistanceCrossfadeCueRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateDistanceCrossfadeCueRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateDistanceCrossfadeCueRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestCrossfade"));

	TArray<TSharedPtr<FJsonValue>> BandsArray;
	TSharedPtr<FJsonObject> BandObj = MakeShared<FJsonObject>();
	BandObj->SetNumberField(TEXT("sound_wave"), 123); // Should be string
	BandsArray.Add(MakeShared<FJsonValueObject>(BandObj));
	Params->SetArrayField(TEXT("bands"), BandsArray);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_distance_crossfade_cue"), Params);

	TestTrue(TEXT("CreateDistanceCrossfadeCue with malformed band sound_wave should return Error"), !Result.bSuccess);

	return true;
}

#if WITH_METASOUND

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateMetaSoundSourceRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateMetaSoundSourceRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateMetaSoundSourceRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/MS_TestRejects"));
	Params->SetStringField(TEXT("one_shot"), TEXT("malformed")); // Should be bool

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_metasound_source"), Params);

	TestTrue(TEXT("CreateMetaSoundSource with malformed one_shot should return Error"), !Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBuildMetaSoundRejectsMalformedSpecTest, "Monolith.ParamGuard.Audio.BuildMetaSoundRejectsMalformedSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBuildMetaSoundRejectsMalformedSpecTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/MS_TestRejectsSpec"));

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	Spec->SetStringField(TEXT("type"), TEXT("Source"));
	Spec->SetStringField(TEXT("one_shot"), TEXT("malformed")); // Should be bool
	Params->SetObjectField(TEXT("spec"), Spec);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_metasound_from_spec"), Params);
	TestTrue(TEXT("BuildMetaSoundFromSpec with malformed one_shot should return Error"), !Result.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBuildMetaSoundRejectsMalformedStrictModeTest, "Monolith.ParamGuard.Audio.BuildMetaSoundRejectsMalformedStrictMode", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBuildMetaSoundRejectsMalformedStrictModeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/MS_TestRejectsStrictMode"));
	Params->SetStringField(TEXT("strict_mode"), TEXT("malformed")); // Should be bool

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	Spec->SetStringField(TEXT("type"), TEXT("Source"));
	Params->SetObjectField(TEXT("spec"), Spec);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_metasound_from_spec"), Params);
	TestTrue(TEXT("BuildMetaSoundFromSpec with malformed strict_mode should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BuildMetaSoundFromSpec reports malformed strict_mode"), Result.ErrorMessage.Contains(TEXT("strict_mode must be a boolean")));

	return true;
}

#endif // WITH_METASOUND

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBuildSoundCueRejectsMalformedPropsTest, "Monolith.ParamGuard.Audio.BuildSoundCueRejectsMalformedProps", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBuildSoundCueRejectsMalformedPropsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedProps"));

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("id"), TEXT("root"));
	NodeObj->SetStringField(TEXT("type"), TEXT("Oscillator"));
	NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	Spec->SetArrayField(TEXT("nodes"), NodesArray);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("bPrimeOnLoad"), TEXT("malformed")); // Should be bool
	Spec->SetObjectField(TEXT("properties"), Props);
	Params->SetObjectField(TEXT("spec"), Spec);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_sound_cue_from_spec"), Params);
	TestTrue(TEXT("BuildSoundCueFromSpec with malformed property should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BuildSoundCueFromSpec reports malformed property"), Result.ErrorMessage.Contains(TEXT("bPrimeOnLoad must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBuildSoundCueRejectsMalformedAttenuationTest, "Monolith.ParamGuard.Audio.BuildSoundCueRejectsMalformedAttenuation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBuildSoundCueRejectsMalformedAttenuationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedAttenuation"));

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	NodeObj->SetStringField(TEXT("id"), TEXT("root"));
	NodeObj->SetStringField(TEXT("type"), TEXT("Oscillator"));
	NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	Spec->SetArrayField(TEXT("nodes"), NodesArray);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("bOverrideAttenuation"), TEXT("malformed")); // Should be bool
	Spec->SetObjectField(TEXT("properties"), Props);
	Params->SetObjectField(TEXT("spec"), Spec);

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("build_sound_cue_from_spec"), Params);
	TestTrue(TEXT("BuildSoundCueFromSpec with malformed property should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BuildSoundCueFromSpec reports malformed property"), Result.ErrorMessage.Contains(TEXT("bOverrideAttenuation must be a boolean")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateSoundCueRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateSoundCueRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateSoundCueRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformed"));
	Params->SetStringField(TEXT("sound_waves"), TEXT("malformed")); // Should be array

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_sound_cue"), Params);
	TestTrue(TEXT("CreateSoundCue with malformed sound_waves should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateSoundCue reports malformed sound_waves"), Result.ErrorMessage.Contains(TEXT("must be an array")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateLoopingAmbientCueRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.CreateLoopingAmbientCueRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateLoopingAmbientCueRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedAmbient"));
	TArray<TSharedPtr<FJsonValue>> WavesArray;
	WavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Audio/SW_Test")));
	Params->SetArrayField(TEXT("sound_waves"), WavesArray);

	Params->SetStringField(TEXT("delay_min"), TEXT("malformed")); // Should be number

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_looping_ambient_cue"), Params);
	TestTrue(TEXT("CreateLoopingAmbientCue with malformed delay_min should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateLoopingAmbientCue reports malformed delay_min"), Result.ErrorMessage.Contains(TEXT("must be a number")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateRandomSoundCueRejectsMalformedWeightsTest, "Monolith.ParamGuard.Audio.CreateRandomSoundCueRejectsMalformedWeights", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateRandomSoundCueRejectsMalformedWeightsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedRandom"));
	TArray<TSharedPtr<FJsonValue>> WavesArray;
	WavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Audio/SW_Test")));
	Params->SetArrayField(TEXT("sound_waves"), WavesArray);
	Params->SetStringField(TEXT("weights"), TEXT("malformed")); // Should be array

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_random_sound_cue"), Params);
	TestTrue(TEXT("CreateRandomSoundCue with malformed weights should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateRandomSoundCue reports malformed weights"), Result.ErrorMessage.Contains(TEXT("weights must be an array")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioCreateLayeredSoundCueRejectsMalformedVolumesTest, "Monolith.ParamGuard.Audio.CreateLayeredSoundCueRejectsMalformedVolumes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioCreateLayeredSoundCueRejectsMalformedVolumesTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SC_TestMalformedLayered"));
	TArray<TSharedPtr<FJsonValue>> WavesArray;
	WavesArray.Add(MakeShared<FJsonValueString>(TEXT("/Game/Audio/SW_Test")));
	Params->SetArrayField(TEXT("sound_waves"), WavesArray);
	Params->SetStringField(TEXT("volumes"), TEXT("malformed")); // Should be array

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("create_layered_sound_cue"), Params);
	TestTrue(TEXT("CreateLayeredSoundCue with malformed volumes should return Error"), !Result.bSuccess);
	TestTrue(TEXT("CreateLayeredSoundCue reports malformed volumes"), Result.ErrorMessage.Contains(TEXT("volumes must be an array")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithParamGuardAudioBindSoundToPerceptionRejectsMalformedParamsTest, "Monolith.ParamGuard.Audio.BindSoundToPerceptionRejectsMalformedParams", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithParamGuardAudioBindSoundToPerceptionRejectsMalformedParamsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("asset_path"), 12345); // Should be string

	FMonolithActionResult Result = ExecuteAudioAction(TEXT("bind_sound_to_perception"), Params);
	TestTrue(TEXT("BindSoundToPerception with malformed asset_path should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BindSoundToPerception reports malformed asset_path"), Result.ErrorMessage.Contains(TEXT("asset_path must be a string")));

	// Test negative loudness
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SW_Test"));
	Params->SetNumberField(TEXT("loudness"), -1.0);
	Result = ExecuteAudioAction(TEXT("bind_sound_to_perception"), Params);
	TestTrue(TEXT("BindSoundToPerception with negative loudness should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BindSoundToPerception reports negative loudness"), Result.ErrorMessage.Contains(TEXT("loudness must be >= 0")));

	// Test negative max_range
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SW_Test"));
	Params->SetNumberField(TEXT("max_range"), -500.0);
	Result = ExecuteAudioAction(TEXT("bind_sound_to_perception"), Params);
	TestTrue(TEXT("BindSoundToPerception with negative max_range should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BindSoundToPerception reports negative max_range"), Result.ErrorMessage.Contains(TEXT("max_range must be >= 0")));

	// Test overlength tag
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SW_Test"));
	FString LongTag;
	for (int i = 0; i < 260; ++i) LongTag += TEXT("A");
	Params->SetStringField(TEXT("tag"), LongTag);
	Result = ExecuteAudioAction(TEXT("bind_sound_to_perception"), Params);
	TestTrue(TEXT("BindSoundToPerception with overlength tag should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BindSoundToPerception reports overlength tag"), Result.ErrorMessage.Contains(TEXT("tag exceeds 255 characters")));

	// Test unsupported sense_class
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Audio/SW_Test"));
	Params->SetStringField(TEXT("sense_class"), TEXT("Sight"));
	Result = ExecuteAudioAction(TEXT("bind_sound_to_perception"), Params);
	TestTrue(TEXT("BindSoundToPerception with unsupported sense_class should return Error"), !Result.bSuccess);
	TestTrue(TEXT("BindSoundToPerception reports unsupported sense_class"), Result.ErrorMessage.Contains(TEXT("Unsupported sense_class")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
