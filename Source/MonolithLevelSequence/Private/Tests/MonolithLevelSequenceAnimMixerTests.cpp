#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "MonolithLevelSequenceActions.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelSequenceAnimMixerStatusTest,
	"Monolith.LevelSequence.AnimMixer.StatusShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceAnimMixerStatusTest::RunTest(const FString& Parameters)
{
	const FMonolithActionResult Result = FMonolithLevelSequenceActions::GetAnimMixerStatus(MakeShared<FJsonObject>());

	TestTrue(TEXT("Status probe should succeed even when MovieSceneAnimMixer is absent"), Result.bSuccess);
	TestTrue(TEXT("Status probe should return JSON"), Result.Result.IsValid());
	if (Result.Result.IsValid())
	{
		TestTrue(TEXT("Result should expose plugin_available"), Result.Result->HasField(TEXT("plugin_available")));
		TestTrue(TEXT("Result should expose plugin_enabled"), Result.Result->HasField(TEXT("plugin_enabled")));
		TestTrue(TEXT("Result should expose modules"), Result.Result->HasTypedField<EJson::Object>(TEXT("modules")));
		TestTrue(TEXT("Result should expose classes"), Result.Result->HasTypedField<EJson::Object>(TEXT("classes")));
		TestFalse(TEXT("Anim Mixer support must remain reflection-only"), Result.Result->GetBoolField(TEXT("hard_dependency")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithLevelSequenceAnimMixerMissingAssetPathTest,
	"Monolith.ParamGuard.LevelSequence.ListAnimMixerTracksRequiresAssetPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLevelSequenceAnimMixerMissingAssetPathTest::RunTest(const FString& Parameters)
{
	const FMonolithActionResult Result = FMonolithLevelSequenceActions::ListAnimMixerTracks(MakeShared<FJsonObject>());

	TestFalse(TEXT("Missing asset_path should be rejected"), Result.bSuccess);
	TestTrue(TEXT("Error should name asset_path"), Result.ErrorMessage.Contains(TEXT("asset_path")));
	return true;
}
