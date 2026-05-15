#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithAudioAssetActions.h"
#include "MonolithAudioSoundCueActions.h"
#include "MonolithToolRegistry.h"
#include "Dom/JsonObject.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FMonolithActionResult ExecuteAudioSecurityAction(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("audio"), Action))
		{
			FMonolithAudioAssetActions::RegisterActions(Registry);
			FMonolithAudioSoundCueActions::RegisterActions(Registry);
		}

		return Registry.ExecuteAction(TEXT("audio"), Action, Params);
	}
}

// ---------------------------------------------------------------------------
// FMonolithAudioSoundCueActions::CreateSoundCue (Validates CreateEmptySoundCue path)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioSecuritySoundCueCreatePathTest, "Monolith.Security.MonolithAudio.CreateSoundCue.RejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioSecuritySoundCueCreatePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/Audio/SC_BadPathTest"), // Double leading slash
		TEXT("Game/Audio/SC_BadPathTest"), // Missing leading slash
		TEXT("/Game/Audio/SC_BadPathTest/"), // Trailing slash
		TEXT("/Game/Audio/SC_BadPathTest#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Path);

		FMonolithActionResult Result = ExecuteAudioSecurityAction(TEXT("create_sound_cue"), Params);

		TestFalse(*FString::Printf(TEXT("CreateSoundCue with malformed path '%s' should return Error"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path '%s'"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should mention invalid package path for '%s'"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")) ||
				Result.ErrorMessage.Contains(Path));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FMonolithAudioAssetActions::CreateSoundAttenuation (Validates CreateAudioAsset template path)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioSecurityAssetCreatePathTest, "Monolith.Security.MonolithAudio.CreateAudioAsset.RejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioSecurityAssetCreatePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/Audio/SA_BadPathTest"), // Double leading slash
		TEXT("Game/Audio/SA_BadPathTest"), // Missing leading slash
		TEXT("/Game/Audio/SA_BadPathTest/"), // Trailing slash
		TEXT("/Game/Audio/SA_BadPathTest#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Path);

		FMonolithActionResult Result = ExecuteAudioSecurityAction(TEXT("create_sound_attenuation"), Params);

		TestFalse(*FString::Printf(TEXT("CreateSoundAttenuation with malformed path '%s' should return Error"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path '%s'"), *Path), Result.ErrorMessage.IsEmpty());
		if (!Path.IsEmpty())
		{
			TestTrue(*FString::Printf(TEXT("Error should mention invalid package path for '%s'"), *Path),
				Result.ErrorMessage.Contains(TEXT("Invalid package path")) ||
				Result.ErrorMessage.Contains(TEXT("Package path")) ||
				Result.ErrorMessage.Contains(Path));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// FMonolithAudioAssetActions::CreateTestWave (Validates standalone CreatePackage in CreateTestWave)
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioSecurityTestWaveCreatePathTest, "Monolith.Security.MonolithAudio.CreateTestWave.RejectsMalformedPath", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithAudioSecurityTestWaveCreatePathTest::RunTest(const FString& Parameters)
{
	TArray<FString> MalformedPaths = {
		TEXT(""), // Empty path
		TEXT("//Game/Audio/SW_BadPathTest"), // Double leading slash
		TEXT("Game/Audio/SW_BadPathTest"), // Missing leading slash
		TEXT("/Game/Audio/SW_BadPathTest/"), // Trailing slash
		TEXT("/Game/Audio/SW_BadPathTest#Invalid") // Illegal characters
	};

	for (const FString& Path : MalformedPaths)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Path);
		Params->SetNumberField(TEXT("duration_seconds"), 1.0);
		Params->SetNumberField(TEXT("frequency_hz"), 440.0);

		FMonolithActionResult Result = ExecuteAudioSecurityAction(TEXT("create_test_wave"), Params);

		TestFalse(*FString::Printf(TEXT("CreateTestWave with malformed path '%s' should return Error"), *Path), Result.bSuccess);
		TestFalse(*FString::Printf(TEXT("Error should be populated for malformed path '%s'"), *Path), Result.ErrorMessage.IsEmpty());

		// It happens that CreateTestWave has a manual StartsWith check that we left in place, but
		// even if that were bypassed, the ValidatePackagePath would reject it.
		// We're mostly ensuring it fails safely without crashing.
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
