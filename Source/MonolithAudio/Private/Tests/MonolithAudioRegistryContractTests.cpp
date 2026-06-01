#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithAudioAssetActions.h"
#include "MonolithAudioSoundCueActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAudioRegistryAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("audio"), Action))
		{
			FMonolithAudioAssetActions::RegisterActions(Registry);
		}
		return Registry.ExecuteAction(TEXT("audio"), Action, Params);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithRegistryAudioCreateTestWaveRejectsAliasCollisionTest, "Monolith.Registry.Audio.CreateTestWaveRejectsAliasCollision", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithRegistryAudioCreateTestWaveRejectsAliasCollisionTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Audio/SW_Collision"));
	Params->SetStringField(TEXT("path"), TEXT("/Game/Tests/Audio/SW_CollisionAlias"));

	FMonolithActionResult Result = ExecuteAudioRegistryAction(TEXT("create_test_wave"), Params);

	TestFalse(TEXT("create_test_wave should fail when both asset_path and its alias path are provided"), Result.bSuccess);
	TestTrue(TEXT("Error message should mention alias collision"), Result.ErrorMessage.Contains(TEXT("collision")) || Result.ErrorMessage.Contains(TEXT("Cannot specify both")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
