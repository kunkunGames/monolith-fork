#include "CoreTypes.h"
#include "Misc/AutomationTest.h"
#include "MonolithSoundPerceptionUserData.h"
#include "Sound/SoundWave.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithSoundPerceptionUserDataTest, "Monolith.AudioRuntime.SoundPerceptionUserData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithSoundPerceptionUserDataTest::RunTest(const FString& Parameters)
{
	USoundWave* MockSound = NewObject<USoundWave>();
	if (!MockSound)
	{
		AddError(TEXT("Failed to create mock USoundWave"));
		return false;
	}

	UMonolithSoundPerceptionUserData* UserData = NewObject<UMonolithSoundPerceptionUserData>(MockSound);
	if (!UserData)
	{
		AddError(TEXT("Failed to create UserData"));
		return false;
	}

	// Test default values
	TestTrue(TEXT("bEnabled should default to true"), UserData->bEnabled);
	TestEqual(TEXT("Loudness should default to 1.0f"), UserData->Loudness, 1.0f);
	TestEqual(TEXT("MaxRange should default to 0.0f"), UserData->MaxRange, 0.0f);
	TestTrue(TEXT("Tag should default to NAME_None"), UserData->Tag == NAME_None);
	TestTrue(TEXT("SenseClass should default to null"), UserData->SenseClass == nullptr);
	TestTrue(TEXT("bFireOnFadeIn should default to true"), UserData->bFireOnFadeIn);
	TestTrue(TEXT("bRequireOwningActor should default to true"), UserData->bRequireOwningActor);

	// Test assignment
	UserData->bEnabled = false;
	UserData->Loudness = 2.5f;
	UserData->MaxRange = 1000.0f;
	UserData->Tag = FName("TestTag");
	UserData->bFireOnFadeIn = false;
	UserData->bRequireOwningActor = false;

	TestFalse(TEXT("bEnabled should be false"), UserData->bEnabled);
	TestEqual(TEXT("Loudness should be 2.5f"), UserData->Loudness, 2.5f);
	TestEqual(TEXT("MaxRange should be 1000.0f"), UserData->MaxRange, 1000.0f);
	TestTrue(TEXT("Tag should be TestTag"), UserData->Tag == FName("TestTag"));
	TestFalse(TEXT("bFireOnFadeIn should be false"), UserData->bFireOnFadeIn);
	TestFalse(TEXT("bRequireOwningActor should be false"), UserData->bRequireOwningActor);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
