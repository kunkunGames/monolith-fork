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

#endif // WITH_DEV_AUTOMATION_TESTS
