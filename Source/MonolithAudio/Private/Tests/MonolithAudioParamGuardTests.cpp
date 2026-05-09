#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithAudioAssetActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAudioAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("audio"), Action))
		{
			FMonolithAudioAssetActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("audio"), Action, Params);
	}
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

#endif // WITH_DEV_AUTOMATION_TESTS
