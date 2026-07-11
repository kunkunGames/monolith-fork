#include "CoreTypes.h"
#include "Misc/AutomationTest.h"
#include "MonolithAudioPerceptionSubsystem.h"
#include "MonolithSoundPerceptionUserData.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWave.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "Perception/AISense_Hearing.h"

#if WITH_DEV_AUTOMATION_TESTS

// Note: A minimal smoke test scaffold for the perception subsystem without launching a live editor session.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioPerceptionSubsystemTest, "Monolith.AudioRuntime.PerceptionSubsystem", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioPerceptionSubsystemTest::RunTest(const FString& Parameters)
{
    // Simple instantiation check to ensure compilation and basic structural integrity
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

    UserData->bEnabled = true;
    UserData->Loudness = 2.0f;
    MockSound->AddAssetUserData(UserData);

    TestTrue(TEXT("UserData was successfully added"), MockSound->GetAssetUserDataOfClass(UMonolithSoundPerceptionUserData::StaticClass()) != nullptr);
    TestEqual(TEXT("Loudness is preserved"), Cast<UMonolithSoundPerceptionUserData>(MockSound->GetAssetUserDataOfClass(UMonolithSoundPerceptionUserData::StaticClass()))->Loudness, 2.0f);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
