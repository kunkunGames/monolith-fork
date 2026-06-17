#include "CoreTypes.h"
#include "Misc/AutomationTest.h"
#include "MonolithAudioPerceptionStatics.h"
#include "MonolithSoundPerceptionUserData.h"
#include "Sound/SoundWave.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

// Note: A minimal smoke test scaffold for the perception statics without launching a live editor session.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioPerceptionStaticsTest, "Monolith.AudioRuntime.PerceptionStatics.NullSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioPerceptionStaticsTest::RunTest(const FString& Parameters)
{
	// Test early out with null sound to ensure it does not crash
	UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise(
		nullptr, // WorldContextObject
		nullptr, // Sound
		FVector::ZeroVector
	);

	// Test early out when sound is valid but has no UserData binding
	USoundWave* MockSound = NewObject<USoundWave>();
	if (!MockSound)
	{
		AddError(TEXT("Failed to create mock USoundWave"));
		return false;
	}

	UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise(
		nullptr, // WorldContextObject
		MockSound,
		FVector::ZeroVector
	);

	// Test early out when sound has UserData binding but it is disabled
	UMonolithSoundPerceptionUserData* UserData = NewObject<UMonolithSoundPerceptionUserData>(MockSound);
	if (UserData)
	{
		UserData->bEnabled = false;
		MockSound->AddAssetUserData(UserData);

		UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise(
			nullptr, // WorldContextObject
			MockSound,
			FVector::ZeroVector
		);
	}

	// Because we don't spin up a live PIE session, this serves as a structural validation
	// that the static function's guards safely reject invalid input gracefully.

	return true;
}
